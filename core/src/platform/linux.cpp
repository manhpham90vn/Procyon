// Linux adapter: procfs and sysfs. Every per-process file used here is world-readable except
// /proc/<pid>/io, exe and environ, which the kernel only shows to the process's owner (or root).
#if defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "linux_internal.hpp"

extern char **environ;

namespace procyon::platform {

namespace linux_internal {

bool read_file(const std::string &path, std::string &out) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            const bool ok = n == 0;
            ::close(fd);
            return ok;
        }
        out.append(buffer, static_cast<size_t>(n));
    }
}

std::string read_line(const std::string &path) {
    std::string text;
    if (!read_file(path, text)) return {};
    const auto end = text.find('\n');
    if (end != std::string::npos) text.resize(end);
    return text;
}

int64_t read_int(const std::string &path, int64_t fallback) {
    const std::string text = read_line(path);
    if (text.empty()) return fallback;
    char *end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    return errno == 0 && end != text.c_str() ? value : fallback;
}

std::string read_link(const std::string &path) {
    char buffer[4096];
    const ssize_t n = ::readlink(path.c_str(), buffer, sizeof(buffer) - 1);
    if (n <= 0) return {};
    return std::string(buffer, static_cast<size_t>(n));
}

bool exists(const std::string &path) { return ::access(path.c_str(), F_OK) == 0; }

std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> names;
    DIR *dir = ::opendir(path.c_str());
    if (!dir) return names;
    while (dirent *entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        names.emplace_back(entry->d_name);
    }
    ::closedir(dir);
    return names;
}

std::vector<int32_t> list_pids() {
    std::vector<int32_t> pids;
    DIR *dir = ::opendir("/proc");
    if (!dir) return pids;
    while (dirent *entry = ::readdir(dir)) {
        const char *name = entry->d_name;
        if (*name < '1' || *name > '9') continue;
        char *end = nullptr;
        const long pid = std::strtol(name, &end, 10);
        if (*end == '\0' && pid > 0) pids.push_back(static_cast<int32_t>(pid));
    }
    ::closedir(dir);
    return pids;
}

std::string trim(std::string_view text) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t at = text.find(separator, start);
        parts.emplace_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return parts;
}

std::vector<std::string> split_whitespace(std::string_view text) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        const size_t start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        if (i > start) parts.emplace_back(text.substr(start, i - start));
    }
    return parts;
}

bool starts_with(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }

