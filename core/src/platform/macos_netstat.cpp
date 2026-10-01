// macOS per-process network counters from the private NetworkStatistics framework (what nettop
// and Activity Monitor use). It is loaded with dlopen so a missing or changed framework only
// turns the feature off; nothing here may crash or stall sampling.
#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach_time.h>

#include <chrono>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../platform.hpp"

namespace procyon::platform {
namespace {

using ManagerRef = void *;
using SourceRef = void *;
using DictionaryBlock = void (^)(CFDictionaryRef);
using VoidBlock = void (^)();

// Signatures as reverse-engineered by nettop clients (e.g. Objective-See's Netiquette).
struct Api {
    ManagerRef (*create)(CFAllocatorRef, dispatch_queue_t, void (^)(SourceRef, void *)) = nullptr;
    int (*add_all_tcp)(ManagerRef, int, int) = nullptr;
    int (*add_all_udp)(ManagerRef, int, int) = nullptr;
    void (*query_all)(ManagerRef, VoidBlock) = nullptr;
    void (*set_counts_block)(SourceRef, DictionaryBlock) = nullptr;
    void (*set_description_block)(SourceRef, DictionaryBlock) = nullptr;
    void (*query_description)(SourceRef) = nullptr;
    void (*set_removed_block)(SourceRef, VoidBlock) = nullptr;
    CFStringRef key_pid = nullptr;
    CFStringRef key_upid = nullptr;
    CFStringRef key_rx = nullptr;
    CFStringRef key_tx = nullptr;
    CFStringRef key_start = nullptr;

    bool load() {
        void *lib = dlopen("/System/Library/PrivateFrameworks/NetworkStatistics.framework/NetworkStatistics",
                           RTLD_LAZY | RTLD_LOCAL);
        if (!lib) return false;
        auto symbol = [lib](auto &out, const char *name) {
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(dlsym(lib, name));
            return out != nullptr;
        };
        auto key = [lib](CFStringRef &out, const char *name) {
            auto *ptr = static_cast<const CFStringRef *>(dlsym(lib, name));
            out = ptr ? *ptr : nullptr;
            return out != nullptr;
        };
        return symbol(create, "NStatManagerCreate") && symbol(add_all_tcp, "NStatManagerAddAllTCPWithFilter") &&
               symbol(add_all_udp, "NStatManagerAddAllUDPWithFilter") &&
               symbol(query_all, "NStatManagerQueryAllSources") &&
               symbol(set_counts_block, "NStatSourceSetCountsBlock") &&
               symbol(set_description_block, "NStatSourceSetDescriptionBlock") &&
               symbol(query_description, "NStatSourceQueryDescription") &&
               symbol(set_removed_block, "NStatSourceSetRemovedBlock") && key(key_pid, "kNStatSrcKeyPID") &&
               key(key_upid, "kNStatSrcKeyUPID") && key(key_rx, "kNStatSrcKeyRxBytes") &&
               key(key_tx, "kNStatSrcKeyTxBytes") && key(key_start, "kNStatSrcKeyStartAbsoluteTime");
    }
};

bool number(CFDictionaryRef dict, CFStringRef key, int64_t &out) {
    auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, key));
    return value && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberSInt64Type, &out);
}

uint64_t advance(uint64_t &last, int64_t now) {
    const auto value = static_cast<uint64_t>(now > 0 ? now : 0);
    const uint64_t delta = value > last ? value - last : 0;
    last = value;
    return delta;
}

// One TCP or UDP socket. Counters are cumulative for the socket's lifetime.
struct Source {
    bool owned = false;     // owner known; counts before that are skipped, losing nothing
    bool baseline = false;  // opened before we started watching: next counts are history, not traffic
    int32_t pid = 0;
    uint64_t upid = 0;
    uint64_t rx = 0, tx = 0;
};

// Per-process totals. They only grow: a closed socket's bytes stay in its owner's total.
struct Totals {
    uint64_t upid = 0;  // unique across the boot, so a reused pid starts over
    uint64_t rx = 0, tx = 0;
    int32_t sources = 0;
};

class NetStat {
public:
    // The manager lives for the whole process: tearing it down while callbacks are in flight
    // isn't worth the risk for a few kilobytes.
    static NetStat &shared() {
        static auto *instance = new NetStat();
        return *instance;
    }

    bool live() const { return manager_ != nullptr; }

