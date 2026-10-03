// GPUs on Windows: adapters from DXGI, usage from the "GPU Engine" and "GPU Adapter Memory"
// performance counters (what Task Manager reads). NVIDIA temperatures come from NVML (nvml.dll ships
// with the driver and answers any user). Per-process GPU time comes from the same
// counters: each engine instance is named after the pid that owns it.
#if defined(_WIN32)

#include "windows_internal.hpp"

#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")

namespace procyon::platform {

using namespace win;

namespace {

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

struct Adapter {
    std::string luid;  // "luid_0x00000000_0x0000d3a5", lower-case
    pc_gpu info{};
};

struct EngineKey {
    std::string luid;
    std::string engine_type;  // "3D", "Copy", "VideoDecode", ...
    bool operator==(const EngineKey &o) const { return luid == o.luid && engine_type == o.engine_type; }
};

struct EngineKeyHash {
    size_t operator()(const EngineKey &k) const { return std::hash<std::string>()(k.luid + "|" + k.engine_type); }
};

struct GpuState {
    std::mutex mutex;
    bool adapters_read = false;
    std::vector<Adapter> adapters;
    bool pdh_tried = false;
    bool pdh_ok = false;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER running_time = nullptr;
    PDH_HCOUNTER dedicated = nullptr;
    PDH_HCOUNTER shared = nullptr;
    std::vector<char> buffer;
    // Cumulative engine time (100 ns) per adapter engine type from the last collection, and per
    // pid and engine type.
    std::unordered_map<EngineKey, uint64_t, EngineKeyHash> engine_time;
    std::unordered_map<int32_t, std::unordered_map<std::string, uint64_t>> process_engine_time;
    // What each process is reported as: nanoseconds of its busiest engine, summed over every
    // collection, so it only grows (see collect()).
    std::unordered_map<int32_t, uint64_t> process_time_ns;
    std::chrono::steady_clock::time_point collected{};
    bool have_previous = false;
    std::unordered_map<EngineKey, double, EngineKeyHash> utilization;  // 0..1 since the previous collection
};

GpuState &state() {
    static GpuState s;
    return s;
}

std::string luid_name(const LUID &luid) {
    char text[64];
    (void)std::snprintf(text, sizeof(text), "luid_0x%08x_0x%08x", static_cast<unsigned>(luid.HighPart),
                        static_cast<unsigned>(luid.LowPart));
    return text;
}

const char *vendor_name(UINT vendor_id) {
    switch (vendor_id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002:
        case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        case 0x5143: return "Qualcomm";
        default: return "";
    }
}

void read_adapters(GpuState &s) {
    if (s.adapters_read) return;
    s.adapters_read = true;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory) return;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            Adapter a;
            a.luid = luid_name(desc.AdapterLuid);
            copy_string(a.info.name, sizeof(a.info.name), utf8(desc.Description, wcsnlen(desc.Description, 128)));
            copy_string(a.info.vendor, sizeof(a.info.vendor), vendor_name(desc.VendorId));
            // Integrated GPUs carve their memory out of system RAM.
            a.info.unified_memory = desc.DedicatedVideoMemory < (512ull << 20);
            a.info.memory_total = a.info.unified_memory ? -1 : static_cast<int64_t>(desc.DedicatedVideoMemory);
            a.info.memory_used = -1;
            a.info.utilization = a.info.renderer_utilization = a.info.tiler_utilization = -1;
            a.info.encoder_utilization = a.info.decoder_utilization = -1;
            a.info.temperature = -1;
            if (std::string(a.info.name) != "Microsoft Basic Render Driver") s.adapters.push_back(std::move(a));
        }
        adapter->Release();
    }
    factory->Release();
}

void open_counters(GpuState &s) {
    if (s.pdh_tried) return;
    s.pdh_tried = true;
    if (PdhOpenQueryW(nullptr, 0, &s.query) != ERROR_SUCCESS) return;
    // English names work on every locale.
    if (PdhAddEnglishCounterW(s.query, L"\\GPU Engine(*)\\Running Time", 0, &s.running_time) != ERROR_SUCCESS) {
        PdhCloseQuery(s.query);
        s.query = nullptr;
        return;
    }
    PdhAddEnglishCounterW(s.query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &s.dedicated);
    PdhAddEnglishCounterW(s.query, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &s.shared);
    s.pdh_ok = PdhCollectQueryData(s.query) == ERROR_SUCCESS;
}