std::string basename_of(const std::string &path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

int run(const std::vector<std::string> &argv, std::string &out, std::string *err, int timeout_ms) {
    out.clear();
    if (err) err->clear();
    if (argv.empty()) return -1;
    int out_pipe[2], err_pipe[2];
    if (::pipe2(out_pipe, O_CLOEXEC) != 0) return -1;
    if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);

    // Tools answer in English whatever the user's locale, so their output can be parsed.
    std::vector<std::string> env_storage;
    for (char **e = environ; e && *e; ++e)
        if (!starts_with(*e, "LC_ALL=") && !starts_with(*e, "LANG=") && !starts_with(*e, "LANGUAGE="))
            env_storage.emplace_back(*e);
    env_storage.emplace_back("LC_ALL=C");
    std::vector<char *> envp;
    for (auto &e : env_storage) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::vector<char *> args;
    std::vector<std::string> arg_storage = argv;
    for (auto &a : arg_storage) args.push_back(a.data());
    args.push_back(nullptr);

    pid_t child = 0;
    const int spawned = ::posix_spawnp(&child, args[0], &actions, nullptr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    if (spawned != 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        return -1;
    }

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    int open_fds = 2;
    bool timed_out = false;
    char buffer[8192];
    while (open_fds > 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) {
            timed_out = true;
            break;
        }
        const int ready = ::poll(fds, 2, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            timed_out = ready == 0;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = ::read(fds[i].fd, buffer, sizeof(buffer));
            if (n > 0) {
                if (i == 0)
                    out.append(buffer, static_cast<size_t>(n));
                else if (err)
                    err->append(buffer, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                --open_fds;
            }
        }
    }
    for (auto &f : fds)
        if (f.fd >= 0) ::close(f.fd);
    if (timed_out) ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (timed_out || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

std::string find_program(const std::string &name) {
    std::vector<std::string> dirs;
    if (const char *path = std::getenv("PATH")) dirs = split(path, ':');
    for (const char *d : {"/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"}) dirs.emplace_back(d);
    for (const auto &dir : dirs) {
        if (dir.empty()) continue;
        const std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return {};
}

uint64_t clock_ticks() {
    static const uint64_t hz = [] {
        const long value = ::sysconf(_SC_CLK_TCK);
        return value > 0 ? static_cast<uint64_t>(value) : 100;
    }();
    return hz;
}

int64_t boot_time() {
    static const int64_t btime = [] {
        std::string text;
        if (!read_file("/proc/stat", text)) return int64_t{0};
        const auto at = text.find("\nbtime ");
        return at == std::string::npos ? int64_t{0}
                                       : static_cast<int64_t>(std::strtoll(text.c_str() + at + 7, nullptr, 10));
    }();
    return btime;
}

bool parse_stat(const std::string &text, ProcStat &out) {
    // "pid (comm) state ppid ...": comm may hold spaces and parentheses, so it ends at the last ')'.
    const auto open = text.find('(');
    const auto close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    out.pid = static_cast<int32_t>(std::strtol(text.c_str(), nullptr, 10));
    out.comm = text.substr(open + 1, close - open - 1);
    const auto fields = split_whitespace(std::string_view(text).substr(close + 1));
    if (fields.size() < 20) return false;
    auto u64 = [&](size_t i) { return std::strtoull(fields[i].c_str(), nullptr, 10); };
    auto i32 = [&](size_t i) { return static_cast<int32_t>(std::strtol(fields[i].c_str(), nullptr, 10)); };
    out.state = fields[0].empty() ? '?' : fields[0][0];
    out.ppid = i32(1);
    out.flags = static_cast<uint32_t>(u64(6));
    out.utime = u64(11);
    out.stime = u64(12);
    out.priority = i32(15);
    out.nice = i32(16);
    out.threads = i32(17);
    out.start_ticks = u64(19);
    return true;
}

bool read_stat(int32_t pid, ProcStat &out) {
    std::string text;
    return read_file("/proc/" + std::to_string(pid) + "/stat", text) && parse_stat(text, out);
}

int32_t state_of(char state) {
    switch (state) {
        case 'R': return PC_STATE_RUNNING;
        case 'S':
        case 'D':
        case 'I': return PC_STATE_SLEEPING;
        case 'T':
        case 't': return PC_STATE_STOPPED;
        case 'Z':
        case 'X': return PC_STATE_ZOMBIE;
        default: return PC_STATE_UNKNOWN;
    }
}

}  // namespace linux_internal

using namespace linux_internal;

namespace {

pc_result errno_result() {
    switch (errno) {
        case ESRCH: return PC_ERR_NOT_FOUND;
        case EPERM:
        case EACCES: return PC_ERR_PERMISSION;
        case EINVAL: return PC_ERR_INVALID;
        default: return PC_ERR_FAILED;
    }
}

std::string proc_path(int32_t pid, const char *leaf) { return "/proc/" + std::to_string(pid) + "/" + leaf; }

uint64_t ticks_to_ns(uint64_t ticks) { return ticks * (1000000000ull / clock_ticks()); }

int64_t start_seconds(const ProcStat &stat) {
    return boot_time() + static_cast<int64_t>(stat.start_ticks / clock_ticks());
}

uint64_t page_size() {
    static const uint64_t size = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
    return size;
}

// Value of "Key:   1234 kB" lines (meminfo, status), in the unit written.
std::unordered_map<std::string, uint64_t> parse_key_values(const std::string &text) {
    std::unordered_map<std::string, uint64_t> values;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const auto colon = text.find(':', start);
        if (colon != std::string::npos && colon < end)
            values[text.substr(start, colon - start)] = std::strtoull(text.c_str() + colon + 1, nullptr, 10);
        start = end + 1;
    }
    return values;
}

// The command line as its NUL-separated arguments; false when unreadable. Kernel threads and
// zombies have an empty one.
bool read_cmdline(int32_t pid, std::vector<std::string> &out) {
    out.clear();
    std::string text;
    if (!read_file(proc_path(pid, "cmdline"), text)) return false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\0', start);
        if (end == std::string::npos) end = text.size();
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return true;
}

// Name and path don't change while a process runs (exec gives it a new start time only on some
// kernels, so the comm is part of the key): resolve them once per process, not on every tick.
struct Identity {
    std::string name;
    std::string path;
};

std::mutex identity_mutex;
std::unordered_map<uint64_t, Identity> identity_cache;

uint64_t identity_key(int32_t pid, uint64_t start_ticks) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(pid)) << 32) ^ start_ticks;
}

Identity resolve_identity(int32_t pid, const ProcStat &stat) {
    Identity id;
    id.name = stat.comm;
    std::string exe = read_link(proc_path(pid, "exe"));
    const std::string deleted = " (deleted)";
    if (exe.size() > deleted.size() && exe.compare(exe.size() - deleted.size(), deleted.size(), deleted) == 0)
        exe.resize(exe.size() - deleted.size());
    std::vector<std::string> argv;
    if (exe.empty() && read_cmdline(pid, argv) && !argv.empty() && !argv[0].empty() && argv[0][0] == '/') {
        // Another user's process: its exe link is hidden, but the command line is public.
        const auto space = argv[0].find(' ');
        exe = space == std::string::npos ? argv[0] : argv[0].substr(0, space);
    }
    id.path = exe;
    // comm is cut at 15 bytes: take the full name from the executable when it starts the same way.
    if (id.name.size() >= 15) {
        std::string full = basename_of(exe);
        if (full.empty() && (argv.empty() ? read_cmdline(pid, argv) : true) && !argv.empty())
            full = basename_of(argv[0]);
        if (starts_with(full, id.name)) id.name = full;
    }
    return id;
}

}  // namespace

