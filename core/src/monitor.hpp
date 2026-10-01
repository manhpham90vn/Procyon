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
};

}  // namespace procyon

struct pc_monitor {
    uint32_t capabilities = 0;
    double last_refresh = 0;

    pc_snapshot snapshot{};
    std::vector<double> core_usage;
    std::vector<pc_process> processes;
    std::vector<pc_volume> volumes;
    procyon::View view;

    std::vector<procyon::platform::CpuTicks> previous_ticks;
    procyon::platform::IoCounters previous_io;
    bool has_previous_io = false;
    std::unordered_map<uint64_t, procyon::ProcessCounters> previous_processes;

    procyon::HelperClient helper;
    int32_t helper_state = PC_HELPER_DETACHED;
};
