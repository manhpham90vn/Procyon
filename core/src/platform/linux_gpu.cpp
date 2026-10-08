// GPUs from the DRM devices in sysfs (AMD and Intel), NVIDIA through NVML (libnvidia-ml, installed
// with the driver, loaded at runtime), and per-process GPU time from the DRM fdinfo engine counters
// (/proc/<pid>/fdinfo, Linux 5.19+: amdgpu, i915, nouveau, msm, panfrost, v3d).
#if defined(__linux__)

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "linux_internal.hpp"

namespace procyon::platform {

using namespace linux_internal;

namespace {

// ---- NVIDIA through NVML (no SDK: the few entry points are declared here) ----

struct NvmlUtilization {
    unsigned int gpu, memory;
};
struct NvmlMemory {
    unsigned long long total, free, used;
};
using NvmlDevice = void *;

struct Nvml {
    bool tried = false;
    bool ready = false;
    int (*device_count)(unsigned int *) = nullptr;
    int (*device_by_index)(unsigned int, NvmlDevice *) = nullptr;
    int (*device_name)(NvmlDevice, char *, unsigned int) = nullptr;
    int (*utilization)(NvmlDevice, NvmlUtilization *) = nullptr;
    int (*memory)(NvmlDevice, NvmlMemory *) = nullptr;
    int (*temperature)(NvmlDevice, int, unsigned int *) = nullptr;
    int (*encoder)(NvmlDevice, unsigned int *, unsigned int *) = nullptr;
    int (*decoder)(NvmlDevice, unsigned int *, unsigned int *) = nullptr;
};

template <typename F>
bool resolve(void *module, const char *name, F &out) {
    out = reinterpret_cast<F>(dlsym(module, name));
    return out != nullptr;
}

Nvml &nvml() {
    static Nvml n;
    static std::once_flag once;
    std::call_once(once, [] {
        n.tried = true;
        // Only where the NVIDIA driver is loaded: dlopen'ing the library elsewhere costs memory.
        if (!exists("/proc/driver/nvidia/version")) return;
        void *module = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!module) return;
        int (*init)() = nullptr;
        if (!resolve(module, "nvmlInit_v2", init) || !resolve(module, "nvmlDeviceGetCount_v2", n.device_count) ||
            !resolve(module, "nvmlDeviceGetHandleByIndex_v2", n.device_by_index) ||
            !resolve(module, "nvmlDeviceGetName", n.device_name) ||
            !resolve(module, "nvmlDeviceGetUtilizationRates", n.utilization) ||
            !resolve(module, "nvmlDeviceGetMemoryInfo", n.memory) ||
            !resolve(module, "nvmlDeviceGetTemperature", n.temperature) || init() != 0) {
            dlclose(module);
            return;
        }
        resolve(module, "nvmlDeviceGetEncoderUtilization", n.encoder);
        resolve(module, "nvmlDeviceGetDecoderUtilization", n.decoder);
        n.ready = true;
    });
    return n;
}

void nvidia_gpus(std::vector<pc_gpu> &out) {
    Nvml &n = nvml();
    if (!n.ready) return;
    unsigned int count = 0;
    if (n.device_count(&count) != 0) return;
    for (unsigned int i = 0; i < count; ++i) {
        NvmlDevice device = nullptr;
        if (n.device_by_index(i, &device) != 0) continue;
        pc_gpu g{};
        char name[96] = {};
        n.device_name(device, name, sizeof(name));
        copy_string(g.name, sizeof(g.name), name);
        copy_string(g.vendor, sizeof(g.vendor), "NVIDIA");
        g.utilization = g.renderer_utilization = g.tiler_utilization = -1;
        g.encoder_utilization = g.decoder_utilization = -1;
        g.memory_used = g.memory_total = -1;
        g.temperature = -1;
        NvmlUtilization utilization{};
        if (n.utilization(device, &utilization) == 0) g.utilization = utilization.gpu / 100.0;
        NvmlMemory memory{};
        if (n.memory(device, &memory) == 0) {
            g.memory_used = static_cast<int64_t>(memory.used);
            g.memory_total = static_cast<int64_t>(memory.total);
        }
        unsigned int celsius = 0, period = 0;
        if (n.temperature(device, 0 /* NVML_TEMPERATURE_GPU */, &celsius) == 0 && celsius < 150)
            g.temperature = celsius;
        unsigned int value = 0;
        if (n.encoder && n.encoder(device, &value, &period) == 0) g.encoder_utilization = value / 100.0;
        if (n.decoder && n.decoder(device, &value, &period) == 0) g.decoder_utilization = value / 100.0;
        out.push_back(g);
    }
}

// ---- DRM devices ----

struct DrmCard {
    std::string device;  // /sys/class/drm/cardN/device
    std::string driver;
    std::string vendor;
    std::string name;
    bool integrated = false;
    std::string hwmon;  // directory with temp1_input, "" when none
    // Intel: each GT's cumulative idle time in ms (i915 rc6_residency_ms, xe gtidle/idle_residency_ms).
    std::vector<std::string> idle_files;
};

// "Vendor Device" from the PCI id database, "" when it isn't installed.
std::string pci_name(uint32_t vendor, uint32_t device) {
    std::string text;
    if (!read_file("/usr/share/hwdata/pci.ids", text) && !read_file("/usr/share/misc/pci.ids", text)) return {};
    char vendor_key[8], device_key[8];
    std::snprintf(vendor_key, sizeof(vendor_key), "%04x", vendor);
    std::snprintf(device_key, sizeof(device_key), "\t%04x", device);
    size_t at = 0;
    bool in_vendor = false;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string_view line(text.data() + at, end - at);
        if (!line.empty() && line[0] != '\t' && line[0] != '#') {
            if (in_vendor) break;
            in_vendor = line.substr(0, 4) == vendor_key;
        } else if (in_vendor && line.substr(0, 5) == device_key) {
            std::string name = trim(line.substr(5));
            // "Rocket Lake-S GT1 [UHD Graphics 750]": the marketing name is in brackets.
            const auto open = name.find('[');
            const auto close = name.rfind(']');
            if (open != std::string::npos && close != std::string::npos && close > open)
                name = name.substr(open + 1, close - open - 1);
            return name;
        }
        at = end + 1;
    }
    return {};
}

