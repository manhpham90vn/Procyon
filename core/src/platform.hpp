// Internal adapter interface. Each OS implements these in src/platform/<os>.cpp;
// everything above this layer (deltas, views, actions policy) is portable.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "procyon/procyon.h"

namespace procyon::platform {

struct RawProcess {
    int32_t pid = 0;
    int32_t ppid = 0;
    uint32_t uid = 0;
    int64_t start_time = 0;    // unix seconds; with pid forms a stable identity
    bool restricted = false;   // task-level counters unreadable
    uint64_t cpu_time_ns = 0;  // user + system
    int64_t memory_bytes = -1;
    bool has_disk_io = false;
    uint64_t disk_read = 0;  // cumulative bytes
    uint64_t disk_write = 0;
    bool has_net_io = false;
    uint64_t net_rx = 0;  // cumulative bytes over the process's sockets
    uint64_t net_tx = 0;
    int32_t threads = -1;
    int32_t nice = 0;
    int32_t state = PC_STATE_UNKNOWN;
    bool has_gpu = false;
    uint64_t gpu_time_ns = 0;  // cumulative GPU time over the process's GPU clients
    bool has_energy = false;
    uint64_t energy_nj = 0;  // cumulative energy the OS billed to the process, nanojoules
    std::string name;
    std::string path;
};

// Per-core scheduler ticks as the kernel counts them: 32-bit, and they wrap (about once a year per
// counter at 100 Hz). Deltas are taken modulo 2^32, see tick_delta in monitor.hpp.
struct CpuTicks {
    uint32_t user = 0, system = 0, idle = 0, nice = 0;
};

// Parent link of a live process: the cheap part of the process table, without any counters.
struct ProcessParent {
    int32_t pid = 0;
    int32_t ppid = 0;
};

struct Memory {
    uint64_t total = 0, used = 0, app = 0, wired = 0, compressed = 0, cached = 0, free = 0;
    uint64_t swap_total = 0, swap_used = 0;
    int32_t pressure = PC_PRESSURE_UNKNOWN;
};

struct IoCounters {
    uint64_t disk_read = 0, disk_write = 0, net_rx = 0, net_tx = 0;
};

struct AppIdentity {
    std::string id;
    std::string name;
};

struct ThreadInfo {
    uint64_t id = 0;
    std::string name;
    double cpu_percent = -1;
    uint64_t user_time_ns = 0;
    uint64_t system_time_ns = 0;
    int32_t priority = 0;
    int32_t state = PC_STATE_UNKNOWN;
};

// What pc_process_details adds to the snapshot's per-process fields.
struct Details {
    std::string cwd;
    bool arguments_known = false;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    bool threads_known = false;
    std::vector<ThreadInfo> threads;