uint32_t capabilities() {
    uint32_t caps = PC_CAP_PROCESS_DISK_IO | PC_CAP_SWAP | PC_CAP_PRIORITY | PC_CAP_SUSPEND | PC_CAP_SIGNALS |
                    PC_CAP_CPU_AFFINITY | PC_CAP_STARTUP | PC_CAP_OPEN_FILES | PC_CAP_CONNECTIONS;
    if (exists("/proc/pressure/memory")) caps |= PC_CAP_MEMORY_PRESSURE;
    if (read_int("/sys/module/zswap/parameters/enabled", 0) == 1 ||
        read_line("/sys/module/zswap/parameters/enabled") == "Y")
        caps |= PC_CAP_MEMORY_COMPRESSED;
    if (exists("/sys/devices/cpu_core/cpus") && exists("/sys/devices/cpu_atom/cpus")) caps |= PC_CAP_HYBRID_CORES;
    if (!gpus().empty()) caps |= PC_CAP_GPU;
    if (process_gpu_available()) caps |= PC_CAP_PROCESS_GPU;
    if (process_network_available()) caps |= PC_CAP_PROCESS_NETWORK;
    double cpu = -1, disk = -1;
    temperatures(cpu, disk);
    if (cpu >= 0 || disk >= 0) caps |= PC_CAP_TEMPERATURE;
    pc_battery battery_info{};
    if (battery(battery_info) && battery_info.present) caps |= PC_CAP_BATTERY;
    if (!find_program("systemctl").empty() && exists("/run/systemd/system")) caps |= PC_CAP_SERVICES;
    return caps;
}

namespace {

// "0-3,8,10-11" → the listed CPU numbers.
std::vector<int> parse_cpu_list(const std::string &text) {
    std::vector<int> cpus;
    for (const auto &part : split(trim(text), ',')) {
        if (part.empty()) continue;
        const auto dash = part.find('-');
        const int first = std::atoi(part.c_str());
        const int last = dash == std::string::npos ? first : std::atoi(part.c_str() + dash + 1);
        for (int c = first; c <= last && c < 4096; ++c) cpus.push_back(c);
    }
    return cpus;
}

std::map<std::string, std::string> os_release() {
    std::map<std::string, std::string> values;
    std::string text;
    if (!read_file("/etc/os-release", text) && !read_file("/usr/lib/os-release", text)) return values;
    for (const auto &line : split(text, '\n')) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string value = line.substr(eq + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        values[line.substr(0, eq)] = value;
    }
    return values;
}

}  // namespace

bool system_info(pc_system_info &out) {
    out = {};
    auto release = os_release();
    copy_string(out.os_name, sizeof(out.os_name), release.count("NAME") ? release["NAME"] : "Linux");
    copy_string(out.os_version, sizeof(out.os_version),
                release.count("VERSION") ? release["VERSION"] : release["VERSION_ID"]);
    copy_string(out.os_build, sizeof(out.os_build),
                release.count("BUILD_ID") ? release["BUILD_ID"] : release["VERSION_CODENAME"]);
    utsname uts{};
    if (::uname(&uts) == 0) {
        copy_string(out.kernel, sizeof(out.kernel), std::string("Linux ") + uts.release);
        copy_string(out.hostname, sizeof(out.hostname), uts.nodename);
        copy_string(out.arch, sizeof(out.arch), uts.machine);
    }
    const std::string vendor = trim(read_line("/sys/class/dmi/id/sys_vendor"));
    const std::string product = trim(read_line("/sys/class/dmi/id/product_name"));
    const std::string version = trim(read_line("/sys/class/dmi/id/product_version"));
    // Boards that ship without a filled-in DMI table say so in many ways.
    auto placeholder = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s.empty() || s == "to be filled by o.e.m." || s == "system product name" || s == "default string" ||
               s == "none" || s == "o.e.m.";
    };
    copy_string(out.model_id, sizeof(out.model_id), placeholder(product) ? "" : product);
    std::string model = placeholder(product) ? "" : product;
    if (!model.empty() && !placeholder(vendor) && !starts_with(model, vendor)) model = vendor + " " + model;
    if (!model.empty() && !placeholder(version) && version.find("ThinkPad") != std::string::npos) model = version;
    copy_string(out.model_name, sizeof(out.model_name), model);

    std::string cpuinfo;
    read_file("/proc/cpuinfo", cpuinfo);
    for (const auto &line : split(cpuinfo, '\n')) {
        if (starts_with(line, "model name") || starts_with(line, "Model")) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                copy_string(out.cpu_brand, sizeof(out.cpu_brand), trim(line.substr(colon + 1)));
                break;
            }
        }
    }

    // Logical CPUs and physical cores from the topology: one core per distinct (package, core id).
    const auto online = parse_cpu_list(read_line("/sys/devices/system/cpu/online"));
    out.logical_cores =
        online.empty() ? static_cast<int32_t>(::sysconf(_SC_NPROCESSORS_ONLN)) : static_cast<int32_t>(online.size());
    std::set<std::pair<int64_t, int64_t>> cores;
    for (int cpu : online) {
        const std::string topo = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        cores.insert({read_int(topo + "physical_package_id", 0), read_int(topo + "core_id", cpu)});
    }
    out.physical_cores = cores.empty() ? out.logical_cores : static_cast<int32_t>(cores.size());

    // Intel hybrid parts list their P- and E-cores as two PMUs.
    const auto p_cpus = parse_cpu_list(read_line("/sys/devices/cpu_core/cpus"));
    const auto e_cpus = parse_cpu_list(read_line("/sys/devices/cpu_atom/cpus"));
    if (!p_cpus.empty() && !e_cpus.empty()) {
        std::set<std::pair<int64_t, int64_t>> p_cores, e_cores;
        for (size_t i = 0; i < online.size() && i < sizeof(out.core_kinds); ++i) {
            const int cpu = online[i];
            const std::string topo = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
            const std::pair<int64_t, int64_t> core{read_int(topo + "physical_package_id", 0),
                                                   read_int(topo + "core_id", cpu)};
            if (std::find(p_cpus.begin(), p_cpus.end(), cpu) != p_cpus.end()) {
                out.core_kinds[i] = PC_CORE_PERFORMANCE;
                p_cores.insert(core);
            } else if (std::find(e_cpus.begin(), e_cpus.end(), cpu) != e_cpus.end()) {
                out.core_kinds[i] = PC_CORE_EFFICIENCY;
                e_cores.insert(core);
            }
        }
        out.performance_cores = static_cast<int32_t>(p_cores.size());
        out.efficiency_cores = static_cast<int32_t>(e_cores.size());
    }

    const int64_t khz = read_int("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", 0);
    out.cpu_frequency_hz = khz > 0 ? static_cast<uint64_t>(khz) * 1000 : 0;
    std::string meminfo;
    if (read_file("/proc/meminfo", meminfo)) out.memory_total = parse_key_values(meminfo)["MemTotal"] * 1024;
    out.boot_time = boot_time();
    return true;
}