const std::vector<DrmCard> &drm_cards() {
    static const std::vector<DrmCard> cards = [] {
        std::vector<DrmCard> list;
        auto names = list_dir("/sys/class/drm");
        std::sort(names.begin(), names.end());
        for (const auto &name : names) {
            // card0, card1 … (connectors are card0-HDMI-A-1).
            if (!starts_with(name, "card") || name.find('-') != std::string::npos) continue;
            DrmCard card;
            card.device = "/sys/class/drm/" + name + "/device";
            card.driver = basename_of(read_link(card.device + "/driver"));
            if (card.driver.empty() || card.driver == "nvidia" || card.driver == "simpledrm" ||
                card.driver == "vboxvideo")
                continue;  // NVIDIA is read through NVML; firmware framebuffers aren't GPUs
            const uint32_t vendor =
                static_cast<uint32_t>(std::strtoul(read_line(card.device + "/vendor").c_str(), nullptr, 16));
            const uint32_t device =
                static_cast<uint32_t>(std::strtoul(read_line(card.device + "/device").c_str(), nullptr, 16));
            card.vendor = vendor == 0x1002   ? "AMD"
                          : vendor == 0x8086 ? "Intel"
                          : vendor == 0x10de ? "NVIDIA"
                                             : card.driver;
            card.name = trim(read_line(card.device + "/product_name"));
            if (card.name.empty()) card.name = pci_name(vendor, device);
            if (card.name.empty())
                card.name = card.vendor + " GPU";
            else if (card.name.find(card.vendor) == std::string::npos)
                card.name = card.vendor + " " + card.name;
            // Intel's integrated GPUs and AMD APUs (no VRAM of their own) share system memory.
            const int64_t vram = read_int(card.device + "/mem_info_vram_total", -1);
            card.integrated = card.driver == "i915" || card.driver == "xe" ? !exists(card.device + "/lmem_total_bytes")
                                                                           : vram >= 0 && vram <= (512ll << 20);
            for (const auto &h : list_dir(card.device + "/hwmon"))
                if (exists(card.device + "/hwmon/" + h + "/temp1_input")) card.hwmon = card.device + "/hwmon/" + h;
            if (card.driver == "i915") {
                for (const auto &gt : list_dir("/sys/class/drm/" + name + "/gt"))
                    if (exists("/sys/class/drm/" + name + "/gt/" + gt + "/rc6_residency_ms"))
                        card.idle_files.push_back("/sys/class/drm/" + name + "/gt/" + gt + "/rc6_residency_ms");
            } else if (card.driver == "xe") {
                for (const auto &tile : list_dir(card.device))
                    if (starts_with(tile, "tile"))
                        for (const auto &gt : list_dir(card.device + "/" + tile))
                            if (exists(card.device + "/" + tile + "/" + gt + "/gtidle/idle_residency_ms"))
                                card.idle_files.push_back(card.device + "/" + tile + "/" + gt +
                                                          "/gtidle/idle_residency_ms");
            }
            list.push_back(card);
        }
        return list;
    }();
    return cards;
}

