// macOS adapter: libproc, mach host statistics, sysctl and IOKit.
#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_info.h>
#include <net/if.h>
#include <net/route.h>
#include <pwd.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../platform.hpp"

namespace procyon::platform {
namespace {

std::string sysctl_string(const char *name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
    value.resize(strnlen(value.c_str(), size));
    return value;
}

template <typename T>
T sysctl_value(const char *name, T fallback = 0) {
    T value{};
    size_t size = sizeof(value);
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? value : fallback;
}

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

// Apple Silicon reports task times in mach absolute units, Intel in nanoseconds.
uint64_t mach_to_ns(uint64_t ticks) {
    static mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    if (timebase.numer == timebase.denom) return ticks;
    return static_cast<uint64_t>(static_cast<__uint128_t>(ticks) * timebase.numer / timebase.denom);
}

std::string model_name() {
    // Apple Silicon exposes the marketing name in the device tree.
    io_registry_entry_t product = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/product");
    if (!product) return {};
    std::string name;
    if (CFTypeRef value = IORegistryEntryCreateCFProperty(product, CFSTR("product-name"), kCFAllocatorDefault, 0)) {
        if (CFGetTypeID(value) == CFDataGetTypeID()) {
            auto data = static_cast<CFDataRef>(value);
            name.assign(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), CFDataGetLength(data));
            name.resize(strnlen(name.c_str(), name.size()));
        }
        CFRelease(value);
    }
    IOObjectRelease(product);
    return name;
}

// Each core's cluster ("P" or "E") from the device tree, by logical CPU id. Hybrid chips don't
// promise an order (M-series list efficiency cores first), so never infer it from the counts.
void read_core_kinds(uint8_t *kinds, size_t capacity) {
    io_registry_entry_t cpus = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/cpus");
    if (!cpus) return;
    io_iterator_t children = 0;
    if (IORegistryEntryGetChildIterator(cpus, kIODeviceTreePlane, &children) == KERN_SUCCESS) {
        while (io_registry_entry_t cpu = IOIteratorNext(children)) {
            int64_t id = -1;
            if (CFTypeRef value =
                    IORegistryEntryCreateCFProperty(cpu, CFSTR("logical-cpu-id"), kCFAllocatorDefault, 0)) {
                if (CFGetTypeID(value) == CFNumberGetTypeID())
                    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &id);
                CFRelease(value);
            }
            if (CFTypeRef value = IORegistryEntryCreateCFProperty(cpu, CFSTR("cluster-type"), kCFAllocatorDefault, 0)) {
                if (CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength(static_cast<CFDataRef>(value)) > 0 &&
                    id >= 0 && static_cast<size_t>(id) < capacity) {
                    const char type = static_cast<char>(CFDataGetBytePtr(static_cast<CFDataRef>(value))[0]);
                    kinds[id] = type == 'P' ? PC_CORE_PERFORMANCE : type == 'E' ? PC_CORE_EFFICIENCY : PC_CORE_UNKNOWN;
                }
                CFRelease(value);
            }
            IOObjectRelease(cpu);
        }
        IOObjectRelease(children);
    }
    IOObjectRelease(cpus);
}

std::string basename_of(const std::string &path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool cf_number_u64(CFDictionaryRef dict, CFStringRef key, uint64_t &out) {
    auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, key));
    return number && CFNumberGetValue(number, kCFNumberSInt64Type, &out);
}

int32_t process_state(int stat) {
    switch (stat) {
        case SRUN: return PC_STATE_RUNNING;
        case SSLEEP: return PC_STATE_SLEEPING;
        case SSTOP: return PC_STATE_STOPPED;
        case SZOMB: return PC_STATE_ZOMBIE;
        default: return PC_STATE_UNKNOWN;
    }
}

int32_t thread_state(int run_state) {
    switch (run_state) {
        case TH_STATE_RUNNING: return PC_STATE_RUNNING;
        case TH_STATE_STOPPED: return PC_STATE_STOPPED;
        case TH_STATE_WAITING:
        case TH_STATE_UNINTERRUPTIBLE: return PC_STATE_SLEEPING;
        default: return PC_STATE_UNKNOWN;
    }
}

pc_result errno_result() {
    switch (errno) {
        case ESRCH: return PC_ERR_NOT_FOUND;
        case EPERM:
        case EACCES: return PC_ERR_PERMISSION;
        case EINVAL: return PC_ERR_INVALID;
        default: return PC_ERR_FAILED;
    }
}

