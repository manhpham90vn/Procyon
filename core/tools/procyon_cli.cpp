// procyon-cli: exercises the C ABI without a UI.
//   procyon-cli [samples] [interval_ms] [filter]
#include <spawn.h>
#include <sys/resource.h>
#include <unistd.h>

#include <string>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "procyon/procyon.h"

static double cpu_seconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

int main(int argc, char **argv) {
    const int samples = argc > 1 ? std::atoi(argv[1]) : 3;
    const int interval_ms = argc > 2 ? std::atoi(argv[2]) : 1000;
    const char *filter = argc > 3 ? argv[3] : "";

    pc_system_info info;
    pc_system_info_get(&info);
    std::printf("%s %s (%s) · %s %s · %s · %d cores (%dP+%dE) · %.1f GB\n", info.os_name, info.os_version,
                info.os_build, info.model_name, info.model_id, info.cpu_brand, info.logical_cores,
                info.performance_cores, info.efficiency_cores, info.memory_total / 1073741824.0);

    pc_monitor *monitor = pc_monitor_create();

    // PROCYON_HELPER=/path/to/procyon-helper: spawn it (run the CLI with sudo for real root access).
    if (const char *helper = std::getenv("PROCYON_HELPER")) {
        std::string socket = "/tmp/procyon-cli-" + std::to_string(getpid()) + ".sock";
        std::string parent = std::to_string(getpid()), uid = std::to_string(getuid());
        const char *args[] = {helper,         "--socket", socket.c_str(), "--parent",
                              parent.c_str(), "--uid",    uid.c_str(),    nullptr};
        pid_t child = 0;
        posix_spawn(&child, helper, nullptr, nullptr, const_cast<char *const *>(args), nullptr);
        bool attached = false;
        for (int i = 0; i < 50 && !attached; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            attached = pc_monitor_attach_helper(monitor, socket.c_str());
        }
        std::printf("helper %s\n", attached ? "attached" : "FAILED to attach");
    }
    const pc_volume *volumes = nullptr;
    for (int i = 0, n = pc_monitor_volumes(monitor, &volumes); i < n; ++i)
        std::printf("volume %s at %s: %.1f / %.1f GB free\n", volumes[i].name, volumes[i].mount_point,
                    volumes[i].available_bytes / 1e9, volumes[i].total_bytes / 1e9);

    const double cpu_start = cpu_seconds();
    const auto wall_start = std::chrono::steady_clock::now();
    for (int s = 0; s < samples; ++s) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        const pc_snapshot *snap = pc_monitor_refresh(monitor);
        std::printf(
            "\ncpu %.1f%% (user %.1f%%) · mem %.2f/%.2f GB (pressure %d) · swap %.2f GB · disk r %.0f w %.0f B/s · "
            "net rx %.0f tx %.0f B/s · %d procs %d threads · %d restricted · helper %d\n",
            snap->cpu_usage * 100, snap->cpu_user * 100, snap->memory_used / 1073741824.0,
            snap->memory_total / 1073741824.0, snap->memory_pressure, snap->swap_used / 1073741824.0,
            snap->disk_read_bps, snap->disk_write_bps, snap->net_rx_bps, snap->net_tx_bps, snap->process_count,
            snap->thread_count, snap->restricted_count, snap->helper_state);

        pc_view_query query{PC_VIEW_GROUPED, PC_COLUMN_CPU, true, filter, 8};
        const pc_row *rows = nullptr;
        const int count = pc_monitor_build_view(monitor, &query, &rows);
        for (int r = 0; r < count; ++r) {
            const pc_row &row = rows[r];
            const char *name = row.process_index >= 0 ? snap->processes[row.process_index].name : row.group_name;
            if (row.depth > 1) continue;
            std::printf("%*s%-40s pid %-6d cpu %6.1f%% mem %8.1f MB  (%d procs)\n", row.depth * 2, "", name,
                        row.process_index >= 0 ? snap->processes[row.process_index].pid : row.group_pid,
                        row.cpu_percent, row.memory_bytes / 1048576.0, row.process_count);
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    std::printf("\ncore overhead: %.2f%% of one core\n", (cpu_seconds() - cpu_start) / wall * 100);
    pc_monitor_destroy(monitor);
}
