// macOS GPUs through IOKit: IOAccelerator performance statistics for the whole GPU, and the
// accumulated GPU time its user clients (one or more per process) report for per-process usage.
#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "../platform.hpp"

namespace procyon::platform {
namespace {

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

bool dict_number(CFDictionaryRef dict, CFStringRef key, int64_t &out) {
    auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, key));
    return value && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberSInt64Type, &out);
}

// A registry property as text: CFString, or NUL-terminated CFData (PCI "model").
std::string property_string(io_registry_entry_t entry, CFStringRef key, bool search_parents = false) {
    const IOOptionBits options = search_parents ? kIORegistryIterateRecursively | kIORegistryIterateParents : 0;
    CFTypeRef value = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault, options);
    if (!value) return {};
    std::string text;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char buffer[256];
        if (CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer), kCFStringEncodingUTF8))
            text = buffer;
    } else if (CFGetTypeID(value) == CFDataGetTypeID()) {
        auto data = static_cast<CFDataRef>(value);
        text.assign(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), CFDataGetLength(data));
        text.resize(strnlen(text.c_str(), text.size()));
    }
    CFRelease(value);
    return text;
}

int64_t property_number(io_registry_entry_t entry, CFStringRef key, bool search_parents = false) {
    const IOOptionBits options = search_parents ? kIORegistryIterateRecursively | kIORegistryIterateParents : 0;
    CFTypeRef value = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault, options);
    if (!value) return -1;
    int64_t number = -1;
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number);
    } else if (CFGetTypeID(value) == CFDataGetTypeID()) {
        auto data = static_cast<CFDataRef>(value);
        uint32_t raw = 0;
        CFDataGetBytes(data, CFRangeMake(0, std::min<CFIndex>(CFDataGetLength(data), sizeof(raw))),
                       reinterpret_cast<UInt8 *>(&raw));
        number = raw;
    }
    CFRelease(value);
    return number;
}

std::string vendor_name(int64_t vendor_id) {
    switch (vendor_id & 0xffff) {
        case 0x106b: return "Apple";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        case 0x10de: return "NVIDIA";
        default: return {};
    }
}

double percent(CFDictionaryRef stats, CFStringRef key) {
    int64_t value = 0;
    return dict_number(stats, key, value) && value >= 0 ? std::min<double>(value, 100) / 100.0 : -1;
}

template <typename Visit>
void for_each_accelerator(Visit visit) {
    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &iterator) != KERN_SUCCESS)
        return;
    while (io_registry_entry_t accelerator = IOIteratorNext(iterator)) {
        visit(accelerator);
        IOObjectRelease(accelerator);
    }
    IOObjectRelease(iterator);
}

// "pid 477, WindowServer" -> 477.
int32_t creator_pid(io_registry_entry_t client) {
    const std::string creator = property_string(client, CFSTR("IOUserClientCreator"));
    if (creator.rfind("pid ", 0) != 0) return -1;
    return static_cast<int32_t>(std::strtol(creator.c_str() + 4, nullptr, 10));
}

// Sums accumulatedGPUTime (nanoseconds) of every command queue each client reports, by pid.
// Returns false when no client reports usage at all (the driver doesn't publish it).
bool gpu_time_by_pid(std::unordered_map<int32_t, uint64_t> &out) {
    bool published = false;
    for_each_accelerator([&](io_registry_entry_t accelerator) {
        io_iterator_t children = 0;
        if (IORegistryEntryGetChildIterator(accelerator, kIOServicePlane, &children) != KERN_SUCCESS) return;
        while (io_registry_entry_t client = IOIteratorNext(children)) {
            CFTypeRef usage = IORegistryEntryCreateCFProperty(client, CFSTR("AppUsage"), kCFAllocatorDefault, 0);
            if (usage) {
                published = true;
                const int32_t pid = CFGetTypeID(usage) == CFArrayGetTypeID() ? creator_pid(client) : -1;
                if (pid >= 0) {
                    auto queues = static_cast<CFArrayRef>(usage);
                    uint64_t total = 0;
                    for (CFIndex i = 0; i < CFArrayGetCount(queues); ++i) {
                        auto queue = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(queues, i));
                        int64_t time = 0;
                        if (CFGetTypeID(queue) == CFDictionaryGetTypeID() &&
                            dict_number(queue, CFSTR("accumulatedGPUTime"), time) && time > 0)
                            total += static_cast<uint64_t>(time);
                    }
                    out[pid] += total;
                }
                CFRelease(usage);
            }
            IOObjectRelease(client);
        }
        IOObjectRelease(children);
    });
    return published;
}

}  // namespace

