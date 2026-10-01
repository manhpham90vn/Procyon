// Portable monitor: turns raw adapter counters into rates and implements the C ABI.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "monitor.hpp"
#include "platform.hpp"
#include "view.hpp"

using namespace procyon;

namespace {

double steady_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

double wall_seconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

double rate(uint64_t now, uint64_t before, double seconds) {
    if (seconds <= 0 || now < before) return 0;
    return static_cast<double>(now - before) / seconds;
}

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

// Asks the privileged helper for the counters this process couldn't read itself.
void fill_from_helper(pc_monitor &monitor, std::vector<platform::RawProcess> &raw) {
    if (!monitor.helper.connected()) return;
    std::vector<int32_t> pids;
    std::unordered_map<int32_t, size_t> index;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (!raw[i].restricted) continue;
        pids.push_back(raw[i].pid);
        index[raw[i].pid] = i;
    }
    if (pids.empty()) return;
    std::vector<helper::Counters> counters;
    if (!monitor.helper.sample(pids, counters)) {
        monitor.helper_state = PC_HELPER_LOST;
        return;
    }
    for (const auto &c : counters) {
        auto it = index.find(c.pid);
        if (it == index.end()) continue;
        auto &r = raw[it->second];
        if (c.start_time != r.start_time) continue;  // pid was reused in between
        r.restricted = false;
        r.cpu_time_ns = c.cpu_time_ns;
        r.memory_bytes = c.memory_bytes;
        r.threads = c.threads;
        r.has_disk_io = c.has_disk_io != 0;
        r.disk_read = c.disk_read;
        r.disk_write = c.disk_write;
    }
}

// Signals directly, falling back to the helper when the process belongs to someone else.
pc_result signal_with_fallback(pc_monitor *monitor, int32_t pid, bool force) {
    pc_result result = platform::signal_process(pid, force);
    if (result == PC_ERR_PERMISSION && monitor && monitor->helper.connected()) {
        result = monitor->helper.end(pid, force);
        if (!monitor->helper.connected()) monitor->helper_state = PC_HELPER_LOST;
    }
    return result;
}

}  // namespace

bool pc_monitor_attach_helper(pc_monitor *monitor, const char *socket_path) {
    if (!monitor || !socket_path) return false;
    const bool ok = monitor->helper.connect(socket_path);
    monitor->helper_state = ok ? PC_HELPER_CONNECTED : PC_HELPER_DETACHED;
    return ok;
}

void pc_monitor_detach_helper(pc_monitor *monitor) {
    if (!monitor) return;
    monitor->helper.disconnect();  // the helper exits when its client goes away
    monitor->helper_state = PC_HELPER_DETACHED;
}

int32_t pc_monitor_helper_state(const pc_monitor *monitor) {
    return monitor ? monitor->helper_state : PC_HELPER_DETACHED;
}

uint32_t pc_capabilities(void) { return platform::capabilities(); }

bool pc_system_info_get(pc_system_info *out) { return out && platform::system_info(*out); }

pc_monitor *pc_monitor_create(void) {
    auto *monitor = new pc_monitor();
    monitor->capabilities = platform::capabilities();
    pc_monitor_refresh(monitor);  // prime counters so the first real refresh has rates
    return monitor;
}

void pc_monitor_destroy(pc_monitor *monitor) { delete monitor; }

const pc_snapshot *pc_monitor_snapshot(const pc_monitor *monitor) { return monitor ? &monitor->snapshot : nullptr; }