// Instances of one counter as (name, raw first value).
std::vector<std::pair<std::string, uint64_t>> raw_array(GpuState &s, PDH_HCOUNTER counter) {
    std::vector<std::pair<std::string, uint64_t>> out;
    if (!counter) return out;
    DWORD size = 0, count = 0;
    PDH_STATUS status = PdhGetRawCounterArrayW(counter, &size, &count, nullptr);
    if (status != PDH_MORE_DATA || size == 0) return out;
    if (s.buffer.size() < size) s.buffer.resize(size);
    auto items = reinterpret_cast<PDH_RAW_COUNTER_ITEM_W *>(s.buffer.data());
    status = PdhGetRawCounterArrayW(counter, &size, &count, items);
    if (status != ERROR_SUCCESS) return out;
    out.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].RawValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].RawValue.CStatus != PDH_CSTATUS_NEW_DATA)
            continue;
        out.emplace_back(lower_ascii(utf8(items[i].szName, wcslen(items[i].szName))),
                         static_cast<uint64_t>(std::max<LONGLONG>(items[i].RawValue.FirstValue, 0)));
    }
    return out;
}

// "pid_1234_luid_0x00000000_0x0000d3a5_phys_0_eng_3_engtype_3d"
bool parse_engine(const std::string &name, int32_t &pid, std::string &luid, std::string &engine_type) {
    if (name.rfind("pid_", 0) != 0) return false;
    pid = std::atoi(name.c_str() + 4);
    const size_t luid_at = name.find("luid_");
    const size_t phys_at = name.find("_phys_");
    const size_t type_at = name.find("engtype_");
    if (luid_at == std::string::npos || phys_at == std::string::npos || type_at == std::string::npos) return false;
    luid = name.substr(luid_at, phys_at - luid_at);
    engine_type = name.substr(type_at + 8);
    return true;
}

// Collects every GPU counter once; the results feed gpus() and process_gpu() of the same refresh.
void collect(GpuState &s) {
    if (!s.pdh_ok || PdhCollectQueryData(s.query) != ERROR_SUCCESS) return;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = s.have_previous ? std::chrono::duration<double>(now - s.collected).count() : 0;

    std::unordered_map<EngineKey, uint64_t, EngineKeyHash> engine_time;
    std::unordered_map<int32_t, std::unordered_map<std::string, uint64_t>> per_process;  // pid -> type -> time
    for (const auto &[name, value] : raw_array(s, s.running_time)) {
        int32_t pid = 0;
        std::string luid, type;
        if (!parse_engine(name, pid, luid, type)) continue;
        engine_time[{luid, type}] += value;
        per_process[pid][type] += value;
    }
    s.utilization.clear();
    if (elapsed > 0) {
        for (const auto &[key, value] : engine_time) {
            auto previous = s.engine_time.find(key);
            if (previous == s.engine_time.end() || value < previous->second) continue;
            // 100 ns of engine time per second of wall time.
            s.utilization[key] = std::min(1.0, static_cast<double>(value - previous->second) / (elapsed * 1e7));
        }
    }
    s.engine_time = std::move(engine_time);
    // A process's GPU share over an interval is its busiest engine type (what Task Manager shows:
    // engines run in parallel, so the sum of all of them would exceed one GPU). The busiest type
    // can change from one interval to the next, so the cumulative figure handed to the portable
    // layer adds up the per-interval winners instead of switching between the raw counters, which
    // would run backwards. Processes gone from the counters are dropped.
    for (auto it = s.process_time_ns.begin(); it != s.process_time_ns.end();) {
        if (per_process.count(it->first))
            ++it;
        else
            it = s.process_time_ns.erase(it);
    }
    for (const auto &[pid, types] : per_process) {
        uint64_t busiest = 0;
        if (auto previous = s.process_engine_time.find(pid); previous != s.process_engine_time.end()) {
            for (const auto &[type, value] : types) {
                auto before = previous->second.find(type);
                if (before != previous->second.end() && value >= before->second)
                    busiest = std::max(busiest, value - before->second);
            }
        }
        s.process_time_ns[pid] += busiest * 100;
    }
    s.process_engine_time = std::move(per_process);

    std::unordered_map<std::string, uint64_t> dedicated, shared;
    for (const auto &[name, value] : raw_array(s, s.dedicated)) {
        const size_t phys = name.find("_phys_");
        dedicated[phys == std::string::npos ? name : name.substr(0, phys)] += value;
    }
    for (const auto &[name, value] : raw_array(s, s.shared)) {
        const size_t phys = name.find("_phys_");
        shared[phys == std::string::npos ? name : name.substr(0, phys)] += value;
    }
    for (Adapter &a : s.adapters) {
        double busiest = -1, three_d = -1, encode = -1, decode = -1;
        for (const auto &[key, value] : s.utilization) {
            if (key.luid != a.luid) continue;
            busiest = std::max(busiest, value);
            if (key.engine_type == "3d") three_d = value;
            if (key.engine_type == "videoencode") encode = value;
            if (key.engine_type == "videodecode") decode = value;
        }
        a.info.utilization = busiest;
        a.info.renderer_utilization = three_d;
        a.info.encoder_utilization = encode;
        a.info.decoder_utilization = decode;
        auto used = (a.info.unified_memory ? shared : dedicated).find(a.luid);
        a.info.memory_used =
            used == (a.info.unified_memory ? shared : dedicated).end() ? -1 : static_cast<int64_t>(used->second);
    }
    s.collected = now;
    s.have_previous = true;
}