std::vector<pc_gpu> gpus() {
    std::vector<pc_gpu> result;
    for_each_accelerator([&](io_registry_entry_t accelerator) {
        pc_gpu gpu{};
        gpu.utilization = gpu.renderer_utilization = gpu.tiler_utilization = -1;
        gpu.encoder_utilization = gpu.decoder_utilization = -1;
        gpu.memory_used = gpu.memory_total = -1;
        gpu.temperature = -1;

        // Apple Silicon names the accelerator itself; discrete GPUs name their PCI device.
        std::string name = property_string(accelerator, CFSTR("model"), true);
        const std::string vendor = vendor_name(property_number(accelerator, CFSTR("vendor-id"), true));
        if (name.empty()) name = vendor.empty() ? "GPU" : vendor + " GPU";
        copy_string(gpu.name, sizeof(gpu.name), name);
        copy_string(gpu.vendor, sizeof(gpu.vendor), vendor);
        gpu.cores = static_cast<int32_t>(std::max<int64_t>(0, property_number(accelerator, CFSTR("gpu-core-count"))));
        gpu.unified_memory = vendor == "Apple" || vendor == "Intel";

        CFTypeRef value =
            IORegistryEntryCreateCFProperty(accelerator, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        if (value && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
            auto stats = static_cast<CFDictionaryRef>(value);
            gpu.utilization = percent(stats, CFSTR("Device Utilization %"));
            if (gpu.utilization < 0) gpu.utilization = percent(stats, CFSTR("GPU Activity(%)"));
            gpu.renderer_utilization = percent(stats, CFSTR("Renderer Utilization %"));
            gpu.tiler_utilization = percent(stats, CFSTR("Tiler Utilization %"));
            int64_t used = 0, free_bytes = 0;
            if (dict_number(stats, CFSTR("In use system memory"), used) ||
                dict_number(stats, CFSTR("vramUsedBytes"), used))
                gpu.memory_used = used;
            if (!gpu.unified_memory && dict_number(stats, CFSTR("vramFreeBytes"), free_bytes) && gpu.memory_used >= 0)
                gpu.memory_total = gpu.memory_used + free_bytes;
        }
        if (value) CFRelease(value);
        if (!gpu.unified_memory && gpu.memory_total < 0) {
            const int64_t vram_mb = property_number(accelerator, CFSTR("VRAM,totalMB"), true);
            if (vram_mb > 0) gpu.memory_total = vram_mb << 20;
        }
        result.push_back(gpu);
    });
    return result;
}

bool process_gpu_available() {
    static const bool available = [] {
        std::unordered_map<int32_t, uint64_t> times;
        return gpu_time_by_pid(times);
    }();
    return available;
}

void process_gpu(std::vector<RawProcess> &processes) {
    if (!process_gpu_available()) return;
    std::unordered_map<int32_t, uint64_t> times;
    gpu_time_by_pid(times);
    for (auto &p : processes) {
        p.has_gpu = true;
        auto it = times.find(p.pid);
        p.gpu_time_ns = it == times.end() ? 0 : it->second;
    }
}

}  // namespace procyon::platform

#endif  // __APPLE__
