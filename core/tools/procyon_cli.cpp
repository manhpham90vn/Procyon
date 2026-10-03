// procyon-cli: exercises the C ABI without a UI.
//   procyon-cli [samples] [interval_ms] [filter]
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <string>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "procyon/procyon.h"

#if defined(_WIN32)
static double cpu_seconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    const auto value = [](const FILETIME &ft) {
        return ((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 1e7;
    };
    return value(kernel) + value(user);
}

static int own_pid() { return static_cast<int>(GetCurrentProcessId()); }
#else
static double cpu_seconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

static int own_pid() { return getpid(); }
#endif

// Unknown values are -1 in the ABI: print them as "-", never as a number.
static const char *rate_text(double value, const char *format, char *buffer, size_t capacity) {
    if (value < 0) return "-";
    (void)std::snprintf(buffer, capacity, format, value);
    return buffer;
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

    // PROCYON_HELPER=/path/to/procyon-helper: spawn it. Run the CLI with sudo for real root access:
    // the helper then starts as root, and the CLI itself goes back to being the invoking user
    // (SUDO_UID), the user the helper serves. The helper refuses to serve root.
    std::string socket_directory, socket_path;
#if !defined(_WIN32)
    if (const char *helper = std::getenv("PROCYON_HELPER")) {
        uid_t user = getuid();
        gid_t group = getgid();
        if (user == 0) {
            const char *sudo_uid = std::getenv("SUDO_UID");
            const char *sudo_gid = std::getenv("SUDO_GID");
            if (sudo_uid && sudo_gid) {
                user = static_cast<uid_t>(std::atoi(sudo_uid));
                group = static_cast<gid_t>(std::atoi(sudo_gid));
            }
        }
        // The helper insists on a socket directory owned by the user and writable by nobody else.
        const char *tmp = std::getenv("TMPDIR");
        socket_directory = tmp && *tmp ? tmp : "/tmp";
        if (socket_directory.back() != '/') socket_directory += '/';
        socket_directory += "procyon-cli-" + std::to_string(getpid());
        bool ready = mkdir(socket_directory.c_str(), 0700) == 0;
        if (ready && getuid() == 0 && user != 0) {
            const int dir = open(socket_directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            ready = dir >= 0 && fchown(dir, user, group) == 0;
            if (dir >= 0) close(dir);
        }
        socket_path = socket_directory + "/helper.sock";
        std::string parent = std::to_string(getpid()), uid = std::to_string(user);
        const char *args[] = {helper,         "--socket", socket_path.c_str(), "--parent",
                              parent.c_str(), "--uid",    uid.c_str(),         nullptr};
        pid_t child = 0;
        if (ready) posix_spawn(&child, helper, nullptr, nullptr, const_cast<char *const *>(args), nullptr);
        if (getuid() == 0 && user != 0 && (setgid(group) != 0 || setuid(user) != 0)) ready = false;
        bool attached = false;
        for (int i = 0; i < 50 && ready && !attached; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            attached = pc_monitor_attach_helper(monitor, socket_path.c_str());
        }
        std::printf("helper %s\n", attached ? "attached" : "FAILED to attach");
    }
#endif
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
    if (pc_process_details_get(monitor, own_pid(), &details) == PC_OK)
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
        if (snap->cpu_temperature >= 0 || snap->disk_temperature >= 0) {
            char cpu_text[32], disk_text[32];
            std::printf("cpu temperature %s °C · ssd %s °C\n",
                        rate_text(snap->cpu_temperature, "%.1f", cpu_text, sizeof(cpu_text)),
                        rate_text(snap->disk_temperature, "%.1f", disk_text, sizeof(disk_text)));
        }
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
            char cpu[32], gpu[32], mem[32], rx[32], tx[32];
            std::printf("%*s%-40s pid %-6d cpu %6s%% gpu %5s%% mem %8s MB  net rx %9s tx %9s B/s  (%d procs)\n",
                        row.depth * 2, "", name,
                        row.process_index >= 0 ? snap->processes[row.process_index].pid : row.group_pid,
                        rate_text(row.cpu_percent, "%.1f", cpu, sizeof(cpu)),
                        rate_text(row.gpu_percent, "%.1f", gpu, sizeof(gpu)),
                        rate_text(row.memory_bytes / 1048576.0, "%.1f", mem, sizeof(mem)),
                        rate_text(row.net_rx_bps, "%.0f", rx, sizeof(rx)),
                        rate_text(row.net_tx_bps, "%.0f", tx, sizeof(tx)), row.process_count);
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    std::printf("\ncore overhead: %.2f%% of one core\n", (cpu_seconds() - cpu_start) / wall * 100);
    pc_monitor_destroy(monitor);
#if !defined(_WIN32)
    if (!socket_directory.empty()) {  // the helper exits when its client goes away
        unlink(socket_path.c_str());
        rmdir(socket_directory.c_str());
    }
#endif
}