    // Everything the OS lets this process see was read.
    bool complete() const { return arguments_known && threads_known; }
};

struct OpenFile {
    int32_t pid = 0;
    int32_t fd = -1;
    int32_t kind = PC_FILE_REGULAR;
    std::string path;
};

struct PowerAssertion {
    int32_t pid = 0;
    int32_t on_behalf_of = -1;
    uint32_t kind = 0;
    std::string type;
    std::string reason;
    int64_t created = 0;
};

uint32_t capabilities();
bool system_info(pc_system_info &out);
bool processes(std::vector<RawProcess> &out);
// Every live process's pid and ppid, read fresh. Cheap enough to call before walking a tree.
bool process_parents(std::vector<ProcessParent> &out);
// Fills cpu/memory/disk/thread counters for one process; false when privileges are missing.
bool read_counters(int32_t pid, RawProcess &out);
// -1 when the process doesn't exist, 0 when it exists but its start time can't be read.
int64_t start_time(int32_t pid);
// Per-process network: whether the source works on this machine, and fills
// has_net_io/net_rx/net_tx. Totals may lag by one refresh; never blocks.
bool process_network_available();
void process_network(std::vector<RawProcess> &processes);
bool cpu_ticks(std::vector<CpuTicks> &out);
bool memory(Memory &out);
bool io_counters(IoCounters &out);
void load_average(double out[3]);
std::vector<pc_volume> volumes();

std::string user_name(uint32_t uid);
AppIdentity app_identity(const RawProcess &process);
bool is_system_process(const RawProcess &process);
int32_t self_pid();
// Pids that must never be ended or changed, whatever the user asks: the kernel and init
// (launchd, pid 0 and 1; the idle process and System, pid 0 and 4, on Windows).
bool protected_pid(int32_t pid);
// Whether privileged actions go through procyon-helper (macOS) or the process runs with the
// rights it has (Windows: the UI relaunches itself elevated for full access).
bool helper_supported();

// Sends a termination request (force = false) or kill (force = true).
pc_result signal_process(int32_t pid, bool force);
pc_result send_signal(int32_t pid, int32_t signal);
bool valid_signal(int32_t signal);
pc_result set_priority(int32_t pid, int32_t nice);
pc_result set_affinity(int32_t pid, uint64_t mask);
bool process_details(int32_t pid, Details &out);

// GPUs and per-process GPU time (fills has_gpu/gpu_time_ns).
std::vector<pc_gpu> gpus();
bool process_gpu_available();
void process_gpu(std::vector<RawProcess> &processes);
// Hottest CPU die sensor in Celsius, -1 when unknown.
double cpu_temperature();
// Hottest CPU die and internal SSD (NAND) sensors in Celsius, -1 each when unknown; one sensor read.
void temperatures(double &cpu, double &disk);

// Open files and sockets of `pid` (-1: every process). False when some process couldn't be read
// (another user's, without privileges); what could be read is still returned.
bool open_files(int32_t pid, std::vector<OpenFile> &out);
bool connections(int32_t pid, std::vector<pc_connection> &out);
// Whether a failed descriptor listing (`bytes` <= 0, `error` the errno) means the process has
// descriptors we may not see, which makes a listing incomplete. A process that is gone, a zombie,
// or one with nothing open simply has nothing to list. Exposed for tests.
bool handles_denied(int bytes, int error);

bool battery(pc_battery &out);
std::vector<PowerAssertion> power_assertions();
#if defined(__linux__)
// The parsing half of power_assertions() on Linux: `busctl --json=short call … ListInhibitors`
// output (systemd-logind's inhibitor locks). Spawns nothing (exposed for tests).
std::vector<PowerAssertion> parse_inhibitors(const std::string &json);
// Every string and number in the "data" of a `busctl --json=short` reply, in order (exposed for tests).
std::vector<std::string> busctl_values(const std::string &json);
// One DRM client's fdinfo: its client id and engine time in nanoseconds (the busiest engine kind).
// False when the text isn't a DRM client's (exposed for tests).
bool parse_drm_fdinfo(const std::string &text, uint64_t &client_id, uint64_t &engine_ns);
// One row of /proc/net/{tcp,tcp6,udp,udp6} and the socket inode it belongs to.
struct SocketEntry {
    pc_connection connection{};
    uint64_t inode = 0;
};
std::vector<SocketEntry> parse_proc_net(const std::string &text, int32_t protocol, int32_t family);
// `systemctl show` output: one key/value map per unit (blank-line separated).
std::vector<std::map<std::string, std::string>> parse_systemctl_show(const std::string &text);
// The [Desktop Entry] keys of an XDG autostart file that decide whether and where it runs.
struct DesktopEntry {
    std::string name, exec, type, only_show_in, not_show_in;
    bool hidden = false;
    bool no_display = false;
    bool autostart_enabled = true;  // X-GNOME-Autostart-enabled
    bool session_phase = false;     // X-GNOME-Autostart-Phase: a session component
};
DesktopEntry parse_desktop_entry(const std::string &text);
#endif
#if defined(_WIN32)
// The parsing half of power_assertions() on Windows: `report` is the output of `powercfg /requests`.
// Spawns nothing (exposed for tests).
std::vector<PowerAssertion> parse_power_requests(const std::string &report);
#endif

// Services and startup items (launchd, systemd, SCM).
std::vector<pc_service> services();
std::vector<pc_startup_item> startup_items();
// Open at Login and app background items of `user` (plus machine-wide ones). macOS shares the list
// only with administrators: the helper calls this as root for the app's user.
std::vector<pc_startup_item> managed_startup_items(uint32_t user);

// One job of `launchctl print <domain>`: its pid (0 when not running) and last exit status.
struct LoadedService {
    int32_t pid = 0;
    int32_t last_exit = 0;
};
using LoadedServices = std::unordered_map<std::string, LoadedService>;
// Pure parsers of the launchctl listings, exposed for tests: the "services = { pid status label }"
// block of `launchctl print <domain>`, and `launchctl print-disabled <domain>` as label -> disabled.
LoadedServices parse_launchctl_print(const std::string &text);
std::unordered_map<std::string, bool> parse_launchctl_disabled(const std::string &text);
// The parsing half of managed_startup_items, on `sfltool dumpbtm` output; `user_jobs` and
// `system_jobs` are the loaded services of the user's gui domain and of the system domain, used
// for the running pids. Spawns nothing (exposed for tests).
std::vector<pc_startup_item> parse_managed_startup_items(std::string dump, uint32_t user,
                                                         const LoadedServices &user_jobs,
                                                         const LoadedServices &system_jobs);
// Runs the OS tool for one action in the given domain as the current user. The monitor routes
// system-domain requests through the helper, which calls this as root.
pc_result service_control(int32_t domain, const std::string &label, int32_t action);
bool valid_service_label(const std::string &label);

}  // namespace procyon::platform