bool process_parents(std::vector<ProcessParent> &out) {
    out.clear();
    ProcStat stat;
    for (int32_t pid : list_pids())
        if (read_stat(pid, stat)) out.push_back({pid, stat.ppid});
    return !out.empty();
}

namespace {

// Everything one tick needs about a process, from stat, statm and io.
bool read_process(int32_t pid, const ProcStat &stat, RawProcess &p) {
    p.pid = pid;
    p.ppid = stat.ppid;
    p.start_time = start_seconds(stat);
    p.cpu_time_ns = ticks_to_ns(stat.utime + stat.stime);
    p.threads = stat.threads;
    p.nice = stat.nice;
    p.state = state_of(stat.state);

    struct stat info{};
    if (::stat(("/proc/" + std::to_string(pid)).c_str(), &info) != 0) return false;
    p.uid = info.st_uid;

    // Resident memory minus the file-backed and shared pages: what the process itself holds.
    std::string statm;
    if (read_file(proc_path(pid, "statm"), statm)) {
        unsigned long long size = 0, resident = 0, shared = 0;
        if (std::sscanf(statm.c_str(), "%llu %llu %llu", &size, &resident, &shared) == 3)
            p.memory_bytes = static_cast<int64_t>((resident > shared ? resident - shared : 0) * page_size());
    }

    // read_bytes/write_bytes are what reached the block layer; only the owner (or root) may read them.
    std::string io;
    if (read_file(proc_path(pid, "io"), io)) {
        auto values = parse_key_values(io);
        p.has_disk_io = true;
        p.disk_read = values["read_bytes"];
        p.disk_write = values["write_bytes"];
    }
    return true;
}

}  // namespace

bool processes(std::vector<RawProcess> &out) {
    out.clear();
    const auto pids = list_pids();
    out.reserve(pids.size());
    std::unordered_map<uint64_t, Identity> seen;
    seen.reserve(pids.size());
    std::lock_guard lock(identity_mutex);
    ProcStat stat;
    for (int32_t pid : pids) {
        if (!read_stat(pid, stat)) continue;
        // Kernel threads (kworker, ksoftirqd, …) have no program, memory or files: like macOS's
        // kernel_task they would be noise in a list of programs.
        if (stat.kernel_thread() || pid == 2) continue;
        RawProcess p;
        if (!read_process(pid, stat, p)) continue;
        const uint64_t key = identity_key(pid, stat.start_ticks);
        auto cached = identity_cache.find(key);
        Identity id = cached != identity_cache.end() && cached->second.name.rfind(stat.comm, 0) == 0
                          ? cached->second
                          : resolve_identity(pid, stat);
        p.name = id.name;
        p.path = id.path;
        seen.emplace(key, std::move(id));
        out.push_back(std::move(p));
    }
    identity_cache = std::move(seen);
    return !out.empty();
}

bool read_counters(int32_t pid, RawProcess &out) {
    ProcStat stat;
    if (!read_stat(pid, stat)) return false;
    return read_process(pid, stat, out);
}

