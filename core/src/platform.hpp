// Internal adapter interface. Each OS implements these in src/platform/<os>.cpp;
// everything above this layer (deltas, views, actions policy) is portable.
#pragma once

#include <cstdint>
#include <string>
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

struct CpuTicks {
    uint64_t user = 0, system = 0, idle = 0, nice = 0;
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
// Fills cpu/memory/disk/thread counters for one process; false when privileges are missing.
bool read_counters(int32_t pid, RawProcess &out);
int64_t start_time(int32_t pid);  // -1 when the process doesn't exist
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

bool battery(pc_battery &out);
std::vector<PowerAssertion> power_assertions();

// Services and startup items (launchd, systemd, SCM).
std::vector<pc_service> services();
std::vector<pc_startup_item> startup_items();
// Open at Login and app background items of `user` (plus machine-wide ones). macOS shares the list
// only with administrators: the helper calls this as root for the app's user.
std::vector<pc_startup_item> managed_startup_items(uint32_t user);
// The parsing half, on `sfltool dumpbtm` output (exposed for tests).
std::vector<pc_startup_item> parse_managed_startup_items(std::string dump, uint32_t user);
// Runs the OS tool for one action in the given domain as the current user. The monitor routes
// system-domain requests through the helper, which calls this as root.
pc_result service_control(int32_t domain, const std::string &label, int32_t action);
bool valid_service_label(const std::string &label);

}  // namespace procyon::platform