// KERN_PROCARGS2: argc, the exec path, padding NULs, then argv and the environment as C strings.
bool read_arguments(int32_t pid, Details &out) {
    static const int max_size = [] {
        int value = 0;
        size_t size = sizeof(value);
        int mib[2] = {CTL_KERN, KERN_ARGMAX};
        return sysctl(mib, 2, &value, &size, nullptr, 0) == 0 && value > 0 ? value : 1 << 20;
    }();
    std::vector<char> buffer(static_cast<size_t>(max_size));
    size_t size = buffer.size();
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) return false;

    int argc = 0;
    std::memcpy(&argc, buffer.data(), sizeof(argc));
    const char *cursor = buffer.data() + sizeof(argc);
    const char *end = buffer.data() + size;
    cursor += strnlen(cursor, static_cast<size_t>(end - cursor));  // exec path
    while (cursor < end && *cursor == '\0') ++cursor;
    auto next = [&]() {
        const size_t length = strnlen(cursor, static_cast<size_t>(end - cursor));
        std::string value(cursor, length);
        cursor += length + 1;
        return value;
    };
    for (int i = 0; i < argc && cursor < end; ++i) out.arguments.push_back(next());
    while (cursor < end && *cursor != '\0') out.environment.push_back(next());
    out.arguments_known = true;
    return true;
}

bool read_threads(int32_t pid, Details &out) {
    proc_taskinfo task{};
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) != sizeof(task)) return false;
    std::vector<uint64_t> handles(static_cast<size_t>(std::max(task.pti_threadnum, 0)) + 16);
    const int bytes =
        proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, handles.data(), static_cast<int>(handles.size() * sizeof(uint64_t)));
    if (bytes <= 0) return false;
    handles.resize(static_cast<size_t>(bytes) / sizeof(uint64_t));
    for (uint64_t handle : handles) {
        proc_threadinfo info{};
        if (proc_pidinfo(pid, PROC_PIDTHREADINFO, handle, &info, sizeof(info)) != sizeof(info)) continue;
        ThreadInfo thread;
        thread.id = handle;
        thread.name = std::string(info.pth_name, strnlen(info.pth_name, sizeof(info.pth_name)));
        thread.cpu_percent = info.pth_cpu_usage * 100.0 / TH_USAGE_SCALE;
        thread.user_time_ns = info.pth_user_time;
        thread.system_time_ns = info.pth_system_time;
        thread.priority = info.pth_curpri;
        thread.state = thread_state(info.pth_run_state);
        out.threads.push_back(std::move(thread));
    }
    out.threads_known = true;
    return true;
}

}  // namespace

uint32_t capabilities() {
    // No CPU affinity: macOS only takes affinity hints, and none on Apple Silicon.
    uint32_t caps = PC_CAP_PROCESS_DISK_IO | PC_CAP_MEMORY_COMPRESSED | PC_CAP_MEMORY_PRESSURE | PC_CAP_SWAP |
                    PC_CAP_PRIORITY | PC_CAP_SUSPEND | PC_CAP_SIGNALS | PC_CAP_SERVICES | PC_CAP_STARTUP |
                    PC_CAP_OPEN_FILES | PC_CAP_CONNECTIONS;
    // Energy counters stay zero on machines that neither measure nor model them.
    rusage_info_v6 self_usage{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, reinterpret_cast<rusage_info_t *>(&self_usage)) == 0 &&
        self_usage.ri_energy_nj > 0)
        caps |= PC_CAP_PROCESS_ENERGY;
    if (sysctl_value<int32_t>("hw.nperflevels") > 1) caps |= PC_CAP_HYBRID_CORES;
    if (process_network_available()) caps |= PC_CAP_PROCESS_NETWORK;
    if (!gpus().empty()) caps |= PC_CAP_GPU;
    if (process_gpu_available()) caps |= PC_CAP_PROCESS_GPU;
    if (cpu_temperature() >= 0) caps |= PC_CAP_TEMPERATURE;
    pc_battery battery_info;
    if (battery(battery_info) && battery_info.present) caps |= PC_CAP_BATTERY;
    return caps;
}