int64_t start_time(int32_t pid) {
    ProcStat stat;
    if (!read_stat(pid, stat)) return exists("/proc/" + std::to_string(pid)) ? 0 : -1;
    return start_seconds(stat);
}

bool cpu_ticks(std::vector<CpuTicks> &out) {
    out.clear();
    std::string text;
    if (!read_file("/proc/stat", text)) return false;
    for (const auto &line : split(text, '\n')) {
        if (!starts_with(line, "cpu") || line.size() < 4 || !std::isdigit(static_cast<unsigned char>(line[3])))
            continue;
        const auto f = split_whitespace(line);
        if (f.size() < 9) continue;
        auto v = [&](size_t i) { return std::strtoull(f[i].c_str(), nullptr, 10); };
        // user nice system idle iowait irq softirq steal. Waiting on I/O is idle time, interrupt
        // and stolen time is time the core wasn't free for us. The counters are truncated to 32
        // bits like the other kernels': deltas are taken modulo 2^32.
        CpuTicks t;
        t.user = static_cast<uint32_t>(v(1));
        t.nice = static_cast<uint32_t>(v(2));
        t.system = static_cast<uint32_t>(v(3) + v(6) + v(7) + v(8));
        t.idle = static_cast<uint32_t>(v(4) + v(5));
        out.push_back(t);
    }
    return !out.empty();
}

namespace {

// "some avg10=1.23 avg60=…": the share of the last 10 s some task waited for memory.
double memory_pressure_avg10() {
    const std::string line = read_line("/proc/pressure/memory");
    const auto at = line.find("avg10=");
    return at == std::string::npos ? -1 : std::strtod(line.c_str() + at + 6, nullptr);
}

}  // namespace

bool memory(Memory &out) {
    out = {};
    std::string text;
    if (!read_file("/proc/meminfo", text)) return false;
    auto kb = parse_key_values(text);
    auto bytes = [&](const char *key) { return kb[key] * 1024; };
    out.total = bytes("MemTotal");
    const uint64_t available = kb.count("MemAvailable") ? bytes("MemAvailable") : bytes("MemFree") + bytes("Cached");
    out.free = bytes("MemFree");
    // Page cache the kernel can drop at once (tmpfs/shm pages can't be dropped: they are in use).
    const uint64_t shmem = bytes("Shmem");
    const uint64_t cache = bytes("Cached") + bytes("Buffers") + bytes("SReclaimable");
    out.cached = cache > shmem ? cache - shmem : 0;
    out.used = out.total > available ? out.total - available : 0;
    // Kernel memory that can't be paged out, like macOS's "wired".
    out.wired = bytes("SUnreclaim") + bytes("KernelStack") + bytes("PageTables") + bytes("Unevictable");
    // zswap's compressed pool sits in RAM (Zswap:), zram counts as a swap device instead.
    out.compressed = bytes("Zswap");
    out.app = out.used > out.wired + out.compressed ? out.used - out.wired - out.compressed : 0;
    out.swap_total = bytes("SwapTotal");
    const uint64_t swap_free = bytes("SwapFree");
    out.swap_used = out.swap_total > swap_free ? out.swap_total - swap_free : 0;

    // Pressure from PSI when the kernel has it: how much time tasks stalled waiting for memory.
    const double stall = memory_pressure_avg10();
    if (stall >= 0)
        out.pressure = stall >= 25 ? PC_PRESSURE_CRITICAL : stall >= 5 ? PC_PRESSURE_WARNING : PC_PRESSURE_NORMAL;
    else
        out.pressure = PC_PRESSURE_UNKNOWN;
    return out.total > 0;
}

namespace {

// Physical disks only: partitions, loop, device-mapper, md and zram devices would count the same
// bytes twice (or count memory as disk).
bool is_physical_disk(const std::string &name) {
    if (starts_with(name, "loop") || starts_with(name, "ram") || starts_with(name, "zram") ||
        starts_with(name, "dm-") || starts_with(name, "md") || starts_with(name, "sr") || starts_with(name, "fd"))
        return false;
    return exists("/sys/block/" + name + "/device");
}

// Interfaces backed by hardware (wired, Wi-Fi, USB tethering). Loopback, bridges, VPN tunnels and
// container veths carry traffic already counted on a physical interface.
bool counts_toward_machine_traffic(const std::string &name) {
    if (name == "lo") return false;
    return exists("/sys/class/net/" + name + "/device");
}

}  // namespace