// ---- NVIDIA temperatures through NVML (no SDK: the driver installs nvml.dll into System32) ----

struct Nvml {
    bool tried = false;
    HMODULE module = nullptr;
    int (*device_count)(unsigned *) = nullptr;
    int (*device_by_index)(unsigned, void **) = nullptr;
    int (*device_name)(void *, char *, unsigned) = nullptr;
    int (*temperature)(void *, int, unsigned *) = nullptr;
    std::vector<std::pair<std::string, void *>> devices;  // product name, handle
};

Nvml &nvml() {
    static Nvml n;
    return n;
}

template <typename F>
bool resolve(HMODULE module, const char *name, F &out) {
    out = reinterpret_cast<F>(GetProcAddress(module, name));
    return out != nullptr;
}

void open_nvml(Nvml &n) {
    if (n.tried) return;
    n.tried = true;
    n.module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!n.module) return;
    int (*init)() = nullptr;
    if (!resolve(n.module, "nvmlInit_v2", init) || !resolve(n.module, "nvmlDeviceGetCount_v2", n.device_count) ||
        !resolve(n.module, "nvmlDeviceGetHandleByIndex_v2", n.device_by_index) ||
        !resolve(n.module, "nvmlDeviceGetName", n.device_name) ||
        !resolve(n.module, "nvmlDeviceGetTemperature", n.temperature) || init() != 0)
        return;
    unsigned count = 0;
    if (n.device_count(&count) != 0) return;
    for (unsigned i = 0; i < count && i < 8; ++i) {
        void *device = nullptr;
        char name[96] = {};
        if (n.device_by_index(i, &device) != 0 || n.device_name(device, name, sizeof(name)) != 0) continue;
        n.devices.emplace_back(name, device);
    }
}

// The GPU sensor of the NVML device matching the adapter: by product name (the nth one among
// identical cards), else by order among the NVIDIA adapters when the counts agree.
double nvidia_temperature(Nvml &n, const std::string &name, size_t same_name_ordinal, size_t nvidia_ordinal,
                          size_t nvidia_count) {
    void *device = nullptr;
    size_t seen = 0;
    for (const auto &[device_name, handle] : n.devices)
        if (device_name == name && seen++ == same_name_ordinal) device = handle;
    if (!device && n.devices.size() == nvidia_count && nvidia_ordinal < n.devices.size())
        device = n.devices[nvidia_ordinal].second;
    unsigned celsius = 0;
    if (!device || n.temperature(device, 0 /* NVML_TEMPERATURE_GPU */, &celsius) != 0 || celsius >= 150) return -1;
    return celsius;
}

}  // namespace

std::vector<pc_gpu> gpus() {
    GpuState &s = state();
    std::lock_guard lock(s.mutex);
    read_adapters(s);
    if (s.adapters.empty()) return {};
    open_counters(s);
    collect(s);
    Nvml &n = nvml();
    open_nvml(n);
    size_t nvidia_count = 0;
    for (const Adapter &a : s.adapters) nvidia_count += std::string(a.info.vendor) == "NVIDIA";
    std::unordered_map<std::string, size_t> same_name;
    size_t nvidia_ordinal = 0;
    std::vector<pc_gpu> out;
    for (const Adapter &a : s.adapters) {
        pc_gpu info = a.info;
        if (std::string(info.vendor) == "NVIDIA" && !n.devices.empty())
            info.temperature = nvidia_temperature(n, info.name, same_name[info.name]++, nvidia_ordinal++, nvidia_count);
        out.push_back(info);
    }
    return out;
}

bool process_gpu_available() {
    GpuState &s = state();
    std::lock_guard lock(s.mutex);
    read_adapters(s);
    if (s.adapters.empty()) return false;
    open_counters(s);
    return s.pdh_ok;
}

void process_gpu(std::vector<RawProcess> &processes) {
    GpuState &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.pdh_ok) return;
    for (RawProcess &p : processes) {
        auto it = s.process_time_ns.find(p.pid);
        p.has_gpu = true;  // an engine-less process has used no GPU time
        p.gpu_time_ns = it == s.process_time_ns.end() ? 0 : it->second;
    }
}

}  // namespace procyon::platform

#endif  // _WIN32