bool system_info(pc_system_info &out) {
    std::memset(&out, 0, sizeof(out));
    copy_string(out.os_name, sizeof(out.os_name), "macOS");
    copy_string(out.os_version, sizeof(out.os_version), sysctl_string("kern.osproductversion"));
    copy_string(out.os_build, sizeof(out.os_build), sysctl_string("kern.osversion"));
    copy_string(out.kernel, sizeof(out.kernel), sysctl_string("kern.version"));
    char host[256] = {};
    gethostname(host, sizeof(host) - 1);
    copy_string(out.hostname, sizeof(out.hostname), host);
    copy_string(out.model_id, sizeof(out.model_id), sysctl_string("hw.model"));
    copy_string(out.model_name, sizeof(out.model_name), model_name());
    copy_string(out.cpu_brand, sizeof(out.cpu_brand), sysctl_string("machdep.cpu.brand_string"));
#if defined(__arm64__)
    copy_string(out.arch, sizeof(out.arch), "arm64");
#else
    copy_string(out.arch, sizeof(out.arch), "x86_64");
#endif
    out.physical_cores = sysctl_value<int32_t>("hw.physicalcpu");
    out.logical_cores = sysctl_value<int32_t>("hw.logicalcpu");
    if (sysctl_value<int32_t>("hw.nperflevels") > 1) {
        out.performance_cores = sysctl_value<int32_t>("hw.perflevel0.physicalcpu");
        out.efficiency_cores = sysctl_value<int32_t>("hw.perflevel1.physicalcpu");
        read_core_kinds(out.core_kinds, sizeof(out.core_kinds));
    }
    out.cpu_frequency_hz = sysctl_value<uint64_t>("hw.cpufrequency");
    out.memory_total = sysctl_value<uint64_t>("hw.memsize");
    out.boot_time = sysctl_value<timeval>("kern.boottime", timeval{}).tv_sec;
    return true;
}

bool processes(std::vector<RawProcess> &out) {
    out.clear();
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0) return false;
    std::vector<kinfo_proc> kinfo;
    // The process table can grow between the two calls; retry with headroom.
    for (int attempt = 0; attempt < 3; ++attempt) {
        size += size / 4;
        kinfo.resize(size / sizeof(kinfo_proc) + 1);
        size = kinfo.size() * sizeof(kinfo_proc);
        if (sysctl(mib, 3, kinfo.data(), &size, nullptr, 0) == 0) break;
        if (errno != ENOMEM) return false;
    }
    kinfo.resize(size / sizeof(kinfo_proc));
    out.reserve(kinfo.size());

    char path[PROC_PIDPATHINFO_MAXSIZE];
    for (const auto &kp : kinfo) {
        RawProcess p;
        p.pid = kp.kp_proc.p_pid;
        p.ppid = kp.kp_eproc.e_ppid;
        p.uid = kp.kp_eproc.e_ucred.cr_uid;
        p.start_time = kp.kp_proc.p_starttime.tv_sec;
        p.nice = kp.kp_proc.p_nice;  // NOLINT(bugprone-signed-char-misuse): nice is a signed value
        p.state = process_state(kp.kp_proc.p_stat);

        if (proc_pidpath(p.pid, path, sizeof(path)) > 0) p.path = path;
        p.name = p.path.empty() ? std::string(kp.kp_proc.p_comm) : basename_of(p.path);
        if (p.pid == 0) p.name = "kernel_task";

        read_counters(p.pid, p);
        out.push_back(std::move(p));
    }
    return true;
}

bool read_counters(int32_t pid, RawProcess &p) {
    p.restricted = false;
    proc_taskinfo task{};
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) == sizeof(task)) {
        p.cpu_time_ns = mach_to_ns(task.pti_total_user + task.pti_total_system);
        p.threads = task.pti_threadnum;
    } else {
        p.restricted = true;
    }

    // V6 adds the energy the OS bills to the process (CPU, GPU, ...), measured where the SoC has
    // energy counters (Apple Silicon) and modelled elsewhere.
    rusage_info_v6 usage{};
    if (proc_pid_rusage(pid, RUSAGE_INFO_V6, reinterpret_cast<rusage_info_t *>(&usage)) == 0) {
        p.memory_bytes = static_cast<int64_t>(usage.ri_phys_footprint);
        p.has_disk_io = true;
        p.disk_read = usage.ri_diskio_bytesread;
        p.disk_write = usage.ri_diskio_byteswritten;
        p.has_energy = true;
        p.energy_nj = usage.ri_energy_nj;
        if (p.restricted) {
            p.cpu_time_ns = mach_to_ns(usage.ri_user_time + usage.ri_system_time);
            p.restricted = false;
        }
    } else if (!p.restricted) {
        p.memory_bytes = static_cast<int64_t>(task.pti_resident_size);
    }
    return !p.restricted;
}