bool io_counters(IoCounters &out) {
    out = {};
    std::string text;
    bool any = false;
    if (read_file("/proc/diskstats", text)) {
        for (const auto &line : split(text, '\n')) {
            const auto f = split_whitespace(line);
            if (f.size() < 10 || !is_physical_disk(f[2])) continue;
            // Sectors are always 512 bytes in diskstats, whatever the device's sector size.
            out.disk_read += std::strtoull(f[5].c_str(), nullptr, 10) * 512;
            out.disk_write += std::strtoull(f[9].c_str(), nullptr, 10) * 512;
            any = true;
        }
    }
    if (read_file("/proc/net/dev", text)) {
        for (const auto &line : split(text, '\n')) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = trim(line.substr(0, colon));
            if (!counts_toward_machine_traffic(name)) continue;
            const auto f = split_whitespace(line.substr(colon + 1));
            if (f.size() < 9) continue;
            out.net_rx += std::strtoull(f[0].c_str(), nullptr, 10);
            out.net_tx += std::strtoull(f[8].c_str(), nullptr, 10);
            any = true;
        }
    }
    return any;
}

void load_average(double out[3]) {
    if (::getloadavg(out, 3) != 3) out[0] = out[1] = out[2] = 0;
}

namespace {

// /proc/mounts escapes spaces, tabs, newlines and backslashes as octal.
std::string unescape_mount(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            out += static_cast<char>(std::strtol(text.substr(i + 1, 3).c_str(), nullptr, 8));
            i += 3;
        } else {
            out += text[i];
        }
    }
    return out;
}

// The kernel's name of the whole disk a partition belongs to ("nvme0n1p2" → "nvme0n1").
std::string parent_disk(const std::string &device_name) {
    const std::string link = read_link("/sys/class/block/" + device_name);
    if (link.empty()) return device_name;
    const auto parts = split(link, '/');
    if (exists("/sys/class/block/" + device_name + "/partition") && parts.size() >= 2) return parts[parts.size() - 2];
    return device_name;
}

bool removable_disk(const std::string &device_name) {
    const std::string disk = parent_disk(device_name);
    if (read_int("/sys/block/" + disk + "/removable", 0) == 1) return true;
    return read_link("/sys/class/block/" + disk).find("/usb") != std::string::npos;
}

std::unordered_map<std::string, std::string> volume_labels() {
    std::unordered_map<std::string, std::string> labels;  // device realpath → label
    for (const auto &name : list_dir("/dev/disk/by-label")) {
        char resolved[PATH_MAX];
        if (::realpath(("/dev/disk/by-label/" + name).c_str(), resolved)) labels[resolved] = unescape_mount(name);
    }
    return labels;
}

}  // namespace

std::vector<pc_volume> volumes() {
    static const std::set<std::string> kFileSystems = {"ext2", "ext3",     "ext4",    "xfs",      "btrfs",   "f2fs",
                                                       "vfat", "exfat",    "ntfs",    "ntfs3",    "fuseblk", "zfs",
                                                       "jfs",  "reiserfs", "hfsplus", "bcachefs", "nilfs2"};
    std::vector<pc_volume> result;
    std::string text;
    if (!read_file("/proc/self/mounts", text)) return result;
    const auto labels = volume_labels();
    std::set<std::string> devices;
    for (const auto &line : split(text, '\n')) {
        const auto f = split_whitespace(line);
        if (f.size() < 3) continue;
        const std::string device = unescape_mount(f[0]);
        const std::string mount = unescape_mount(f[1]);
        const std::string &fs = f[2];
        if (!kFileSystems.count(fs) && fs != "zfs") continue;
        if (fs != "zfs" && !starts_with(device, "/dev/")) continue;
        if (starts_with(device, "/dev/loop")) continue;  // snaps and disk images
        if (mount == "/boot" || starts_with(mount, "/boot/") || starts_with(mount, "/snap/")) continue;
        // btrfs subvolumes and bind mounts mount one device several times: show it once.
        char resolved[PATH_MAX];
        const std::string real = ::realpath(device.c_str(), resolved) ? std::string(resolved) : device;
        if (!devices.insert(real).second) continue;
        struct statvfs stats{};
        if (::statvfs(mount.c_str(), &stats) != 0 || stats.f_blocks == 0) continue;
        pc_volume v{};
        auto label = labels.find(real);
        std::string name = label != labels.end() ? label->second : mount == "/" ? "System" : basename_of(mount);
        copy_string(v.name, sizeof(v.name), name);
        copy_string(v.mount_point, sizeof(v.mount_point), mount);
        copy_string(v.file_system, sizeof(v.file_system), fs);
        v.total_bytes = static_cast<uint64_t>(stats.f_blocks) * stats.f_frsize;
        v.available_bytes = static_cast<uint64_t>(stats.f_bavail) * stats.f_frsize;
        v.is_root = mount == "/";
        v.is_removable = starts_with(real, "/dev/") && removable_disk(basename_of(real));
        result.push_back(v);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const pc_volume &a, const pc_volume &b) { return a.is_root > b.is_root; });
    return result;
}

std::string user_name(uint32_t uid) {
    static std::mutex mutex;
    static std::unordered_map<uint32_t, std::string> cache;
    std::lock_guard lock(mutex);
    auto found = cache.find(uid);
    if (found != cache.end()) return found->second;
    passwd entry{}, *result = nullptr;
    char buffer[4096];
    std::string name = ::getpwuid_r(uid, &entry, buffer, sizeof(buffer), &result) == 0 && result ? result->pw_name
                                                                                                 : std::to_string(uid);
    cache[uid] = name;
    return name;
}

