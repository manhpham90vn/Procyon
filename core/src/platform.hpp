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
    int32_t threads = -1;
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

uint32_t capabilities();
bool system_info(pc_system_info &out);
bool processes(std::vector<RawProcess> &out);
// Fills cpu/memory/disk/thread counters for one process; false when privileges are missing.
bool read_counters(int32_t pid, RawProcess &out);
int64_t start_time(int32_t pid);  // -1 when the process doesn't exist
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

}  // namespace procyon::platform