int64_t start_time(int32_t pid) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    kinfo_proc info{};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return -1;
    return info.kp_proc.p_starttime.tv_sec;
}

bool cpu_ticks(std::vector<CpuTicks> &out) {
    natural_t count = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t info_count = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &info_count) != KERN_SUCCESS)
        return false;
    out.resize(count);
    for (natural_t i = 0; i < count; ++i) {
        const integer_t *ticks = info + CPU_STATE_MAX * i;
        out[i].user = static_cast<uint32_t>(ticks[CPU_STATE_USER]);
        out[i].system = static_cast<uint32_t>(ticks[CPU_STATE_SYSTEM]);
        out[i].idle = static_cast<uint32_t>(ticks[CPU_STATE_IDLE]);
        out[i].nice = static_cast<uint32_t>(ticks[CPU_STATE_NICE]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info), info_count * sizeof(integer_t));
    return true;
}

bool memory(Memory &out) {
    out = {};
    out.total = sysctl_value<uint64_t>("hw.memsize");

    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
        KERN_SUCCESS)
        return false;
    const uint64_t page = vm_kernel_page_size;

    // Same breakdown as Activity Monitor.
    out.app = (vm.internal_page_count - vm.purgeable_count) * page;
    out.wired = vm.wire_count * page;
    out.compressed = vm.compressor_page_count * page;
    out.cached = (vm.external_page_count + vm.purgeable_count) * page;
    out.used = out.app + out.wired + out.compressed;
    out.free = out.total > out.used + out.cached ? out.total - out.used - out.cached : 0;

    xsw_usage swap{};
    size_t size = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0) {
        out.swap_total = swap.xsu_total;
        out.swap_used = swap.xsu_used;
    }

    switch (sysctl_value<int32_t>("kern.memorystatus_vm_pressure_level", -1)) {
        case 1: out.pressure = PC_PRESSURE_NORMAL; break;
        case 2: out.pressure = PC_PRESSURE_WARNING; break;
        case 4: out.pressure = PC_PRESSURE_CRITICAL; break;
        default: out.pressure = PC_PRESSURE_UNKNOWN; break;
    }
    return true;
}

bool io_counters(IoCounters &out) {
    out = {};

    io_iterator_t drivers = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOBlockStorageDriverClass), &drivers) ==
        KERN_SUCCESS) {
        while (io_registry_entry_t driver = IOIteratorNext(drivers)) {
            CFMutableDictionaryRef props = nullptr;
            if (IORegistryEntryCreateCFProperties(driver, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
                auto stats = static_cast<CFDictionaryRef>(
                    CFDictionaryGetValue(props, CFSTR(kIOBlockStorageDriverStatisticsKey)));
                uint64_t value = 0;
                if (stats && cf_number_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsBytesReadKey), value))
                    out.disk_read += value;
                if (stats && cf_number_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsBytesWrittenKey), value))
                    out.disk_write += value;
                CFRelease(props);
            }
            IOObjectRelease(driver);
        }
        IOObjectRelease(drivers);
    }

    int mib[6] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
    size_t size = 0;
    if (sysctl(mib, 6, nullptr, &size, nullptr, 0) == 0) {
        std::vector<char> buffer(size);
        if (sysctl(mib, 6, buffer.data(), &size, nullptr, 0) == 0) {
            for (size_t offset = 0; offset + sizeof(if_msghdr) <= size;) {
                auto header = reinterpret_cast<const if_msghdr *>(buffer.data() + offset);
                if (header->ifm_msglen == 0) break;
                if (header->ifm_type == RTM_IFINFO2) {
                    auto info = reinterpret_cast<const if_msghdr2 *>(header);
                    if (!(info->ifm_flags & IFF_LOOPBACK)) {
                        out.net_rx += info->ifm_data.ifi_ibytes;
                        out.net_tx += info->ifm_data.ifi_obytes;
                    }
                }
                offset += header->ifm_msglen;
            }
        }
    }
    return true;
}

void load_average(double out[3]) {
    if (getloadavg(out, 3) != 3) out[0] = out[1] = out[2] = 0;
}

