#pragma once

#include <unordered_map>
#include <vector>

#include "helper_client.hpp"
#include "platform.hpp"
#include "procyon/procyon.h"
#include "view.hpp"

namespace procyon {

// Ticks spent per state between two samples of one core.
struct TickDelta {
    uint64_t user = 0, system = 0, idle = 0, nice = 0;
};

// The kernel's tick counters are 32-bit and wrap: subtract in 32 bits so a wrapped counter still
// yields the small delta it stands for, then widen for the sums.
inline TickDelta tick_delta(const platform::CpuTicks &now, const platform::CpuTicks &before) {
    return {static_cast<uint32_t>(now.user - before.user), static_cast<uint32_t>(now.system - before.system),
            static_cast<uint32_t>(now.idle - before.idle), static_cast<uint32_t>(now.nice - before.nice)};
}

struct ProcessCounters {
    uint64_t cpu_time_ns = 0;
    uint64_t disk_read = 0;
    uint64_t disk_write = 0;
    uint64_t net_rx = 0;
    uint64_t net_tx = 0;
    uint64_t gpu_time_ns = 0;
    uint64_t energy_nj = 0;
    // Which counters were readable: a rate needs the same counter at both ends, or a counter that
    // turns readable (helper connected) would count the process's whole lifetime as one interval.
    bool has_cpu = false;
    bool has_disk_io = false;
    bool has_net_io = false;
    bool has_gpu = false;
    bool has_energy = false;
};

// Storage behind pc_process_details pointers.
struct DetailsStorage {
    pc_process_details details{};
    platform::Details data;
    std::vector<const char *> arguments;
    std::vector<const char *> environment;
    std::vector<pc_thread> threads;
};

}  // namespace procyon

struct pc_monitor {
    uint32_t capabilities = 0;
    double last_refresh = 0;
    bool sample_processes = true;

    pc_snapshot snapshot{};
    std::vector<double> core_usage;
    std::vector<pc_process> processes;
    std::vector<pc_volume> volumes;
    std::vector<pc_gpu> gpus;
    std::vector<pc_service> services;
    std::vector<pc_startup_item> startup_items;
    std::vector<pc_startup_item> managed_startup_items;
    std::vector<pc_power_assertion> power_assertions;
    std::vector<procyon::platform::OpenFile> open_file_data;
    std::vector<pc_open_file> open_files;
    std::vector<pc_connection> connections;
    procyon::DetailsStorage details;
    procyon::View view;

    std::vector<procyon::platform::CpuTicks> previous_ticks;
    procyon::platform::IoCounters previous_io;
    bool has_previous_io = false;
    std::unordered_map<uint64_t, procyon::ProcessCounters> previous_processes;

    procyon::HelperClient helper;
    int32_t helper_state = PC_HELPER_DETACHED;
};