const pc_snapshot *pc_monitor_refresh(pc_monitor *monitor) {
    if (!monitor) return nullptr;
    const double now = steady_seconds();
    const double elapsed = monitor->last_refresh > 0 ? now - monitor->last_refresh : 0;
    monitor->last_refresh = now;

    pc_snapshot &snap = monitor->snapshot;
    snap = {};
    snap.timestamp = wall_seconds();
    snap.interval = elapsed;

    // CPU: per-core tick deltas.
    std::vector<platform::CpuTicks> ticks;
    if (platform::cpu_ticks(ticks)) {
        monitor->core_usage.assign(ticks.size(), 0);
        uint64_t busy_total = 0, user_total = 0, system_total = 0, all_total = 0;
        for (size_t i = 0; i < ticks.size(); ++i) {
            platform::CpuTicks d = ticks[i];
            if (i < monitor->previous_ticks.size()) {
                const auto &p = monitor->previous_ticks[i];
                d = {ticks[i].user - p.user, ticks[i].system - p.system, ticks[i].idle - p.idle,
                     ticks[i].nice - p.nice};
            }
            const uint64_t busy = d.user + d.system + d.nice;
            const uint64_t all = busy + d.idle;
            monitor->core_usage[i] = all ? static_cast<double>(busy) / all : 0;
            busy_total += busy;
            user_total += d.user + d.nice;
            system_total += d.system;
            all_total += all;
        }
        if (all_total) {
            snap.cpu_usage = static_cast<double>(busy_total) / all_total;
            snap.cpu_user = static_cast<double>(user_total) / all_total;
            snap.cpu_system = static_cast<double>(system_total) / all_total;
        }
        monitor->previous_ticks = std::move(ticks);
    }
    snap.core_count = static_cast<int32_t>(monitor->core_usage.size());
    snap.core_usage = monitor->core_usage.data();
    platform::load_average(snap.load_average);

    platform::Memory mem;
    if (platform::memory(mem)) {
        snap.memory_total = mem.total;
        snap.memory_used = mem.used;
        snap.memory_app = mem.app;
        snap.memory_wired = mem.wired;
        snap.memory_compressed = mem.compressed;
        snap.memory_cached = mem.cached;
        snap.memory_free = mem.free;
        snap.swap_total = mem.swap_total;
        snap.swap_used = mem.swap_used;
        snap.memory_pressure = mem.pressure;
    } else {
        snap.memory_pressure = PC_PRESSURE_UNKNOWN;
    }

    platform::IoCounters io;
    if (platform::io_counters(io)) {
        if (monitor->has_previous_io) {
            snap.disk_read_bps = rate(io.disk_read, monitor->previous_io.disk_read, elapsed);
            snap.disk_write_bps = rate(io.disk_write, monitor->previous_io.disk_write, elapsed);
            snap.net_rx_bps = rate(io.net_rx, monitor->previous_io.net_rx, elapsed);
            snap.net_tx_bps = rate(io.net_tx, monitor->previous_io.net_tx, elapsed);
        }
        snap.disk_read_total = io.disk_read;
        snap.disk_write_total = io.disk_write;
        snap.net_rx_total = io.net_rx;
        snap.net_tx_total = io.net_tx;
        monitor->previous_io = io;
        monitor->has_previous_io = true;
    }

    // Processes: identity is (pid, start_time) so a reused pid never inherits old counters.
    std::vector<platform::RawProcess> raw;
    platform::processes(raw);
    fill_from_helper(*monitor, raw);
    platform::process_network(raw);
    const int32_t self = platform::self_pid();
    std::unordered_map<uint64_t, ProcessCounters> current;
    current.reserve(raw.size());
    monitor->processes.resize(raw.size());
    int32_t threads = 0;
    int32_t restricted = 0;

    for (size_t i = 0; i < raw.size(); ++i) {
        const auto &r = raw[i];
        pc_process &p = monitor->processes[i];
        std::memset(&p, 0, sizeof(p));
        p.pid = r.pid;
        p.ppid = r.ppid == r.pid ? -1 : r.ppid;
        p.uid = r.uid;
        p.start_time = r.start_time;
        p.threads = r.threads;
        p.memory_bytes = r.memory_bytes;
        p.cpu_percent = -1;
        p.disk_read_bps = p.disk_write_bps = -1;
        p.net_rx_bps = p.net_tx_bps = -1;
        if (r.threads > 0) threads += r.threads;

        const uint64_t key =
            (static_cast<uint64_t>(static_cast<uint32_t>(r.pid)) << 32) ^ static_cast<uint64_t>(r.start_time);
        ProcessCounters counters{r.cpu_time_ns, r.disk_read, r.disk_write, r.net_rx, r.net_tx};
        auto previous = monitor->previous_processes.find(key);
        const bool has_previous = previous != monitor->previous_processes.end() && elapsed > 0;

        if (!r.restricted) {
            p.cpu_percent = has_previous ? rate(r.cpu_time_ns, previous->second.cpu_time_ns, elapsed) / 1e7 : 0;
        }
        if (r.has_disk_io) {
            p.disk_read_bps = has_previous ? rate(r.disk_read, previous->second.disk_read, elapsed) : 0;
            p.disk_write_bps = has_previous ? rate(r.disk_write, previous->second.disk_write, elapsed) : 0;
        }
        if (r.has_net_io) {
            p.net_rx_bps = has_previous ? rate(r.net_rx, previous->second.net_rx, elapsed) : 0;
            p.net_tx_bps = has_previous ? rate(r.net_tx, previous->second.net_tx, elapsed) : 0;
        }
        current.emplace(key, counters);

        if (r.restricted) {
            p.flags |= PC_PROC_RESTRICTED;
            ++restricted;
        }
        if (platform::is_system_process(r)) p.flags |= PC_PROC_SYSTEM;
        if (r.pid <= 1 || r.pid == self) p.flags |= PC_PROC_PROTECTED;

        const auto app = platform::app_identity(r);
        if (app.id.rfind("exe:", 0) != 0) p.flags |= PC_PROC_APP_BUNDLE;
        copy_string(p.name, sizeof(p.name), r.name);
        copy_string(p.path, sizeof(p.path), r.path);
        copy_string(p.user, sizeof(p.user), platform::user_name(r.uid));
        copy_string(p.app_id, sizeof(p.app_id), app.id);
        copy_string(p.app_name, sizeof(p.app_name), app.name);
    }
    monitor->previous_processes = std::move(current);

    snap.process_count = static_cast<int32_t>(monitor->processes.size());
    snap.thread_count = threads;
    snap.restricted_count = restricted;
    snap.helper_state = monitor->helper_state;
    snap.processes = monitor->processes.data();
    return &snap;
}

