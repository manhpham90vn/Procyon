// procyon-cli: exercises the C ABI without a UI.
//   procyon-cli [samples] [interval_ms] [filter]
#include <spawn.h>
#include <sys/resource.h>
#include <unistd.h>

#include <string>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    pc_battery battery;
    if (pc_battery_get(&battery) && battery.present)
        std::printf("battery %.0f%% %s · health %.0f%% · %d cycles · %.1f °C · %.1f W · %s\n", battery.level * 100,
                    battery.charging      ? "charging"
                    : battery.on_ac_power ? "on AC"
                                          : "on battery",
                    battery.health * 100, battery.cycle_count, battery.temperature, battery.power_watts,
                    battery.condition);
    const pc_power_assertion *assertions = nullptr;
    for (int i = 0, n = pc_monitor_power_assertions(monitor, &assertions); i < n; ++i)
        std::printf(
            "keeps awake: %s (pid %d%s) %s — %s\n", assertions[i].process_name, assertions[i].pid,
            assertions[i].on_behalf_of > 0 ? (", for " + std::to_string(assertions[i].on_behalf_of)).c_str() : "",
            assertions[i].type, assertions[i].reason);
    const pc_service *services = nullptr;
    const int service_count = pc_monitor_services(monitor, &services);
    int running = 0, third_party = 0;
    for (int i = 0; i < service_count; ++i) {
        running += services[i].pid > 0;
        third_party += !services[i].apple;
    }
    std::printf("services: %d (%d running, %d third-party)\n", service_count, running, third_party);
    const pc_startup_item *items = nullptr;
    for (int i = 0, n = pc_monitor_startup_items(monitor, &items); i < n; ++i)
        std::printf("startup: %-24s %-44s scope %d %s pid %d\n", items[i].name, items[i].label, items[i].scope,
                    items[i].enabled ? "enabled " : "disabled", items[i].pid);
    const pc_startup_item *managed = nullptr;
    bool managed_ready = false;
    for (int i = 0, n = pc_monitor_startup_managed_items(monitor, &managed, &managed_ready); i < n; ++i)
        std::printf("managed: %-34s scope %d %s pid %-6d app %s (%s)\n", managed[i].name, managed[i].scope,
                    managed[i].enabled ? "enabled " : "disabled", managed[i].pid, managed[i].app_path,
                    managed[i].parent_name);
    const pc_process_details *details = nullptr;
    if (pc_process_details_get(monitor, getpid(), &details) == PC_OK)
        std::printf("self: %d args, %d env, %d threads, cwd %s, nice %d\n", details->argument_count,
                    details->environment_count, details->thread_count, details->cwd, details->nice);

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
        if (snap->cpu_temperature >= 0)
            std::printf("cpu temperature %.1f °C · ssd %.1f °C\n", snap->cpu_temperature, snap->disk_temperature);
        for (int g = 0; g < snap->gpu_count; ++g) {
            const pc_gpu &gpu = snap->gpus[g];
            double per_process = 0;
            for (int p = 0; p < snap->process_count; ++p)
                if (snap->processes[p].gpu_percent > 0) per_process += snap->processes[p].gpu_percent;
            std::printf("gpu %s (%s, %d cores): %.0f%% · memory %.2f GB · processes sum %.1f%%\n", gpu.name, gpu.vendor,
                        gpu.cores, gpu.utilization * 100, gpu.memory_used / 1073741824.0, per_process);
        }

        const char *sort = std::getenv("PROCYON_SORT");
        pc_view_query query{PC_VIEW_GROUPED, sort && !std::strcmp(sort, "gpu") ? PC_COLUMN_GPU : PC_COLUMN_CPU, true,
                            filter, 8};
        const pc_row *rows = nullptr;
        const int count = pc_monitor_build_view(monitor, &query, &rows);
        for (int r = 0; r < count; ++r) {
            const pc_row &row = rows[r];
            const char *name = row.process_index >= 0 ? snap->processes[row.process_index].name : row.group_name;
            if (row.depth > 1) continue;
            std::printf(
                "%*s%-40s pid %-6d cpu %6.1f%% gpu %5.1f%% mem %8.1f MB  net rx %9.0f tx %9.0f B/s  (%d procs)\n",
                row.depth * 2, "", name,
                row.process_index >= 0 ? snap->processes[row.process_index].pid : row.group_pid, row.cpu_percent,
                row.gpu_percent, row.memory_bytes / 1048576.0, row.net_rx_bps, row.net_tx_bps, row.process_count);
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    std::printf("\ncore overhead: %.2f%% of one core\n", (cpu_seconds() - cpu_start) / wall * 100);
    pc_monitor_destroy(monitor);
}