namespace {

struct DesktopApp {
    std::string name;
};

bool generic_bin_dir(const std::string &dir) {
    return dir == "/usr/bin" || dir == "/bin" || dir == "/usr/local/bin" || dir == "/usr/sbin" || dir == "/sbin" ||
           dir == "/usr/games" || dir == "/snap/bin" || dir.empty();
}

// Installed applications by the program they run: ~/.local/share/applications, the system
// directories and the Flatpak and Snap exports. Read once; an app installed while Procyon runs is
// grouped by its executable until the next launch.
const std::unordered_map<std::string, DesktopApp> &desktop_apps() {
    static const std::unordered_map<std::string, DesktopApp> apps = [] {
        std::unordered_map<std::string, DesktopApp> map;
        std::vector<std::string> dirs;
        const char *home = std::getenv("HOME");
        const char *data_home = std::getenv("XDG_DATA_HOME");
        if (data_home && *data_home)
            dirs.push_back(std::string(data_home) + "/applications");
        else if (home)
            dirs.push_back(std::string(home) + "/.local/share/applications");
        const char *data_dirs = std::getenv("XDG_DATA_DIRS");
        for (const auto &d : split(data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share", ':'))
            if (!d.empty()) dirs.push_back(d + "/applications");
        dirs.push_back("/var/lib/flatpak/exports/share/applications");
        dirs.push_back("/var/lib/snapd/desktop/applications");
        for (const auto &dir : dirs) {
            for (const auto &file : list_dir(dir)) {
                if (file.size() < 9 || file.compare(file.size() - 8, 8, ".desktop") != 0) continue;
                std::string text;
                if (!read_file(dir + "/" + file, text)) continue;
                std::string name, exec, wm_class;
                bool in_entry = false, hidden = false;
                for (const auto &raw : split(text, '\n')) {
                    const std::string line = trim(raw);
                    if (!line.empty() && line[0] == '[') {
                        in_entry = line == "[Desktop Entry]";
                        continue;
                    }
                    if (!in_entry) continue;
                    if (starts_with(line, "Name=") && name.empty()) name = line.substr(5);
                    if (starts_with(line, "Exec=") && exec.empty()) exec = line.substr(5);
                    if (starts_with(line, "StartupWMClass=")) wm_class = line.substr(15);
                    if (line == "NoDisplay=true" || line == "Hidden=true" || line == "Terminal=true") hidden = true;
                }
                if (hidden || name.empty() || exec.empty()) continue;
                // The program behind "env VAR=x prog", "flatpak run --command=prog id" and "/snap/bin/prog".
                std::string program;
                const auto args = split_whitespace(exec);
                for (size_t i = 0; i < args.size(); ++i) {
                    const std::string base = basename_of(args[i]);
                    if (base == "env" || args[i].find('=') != std::string::npos) {
                        if (starts_with(args[i], "--command=")) program = args[i].substr(10);
                        continue;
                    }
                    if (base == "flatpak") {
                        for (const auto &a : args)
                            if (starts_with(a, "--command=")) program = a.substr(10);
                        break;
                    }
                    program = base;
                    break;
                }
                // Launchers in /usr/bin often point into an install directory (/opt/google/chrome,
                // /usr/lib/firefox): every program in that directory belongs to the app.
                std::string launcher = program.empty() ? "" : args.empty() ? "" : find_program(program);
                char resolved[PATH_MAX];
                if (!launcher.empty() && ::realpath(launcher.c_str(), resolved)) {
                    const std::string dir = std::string(resolved).substr(0, std::string(resolved).find_last_of('/'));
                    if (!generic_bin_dir(dir) && !map.count("dir:" + dir)) map["dir:" + dir] = {name};
                }
                for (const std::string &key : {program, wm_class}) {
                    std::string lower = key;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (!lower.empty() && !map.count(lower)) map[lower] = {name};
                }
            }
        }
        return map;
    }();
    return apps;
}

}  // namespace

AppIdentity app_identity(const RawProcess &process) {
    // Linux has no bundles: an installed application is a .desktop entry, and every process of its
    // executable belongs to it (Firefox's content processes run the firefox binary too). Programs
    // without an entry (shells, daemons, tools) group by name and are not apps.
    const std::string program = basename_of(process.path.empty() ? process.name : process.path);
    std::string lower = program;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    const auto &apps = desktop_apps();
    auto found = apps.find(lower);
    if (found == apps.end() && !process.path.empty()) {
        const std::string dir = process.path.substr(0, process.path.find_last_of('/'));
        if (!generic_bin_dir(dir)) found = apps.find("dir:" + dir);
    }
    if (found == apps.end() && !process.name.empty()) {
        std::string name = process.name;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        found = apps.find(name);
    }
    if (found != apps.end() && !process.path.empty() && process.uid >= 1000) return {process.path, found->second.name};
    return {"exe:" + process.name, process.name};
}

bool is_system_process(const RawProcess &process) {
    // root and the system accounts (below UID_MIN, 1000 on every major distribution), and nobody.
    if (process.uid < 1000 || process.uid == 65534) return true;
    return starts_with(process.path, "/usr/libexec/") || starts_with(process.path, "/usr/lib/systemd/") ||
           starts_with(process.path, "/lib/systemd/");
}

int32_t self_pid() { return ::getpid(); }

bool protected_pid(int32_t pid) { return pid <= 2; }  // the idle task, init and kthreadd

// No privileged helper on Linux yet: the core acts with the rights it has, and system services go
// through systemctl, which asks for authorization through polkit when it needs it.
bool helper_supported() { return false; }

pc_result signal_process(int32_t pid, bool force) { return send_signal(pid, force ? SIGKILL : SIGTERM); }

bool valid_signal(int32_t signal) { return signal > 0 && signal <= SIGRTMAX; }

pc_result send_signal(int32_t pid, int32_t signal) {
    if (!valid_signal(signal)) return PC_ERR_INVALID;
    return ::kill(pid, signal) == 0 ? PC_OK : errno_result();
}

namespace {

// Linux schedules threads: the nice value and affinity of a "process" are per thread, so apply
// them to every thread, like `renice` and `taskset -a` do.
template <typename Apply>
pc_result for_each_thread(int32_t pid, Apply apply) {
    const auto tids = list_dir(proc_path(pid, "task"));
    if (tids.empty()) {
        errno = ESRCH;
        return exists("/proc/" + std::to_string(pid)) ? PC_ERR_PERMISSION : PC_ERR_NOT_FOUND;
    }
    pc_result result = PC_OK;
    bool any = false;
    for (const auto &name : tids) {
        const int tid = std::atoi(name.c_str());
        if (tid <= 0) continue;
        if (apply(tid)) {
            any = true;
        } else if (errno != ESRCH) {  // a thread that exited in between is fine
            result = errno_result();
        }
    }
    return any || result != PC_OK ? result : PC_ERR_NOT_FOUND;
}

}  // namespace

pc_result set_priority(int32_t pid, int32_t nice) {
    if (nice < -20 || nice > 19) return PC_ERR_INVALID;
    return for_each_thread(pid, [&](int tid) {
        errno = 0;
        return ::setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) == 0;
    });
}

pc_result set_affinity(int32_t pid, uint64_t mask) {
    if (mask == 0) return PC_ERR_INVALID;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 64; ++cpu)
        if (mask & (1ull << cpu)) CPU_SET(cpu, &set);
    return for_each_thread(pid, [&](int tid) { return ::sched_setaffinity(tid, sizeof(set), &set) == 0; });
}