int32_t pc_monitor_volumes(pc_monitor *monitor, const pc_volume **out_volumes) {
    if (!monitor) return 0;
    monitor->volumes = platform::volumes();
    if (out_volumes) *out_volumes = monitor->volumes.data();
    return static_cast<int32_t>(monitor->volumes.size());
}

int32_t pc_monitor_build_view(pc_monitor *monitor, const pc_view_query *query, const pc_row **out_rows) {
    if (!monitor || !query) return 0;
    build_view(monitor->processes, *query, monitor->view);
    if (out_rows) *out_rows = monitor->view.rows.data();
    return static_cast<int32_t>(monitor->view.rows.size());
}

pc_result pc_process_end(pc_monitor *monitor, int32_t pid, bool force) {
    if (monitor) {
        for (const auto &p : monitor->processes)
            if (p.pid == pid && (p.flags & PC_PROC_PROTECTED)) return PC_ERR_PROTECTED;
    }
    if (pid <= 1 || pid == platform::self_pid()) return PC_ERR_PROTECTED;
    return signal_with_fallback(monitor, pid, force);
}

pc_result pc_process_end_tree(pc_monitor *monitor, int32_t pid) {
    if (!monitor) return PC_ERR_FAILED;
    if (pid <= 1 || pid == platform::self_pid()) return PC_ERR_PROTECTED;

    std::unordered_map<int32_t, std::vector<int32_t>> children;
    std::unordered_set<int32_t> protected_pids;
    for (const auto &p : monitor->processes) {
        children[p.ppid].push_back(p.pid);
        if (p.flags & PC_PROC_PROTECTED) protected_pids.insert(p.pid);
    }
    // Post-order: kill leaves first so parents cannot respawn them.
    std::vector<int32_t> order;
    std::vector<std::pair<int32_t, bool>> stack{{pid, false}};
    std::unordered_set<int32_t> seen;
    while (!stack.empty()) {
        auto [current, expanded] = stack.back();
        stack.pop_back();
        if (expanded) {
            order.push_back(current);
            continue;
        }
        if (!seen.insert(current).second) continue;
        stack.push_back({current, true});
        for (int32_t child : children[current]) stack.push_back({child, false});
    }

    pc_result result = PC_OK;
    for (int32_t target : order) {
        if (protected_pids.count(target)) continue;
        pc_result r = signal_with_fallback(monitor, target, true);
        if (target == pid)
            result = r;
        else if (r == PC_ERR_PERMISSION && result == PC_OK)
            result = PC_ERR_PERMISSION;
    }
    return result;
}

const char *pc_result_message(pc_result result) {
    switch (result) {
        case PC_OK: return "Done";
        case PC_ERR_NOT_FOUND: return "The process no longer exists.";
        case PC_ERR_PERMISSION: return "You don't have permission to end this process.";
        case PC_ERR_PROTECTED: return "This process is critical to the system and can't be ended.";
        case PC_ERR_FAILED: return "The process could not be ended.";
    }
    return "Unknown error";
}