std::vector<pc_volume> volumes() {
    std::vector<pc_volume> result;
    struct statfs *mounts = nullptr;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    for (int i = 0; i < count; ++i) {
        const auto &fs = mounts[i];
        if (!(fs.f_flags & MNT_LOCAL) || (fs.f_flags & MNT_DONTBROWSE)) continue;

        pc_volume volume{};
        copy_string(volume.mount_point, sizeof(volume.mount_point), fs.f_mntonname);
        copy_string(volume.file_system, sizeof(volume.file_system), fs.f_fstypename);
        volume.is_root = std::strcmp(fs.f_mntonname, "/") == 0;
        volume.is_removable = !(fs.f_flags & MNT_ROOTFS) && std::strncmp(fs.f_mntonname, "/Volumes/", 9) == 0;
        volume.total_bytes = static_cast<uint64_t>(fs.f_blocks) * fs.f_bsize;
        volume.available_bytes = static_cast<uint64_t>(fs.f_bavail) * fs.f_bsize;
        std::string name = volume.is_root ? "Macintosh HD" : basename_of(fs.f_mntonname);

        // Finder-consistent name and free space (includes purgeable space).
        if (CFURLRef url = CFURLCreateFromFileSystemRepresentation(
                kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(fs.f_mntonname),
                static_cast<CFIndex>(std::strlen(fs.f_mntonname)), true)) {
            CFTypeRef value = nullptr;
            if (CFURLCopyResourcePropertyForKey(url, kCFURLVolumeNameKey, static_cast<void *>(&value), nullptr) &&
                value) {
                char buffer[256];
                if (CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer), kCFStringEncodingUTF8))
                    name = buffer;
                CFRelease(value);
            }
            value = nullptr;
            if (CFURLCopyResourcePropertyForKey(url, kCFURLVolumeAvailableCapacityForImportantUsageKey,
                                                static_cast<void *>(&value), nullptr) &&
                value) {
                int64_t available = 0;
                if (CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &available) && available > 0)
                    volume.available_bytes = static_cast<uint64_t>(available);
                CFRelease(value);
            }
            CFRelease(url);
        }
        copy_string(volume.name, sizeof(volume.name), name);
        result.push_back(volume);
    }
    return result;
}

std::string user_name(uint32_t uid) {
    static std::mutex mutex;
    static std::unordered_map<uint32_t, std::string> cache;
    std::lock_guard lock(mutex);
    if (auto it = cache.find(uid); it != cache.end()) return it->second;
    passwd *pw = getpwuid(uid);
    std::string name = pw ? pw->pw_name : std::to_string(uid);
    cache.emplace(uid, name);
    return name;
}

AppIdentity app_identity(const RawProcess &process) {
    // Group helpers with their host app: the outermost ".app/" in the path wins,
    // so ".../Google Chrome.app/.../Google Chrome Helper.app/..." maps to Chrome.
    auto pos = process.path.find(".app/");
    if (pos != std::string::npos) {
        std::string bundle = process.path.substr(0, pos + 4);
        std::string name = basename_of(bundle);
        name.resize(name.size() - 4);
        return {bundle, name};
    }
    return {"exe:" + process.name, process.name};
}

bool is_system_process(const RawProcess &process) {
    if (process.uid < 500) return true;  // root and daemon accounts (_spotlight, _windowserver, …)
    return process.path.rfind("/System/", 0) == 0 || process.path.rfind("/usr/libexec/", 0) == 0 ||
           process.path.rfind("/usr/sbin/", 0) == 0;
}

int32_t self_pid() { return getpid(); }

pc_result signal_process(int32_t pid, bool force) { return send_signal(pid, force ? SIGKILL : SIGTERM); }

bool valid_signal(int32_t signal) { return signal > 0 && signal < NSIG; }

pc_result send_signal(int32_t pid, int32_t signal) {
    if (!valid_signal(signal)) return PC_ERR_INVALID;
    return kill(pid, signal) == 0 ? PC_OK : errno_result();
}

pc_result set_priority(int32_t pid, int32_t nice) {
    if (nice < PRIO_MIN || nice > PRIO_MAX) return PC_ERR_INVALID;
    errno = 0;
    return setpriority(PRIO_PROCESS, static_cast<id_t>(pid), nice) == 0 ? PC_OK : errno_result();
}

pc_result set_affinity(int32_t, uint64_t) { return PC_ERR_UNSUPPORTED; }

bool process_details(int32_t pid, Details &out) {
    out = {};
    proc_vnodepathinfo vnode{};
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vnode, sizeof(vnode)) == sizeof(vnode))
        out.cwd = vnode.pvi_cdir.vip_path;
    read_arguments(pid, out);
    read_threads(pid, out);
    return out.complete();
}

}  // namespace procyon::platform

#endif  // __APPLE__