namespace {

// Recent CPU use of each thread: the time it ran since the previous details read of it.
struct ThreadSample {
    uint64_t cpu_ns = 0;
    double at = 0;
};
std::mutex thread_mutex;
std::unordered_map<uint64_t, ThreadSample> thread_samples;

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool process_details(int32_t pid, Details &out) {
    out = {};
    out.cwd = read_link(proc_path(pid, "cwd"));
    if (read_cmdline(pid, out.arguments)) out.arguments_known = true;
    std::string environ_text;
    if (read_file(proc_path(pid, "environ"), environ_text)) {
        size_t start = 0;
        while (start < environ_text.size()) {
            size_t end = environ_text.find('\0', start);
            if (end == std::string::npos) end = environ_text.size();
            if (end > start) out.environment.push_back(environ_text.substr(start, end - start));
            start = end + 1;
        }
    } else if (exists("/proc/" + std::to_string(pid))) {
        out.arguments_known = false;  // another user's process: its environment is private
    }

    const auto tids = list_dir(proc_path(pid, "task"));
    const double now = now_seconds();
    std::lock_guard lock(thread_mutex);
    for (const auto &name : tids) {
        const int32_t tid = std::atoi(name.c_str());
        std::string text;
        ProcStat stat;
        if (tid <= 0 || !read_file("/proc/" + std::to_string(pid) + "/task/" + name + "/stat", text) ||
            !parse_stat(text, stat))
            continue;
        ThreadInfo t;
        t.id = static_cast<uint64_t>(tid);
        t.name = stat.comm;
        t.user_time_ns = ticks_to_ns(stat.utime);
        t.system_time_ns = ticks_to_ns(stat.stime);
        t.priority = stat.priority;
        t.state = state_of(stat.state);
        const uint64_t key = identity_key(tid, stat.start_ticks);
        const uint64_t cpu = t.user_time_ns + t.system_time_ns;
        auto previous = thread_samples.find(key);
        if (previous != thread_samples.end() && now > previous->second.at && cpu >= previous->second.cpu_ns)
            t.cpu_percent = static_cast<double>(cpu - previous->second.cpu_ns) / 1e7 / (now - previous->second.at);
        thread_samples[key] = {cpu, now};
        out.threads.push_back(std::move(t));
    }
    if (thread_samples.size() > 20000) thread_samples.clear();
    out.threads_known = !out.threads.empty();
    return out.complete();
}

}  // namespace procyon::platform

#endif  // __linux__
