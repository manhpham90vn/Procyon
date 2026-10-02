#pragma once

#include <unordered_map>
#include <vector>

#include "helper_client.hpp"
#include "platform.hpp"
#include "procyon/procyon.h"
#include "view.hpp"

namespace procyon {

struct ProcessCounters {
    uint64_t cpu_time_ns = 0;
    uint64_t disk_read = 0;
    uint64_t disk_write = 0;
    uint64_t net_rx = 0;
    uint64_t net_tx = 0;
    uint64_t gpu_time_ns = 0;
    uint64_t energy_nj = 0;
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