// Drivers whose fdinfo has drm-engine-<name>: <ns> counters.
bool fdinfo_driver(const std::string &driver) {
    return driver == "amdgpu" || driver == "i915" || driver == "nouveau" || driver == "msm" || driver == "panfrost" ||
           driver == "v3d" || driver == "etnaviv" || driver == "panthor";
}

}  // namespace

namespace {

// Intel publishes no busy share without perf (root). Its GTs' idle residency is public: the share of
// time a GT was out of RC6 (awake) since the previous call, the busiest GT's. Awake is a little more
// than busy (a GT stays up briefly after its last job), so light loads read slightly high. -1 on the
// first call, which only sets the baseline.
double intel_awake_share(const DrmCard &card, size_t index) {
    struct Sample {
        std::vector<int64_t> idle_ms;
        double at = 0;
    };
    static std::mutex mutex;
    static std::unordered_map<size_t, Sample> previous;
    if (card.idle_files.empty()) return -1;
    Sample now;
    now.at = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const auto &file : card.idle_files) now.idle_ms.push_back(read_int(file, -1));
    std::lock_guard lock(mutex);
    double share = -1;
    auto it = previous.find(index);
    if (it != previous.end() && it->second.idle_ms.size() == now.idle_ms.size() && now.at - it->second.at >= 200) {
        const double elapsed = now.at - it->second.at;
        for (size_t i = 0; i < now.idle_ms.size(); ++i) {
            if (now.idle_ms[i] < 0 || it->second.idle_ms[i] < 0) continue;
            const double idle = static_cast<double>(now.idle_ms[i] - it->second.idle_ms[i]);
            share = std::max(share, std::clamp(1.0 - idle / elapsed, 0.0, 1.0));
        }
    }
    if (it == previous.end() || now.at - it->second.at >= 200) previous[index] = std::move(now);
    return share;
}

}  // namespace

std::vector<pc_gpu> gpus() {
    std::vector<pc_gpu> result;
    size_t index = 0;
    for (const auto &card : drm_cards()) {
        pc_gpu g{};
        copy_string(g.name, sizeof(g.name), card.name);
        copy_string(g.vendor, sizeof(g.vendor), card.vendor);
        g.unified_memory = card.integrated;
        g.utilization = g.renderer_utilization = g.tiler_utilization = -1;
        g.encoder_utilization = g.decoder_utilization = -1;
        g.memory_used = g.memory_total = -1;
        g.temperature = -1;
        // amdgpu publishes its busy share; Intel's comes from idle residency.
        const int64_t busy = read_int(card.device + "/gpu_busy_percent", -1);
        if (busy >= 0 && busy <= 100) g.utilization = busy / 100.0;
        if (g.utilization < 0) g.utilization = intel_awake_share(card, index);
        ++index;
        const int64_t used = read_int(card.device + "/mem_info_vram_used", -1);
        const int64_t total = read_int(card.device + "/mem_info_vram_total", -1);
        if (!card.integrated) {
            if (used >= 0 && total > 0) {
                g.memory_used = used;
                g.memory_total = total;
            }
        } else if (used >= 0) {
            // An APU's carve-out plus what it maps from system memory (GTT): the memory it uses, as
            // macOS and Windows report a unified GPU's. No total: it shares the system's. i915/xe
            // publish no such figure.
            const int64_t gtt = read_int(card.device + "/mem_info_gtt_used", 0);
            g.memory_used = used + std::max<int64_t>(gtt, 0);
        }
        if (!card.hwmon.empty()) {
            const int64_t milli = read_int(card.hwmon + "/temp1_input", -1);
            if (milli > 0 && milli < 150000) g.temperature = milli / 1000.0;
        }
        result.push_back(g);
    }
    nvidia_gpus(result);
    return result;
}

