// Per-process network on Windows: the Microsoft-Windows-Kernel-Network ETW provider (what Task
// Manager's network column and Resource Monitor read), consumed in a real-time trace session of
// our own. Starting a session takes administrator rights (or the Performance Log Users group):
// without them the feature stays off and PC_CAP_PROCESS_NETWORK is clear, as with the private
// NetworkStatistics framework on macOS when it can't be loaded.
#if defined(_WIN32)

#include "windows_internal.hpp"

#include <evntcons.h>
#include <evntrace.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "advapi32.lib")

namespace procyon::platform {

using namespace win;

namespace {

// {7DD42A49-5329-4832-8DFD-43D979153A88}
const GUID kKernelNetwork = {0x7DD42A49, 0x5329, 0x4832, {0x8D, 0xFD, 0x43, 0xD9, 0x79, 0x15, 0x3A, 0x88}};
const wchar_t kSessionName[] = L"Procyon-Network";

// Event ids of the provider: data sent / received for TCPv4, TCPv6, UDPv4, UDPv6. Every one of
// them starts its payload with the process id and the byte count (two UINT32).
bool is_send(USHORT id) { return id == 10 || id == 26 || id == 42 || id == 58; }
bool is_receive(USHORT id) { return id == 11 || id == 27 || id == 43 || id == 59; }

struct Counters {
    uint64_t rx = 0, tx = 0;
};

class NetTrace {
public:
    static NetTrace &shared() {
        static NetTrace instance;
        return instance;
    }

    bool live() const { return live_; }

    void read(std::vector<RawProcess> &processes) {
        if (!live_) return;
        std::lock_guard lock(mutex_);
        std::unordered_set<uint32_t> alive;
        alive.reserve(processes.size());
        for (RawProcess &p : processes) {
            alive.insert(static_cast<uint32_t>(p.pid));
            p.has_net_io = true;
            if (auto it = totals_.find(static_cast<uint32_t>(p.pid)); it != totals_.end()) {
                p.net_rx = it->second.rx;
                p.net_tx = it->second.tx;
            }
        }
        // Processes that are gone take their totals along (a reused pid starts from whatever is
        // left, which the portable layer tolerates: its identity includes the start time).
        for (auto it = totals_.begin(); it != totals_.end();)
            it = alive.count(it->first) ? std::next(it) : totals_.erase(it);
    }

private:
    NetTrace() { live_ = is_elevated() && start(); }

    ~NetTrace() {
        if (session_) ControlTraceW(session_, nullptr, properties(), EVENT_TRACE_CONTROL_STOP);
        if (consumer_ != INVALID_PROCESSTRACE_HANDLE) CloseTrace(consumer_);
        if (thread_.joinable()) thread_.join();
    }

    EVENT_TRACE_PROPERTIES *properties() { return reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buffer_.data()); }

    bool start() {
        buffer_.assign(sizeof(EVENT_TRACE_PROPERTIES) + 2 * 1024 * sizeof(wchar_t), 0);
        EVENT_TRACE_PROPERTIES *props = properties();
        props->Wnode.BufferSize = static_cast<ULONG>(buffer_.size());
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;  // QueryPerformanceCounter timestamps
        props->BufferSize = 64;          // KB
        props->MinimumBuffers = 4;
        props->MaximumBuffers = 16;
        props->FlushTimer = 1;  // deliver every second, so a refresh sees the previous second
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ULONG status = StartTraceW(&session_, kSessionName, props);
        if (status == ERROR_ALREADY_EXISTS) {
            // Left behind by a Procyon that didn't exit cleanly: take it over.
            ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
            std::memset(buffer_.data() + sizeof(EVENT_TRACE_PROPERTIES), 0,
                        buffer_.size() - sizeof(EVENT_TRACE_PROPERTIES));
            props->Wnode.BufferSize = static_cast<ULONG>(buffer_.size());
            props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            props->Wnode.ClientContext = 1;
            props->BufferSize = 64;
            props->MinimumBuffers = 4;
            props->MaximumBuffers = 16;
            props->FlushTimer = 1;
            props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
            props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            status = StartTraceW(&session_, kSessionName, props);
        }
        if (status != ERROR_SUCCESS) {
            session_ = 0;
            return false;
        }
        if (EnableTraceEx2(session_, &kKernelNetwork, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0, 0,
                           0, nullptr) != ERROR_SUCCESS) {
            ControlTraceW(session_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
            return false;
        }
        EVENT_TRACE_LOGFILEW log{};
        log.LoggerName = const_cast<wchar_t *>(kSessionName);
        log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        log.EventRecordCallback = &NetTrace::on_event;
        log.Context = this;
        consumer_ = OpenTraceW(&log);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
            ControlTraceW(session_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
            return false;
        }
        thread_ = std::thread([this] {
            TRACEHANDLE handle = consumer_;
            ProcessTrace(&handle, 1, nullptr, nullptr);  // returns when the session is closed
        });
        return true;
    }

    static void WINAPI on_event(PEVENT_RECORD record) {
        auto *self = static_cast<NetTrace *>(record->UserContext);
        if (!self || !IsEqualGUID(record->EventHeader.ProviderId, kKernelNetwork)) return;
        const USHORT id = record->EventHeader.EventDescriptor.Id;
        const bool send = is_send(id);
        if (!send && !is_receive(id)) return;
        if (record->UserDataLength < 8 || !record->UserData) return;
        uint32_t pid = 0, size = 0;
        std::memcpy(&pid, record->UserData, sizeof(pid));
        std::memcpy(&size, static_cast<const char *>(record->UserData) + 4, sizeof(size));
        std::lock_guard lock(self->mutex_);
        Counters &c = self->totals_[pid];
        (send ? c.tx : c.rx) += size;
    }

    std::vector<char> buffer_;
    TRACEHANDLE session_ = 0;
    TRACEHANDLE consumer_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread thread_;
    std::mutex mutex_;
    std::unordered_map<uint32_t, Counters> totals_;
    bool live_ = false;
};

}  // namespace

bool process_network_available() { return NetTrace::shared().live(); }

void process_network(std::vector<RawProcess> &processes) { NetTrace::shared().read(processes); }

}  // namespace procyon::platform

#endif  // _WIN32