    void read(std::vector<RawProcess> &processes) {
        if (!live()) return;
        {
            std::lock_guard lock(mutex_);
            std::unordered_set<int32_t> alive;
            alive.reserve(processes.size());
            for (auto &p : processes) {
                alive.insert(p.pid);
                p.has_net_io = true;
                if (auto it = totals_.find(p.pid); it != totals_.end()) {
                    p.net_rx = it->second.rx;
                    p.net_tx = it->second.tx;
                }
            }
            std::erase_if(totals_,
                          [&](const auto &entry) { return entry.second.sources <= 0 && !alive.count(entry.first); });
            // A query normally completes within milliseconds; retry if a completion never came.
            const auto now = std::chrono::steady_clock::now();
            if (query_in_flight_ && now - query_started_ < std::chrono::seconds(5)) return;
            query_in_flight_ = true;
            query_started_ = now;
        }
        // Asynchronous: the counts land on our queue and are read by the next refresh.
        api_.query_all(manager_, ^{
          std::lock_guard lock(mutex_);
          query_in_flight_ = false;
        });
    }

private:
    NetStat() {
        if (!api_.load()) return;
        started_ = mach_continuous_time();
        queue_ = dispatch_queue_create("procyon.netstat", DISPATCH_QUEUE_SERIAL);
        manager_ = api_.create(kCFAllocatorDefault, queue_, ^(SourceRef source, void *) {
          added(source);
        });
        if (!manager_) return;
        api_.add_all_tcp(manager_, 0, 0);
        api_.add_all_udp(manager_, 0, 0);
    }

    // Runs on queue_.
    void added(SourceRef source) {
        // Counts from a query only name the owner once the description has been fetched.
        api_.set_description_block(source, ^(CFDictionaryRef dict) {
          describe(source, dict);
        });
        api_.set_counts_block(source, ^(CFDictionaryRef dict) {
          counts(source, dict);
        });
        // The framework delivers a final counts update just before this, so nothing is lost.
        api_.set_removed_block(source, ^{
          removed(source);
        });
        api_.query_description(source);
    }

    // Runs on queue_ with mutex_ held. Descriptions carry no byte counts, only the owner.
    bool own(Source &s, CFDictionaryRef dict) {
        // Attribute to the socket's owner (PID), not the effective PID it may be delegated to
        // (EPID, e.g. nsurlsessiond working for an app): this matches nettop's per-process
        // view, and a process's row then reflects its own sockets.
        int64_t pid = 0, upid = 0, start = 0;
        if (!number(dict, api_.key_pid, pid) || !number(dict, api_.key_upid, upid) || upid <= 0) return false;
        s.owned = true;
        s.pid = static_cast<int32_t>(pid);
        s.upid = static_cast<uint64_t>(upid);
        s.baseline = !number(dict, api_.key_start, start) || static_cast<uint64_t>(start) < started_;
        Totals &t = totals_[s.pid];
        if (t.upid < s.upid) t = {s.upid, 0, 0, 0};  // the pid was reused
        if (t.upid == s.upid) ++t.sources;
        return true;
    }

    // Runs on queue_.
    void describe(SourceRef source, CFDictionaryRef dict) {
        if (!dict) return;
        std::lock_guard lock(mutex_);
        Source &s = sources_[source];
        if (!s.owned) own(s, dict);
    }

    // Runs on queue_.
    void counts(SourceRef source, CFDictionaryRef dict) {
        int64_t rx = 0, tx = 0;
        if (!dict || !number(dict, api_.key_rx, rx) || !number(dict, api_.key_tx, tx)) return;
        std::lock_guard lock(mutex_);
        Source &s = sources_[source];
        if (!s.owned && !own(s, dict)) return;
        const uint64_t rx_delta = advance(s.rx, rx);
        const uint64_t tx_delta = advance(s.tx, tx);
        if (s.baseline) {
            s.baseline = false;
            return;
        }
        auto t = totals_.find(s.pid);
        if (t == totals_.end() || t->second.upid != s.upid) return;  // late update from a previous owner of the pid
        t->second.rx += rx_delta;
        t->second.tx += tx_delta;
    }

    // Runs on queue_.
    void removed(SourceRef source) {
        std::lock_guard lock(mutex_);
        auto it = sources_.find(source);
        if (it == sources_.end()) return;
        const Source &s = it->second;
        if (auto t = totals_.find(s.pid); s.owned && t != totals_.end() && t->second.upid == s.upid)
            --t->second.sources;
        sources_.erase(it);
    }

    Api api_;
    uint64_t started_ = 0;  // mach continuous time, the clock of kNStatSrcKeyStartAbsoluteTime
    dispatch_queue_t queue_ = nullptr;
    ManagerRef manager_ = nullptr;

    std::mutex mutex_;  // guards everything below; callbacks and the sampling thread share it
    bool query_in_flight_ = false;
    std::chrono::steady_clock::time_point query_started_;
    std::unordered_map<SourceRef, Source> sources_;
    std::unordered_map<int32_t, Totals> totals_;
};

}  // namespace

bool process_network_available() { return NetStat::shared().live(); }

void process_network(std::vector<RawProcess> &processes) { NetStat::shared().read(processes); }

}  // namespace procyon::platform

#endif  // __APPLE__