bool process_gpu_available() {
    for (const auto &card : drm_cards())
        if (fdinfo_driver(card.driver)) return true;
    return false;
}

namespace {

// Which descriptors of a process are DRM clients. Listing every descriptor of every process each
// tick is the expensive part, so the list is kept and re-scanned every few seconds.
struct GpuFds {
    std::vector<int32_t> fds;
    double scanned = 0;
};
std::mutex gpu_mutex;
std::unordered_map<uint64_t, GpuFds> gpu_fds;

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void scan_gpu_fds(int32_t pid, GpuFds &entry) {
    entry.fds.clear();
    const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
    for (const auto &name : list_dir(dir)) {
        const std::string target = read_link(dir + "/" + name);
        if (starts_with(target, "/dev/dri/")) entry.fds.push_back(std::atoi(name.c_str()));
    }
}

}  // namespace

// The engine time of one fdinfo text: the busiest kind of engine, so a client that renders and
// decodes at once isn't counted twice. Returns false when it isn't a DRM client. Exposed for tests.
bool parse_drm_fdinfo(const std::string &text, uint64_t &client_id, uint64_t &engine_ns) {
    client_id = 0;
    engine_ns = 0;
    bool client = false;
    for (const auto &line : split(text, '\n')) {
        if (starts_with(line, "drm-client-id:")) {
            client_id = std::strtoull(line.c_str() + 14, nullptr, 10);
            client = true;
        } else if (starts_with(line, "drm-engine-") && !starts_with(line, "drm-engine-capacity-")) {
            const auto colon = line.find(':');
            if (colon == std::string::npos || line.find(" ns", colon) == std::string::npos) continue;
            engine_ns = std::max<uint64_t>(engine_ns, std::strtoull(line.c_str() + colon + 1, nullptr, 10));
        }
    }
    return client;
}

void process_gpu(std::vector<RawProcess> &processes) {
    const double now = now_seconds();
    std::lock_guard lock(gpu_mutex);
    std::unordered_map<uint64_t, GpuFds> next;
    next.reserve(processes.size());
    std::string text;
    for (auto &p : processes) {
        // Another user's descriptors can't be read: its GPU time stays unknown.
        if (!p.has_disk_io) continue;
        const uint64_t key =
            (static_cast<uint64_t>(static_cast<uint32_t>(p.pid)) << 32) ^ static_cast<uint64_t>(p.start_time);
        GpuFds entry;
        auto found = gpu_fds.find(key);
        if (found != gpu_fds.end() && now - found->second.scanned < 5) {
            entry = found->second;
        } else {
            scan_gpu_fds(p.pid, entry);
            entry.scanned = now;
        }
        // One client may be reachable through several descriptors (dup, fork): count it once.
        std::unordered_set<uint64_t> clients;
        uint64_t total = 0;
        const std::string base = "/proc/" + std::to_string(p.pid) + "/fdinfo/";
        for (int32_t fd : entry.fds) {
            uint64_t client = 0, ns = 0;
            if (!read_file(base + std::to_string(fd), text) || !parse_drm_fdinfo(text, client, ns)) continue;
            if (clients.insert(client).second) total += ns;
        }
        p.has_gpu = true;
        p.gpu_time_ns = total;
        next.emplace(key, std::move(entry));
    }
    gpu_fds = std::move(next);
}

}  // namespace procyon::platform

#endif  // __linux__
