// Portable monitor: turns raw adapter counters into rates and implements the C ABI.
#include <algorithm>
#include <chrono>
#include <csignal>
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
        r.has_energy = c.has_energy != 0;
        r.energy_nj = c.energy_nj;
    }
}

// Runs `local` and, when the OS refuses for lack of privileges, `elevated` through the helper.
template <typename Local, typename Elevated>
pc_result with_fallback(pc_monitor *monitor, Local local, Elevated elevated) {
    pc_result result = local();
    if (result == PC_ERR_PERMISSION && monitor && monitor->helper.connected()) {
        result = elevated(monitor->helper);
        if (!monitor->helper.connected()) monitor->helper_state = PC_HELPER_LOST;
    }
    return result;
}

// Signals directly, falling back to the helper when the process belongs to someone else.
pc_result signal_with_fallback(pc_monitor *monitor, int32_t pid, int32_t signal) {
    return with_fallback(
        monitor, [&] { return platform::send_signal(pid, signal); },
        [&](HelperClient &helper) { return helper.signal(pid, signal); });
}

pc_result signal_with_fallback(pc_monitor *monitor, int32_t pid, bool force) {
    return signal_with_fallback(monitor, pid, force ? SIGKILL : SIGTERM);
}

// Kernel, init/launchd and Procyon itself never take signals or priority changes.
bool is_protected(const pc_monitor *monitor, int32_t pid) {
    if (pid <= 1 || pid == platform::self_pid()) return true;
    if (monitor) {
        for (const auto &p : monitor->processes)
            if (p.pid == pid) return (p.flags & PC_PROC_PROTECTED) != 0;
    }
    return false;
}

const pc_process *find_process(const pc_monitor *monitor, int32_t pid) {
    for (const auto &p : monitor->processes)
        if (p.pid == pid) return &p;
    return nullptr;
}

// Probing the capabilities walks IOKit, HID sensors and the battery: once per process is enough,
// they do not change while it runs.
uint32_t cached_capabilities() {
    static const uint32_t caps = platform::capabilities();
    return caps;
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

uint32_t pc_capabilities(void) { return cached_capabilities(); }

bool pc_system_info_get(pc_system_info *out) { return out && platform::system_info(*out); }

pc_monitor *pc_monitor_create(void) {
    auto *monitor = new pc_monitor();
    monitor->capabilities = cached_capabilities();
    pc_monitor_refresh(monitor);  // prime counters so the first real refresh has rates
    return monitor;
}

void pc_monitor_destroy(pc_monitor *monitor) { delete monitor; }

void pc_monitor_set_process_sampling(pc_monitor *monitor, bool enabled) {
    if (monitor) monitor->sample_processes = enabled;
}

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
            const TickDelta d = i < monitor->previous_ticks.size() ? tick_delta(ticks[i], monitor->previous_ticks[i])
                                                                   : tick_delta(ticks[i], platform::CpuTicks{});
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
    snap.cpu_temperature = snap.disk_temperature = -1;
    if (monitor->capabilities & PC_CAP_TEMPERATURE) platform::temperatures(snap.cpu_temperature, snap.disk_temperature);
    if (monitor->capabilities & PC_CAP_GPU) monitor->gpus = platform::gpus();
    snap.gpu_count = static_cast<int32_t>(monitor->gpus.size());
    snap.gpus = monitor->gpus.data();

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
    if (monitor->sample_processes) platform::processes(raw);
    fill_from_helper(*monitor, raw);
    if (!raw.empty()) platform::process_network(raw);
    if (!raw.empty() && (monitor->capabilities & PC_CAP_PROCESS_GPU)) platform::process_gpu(raw);
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
        p.nice = r.nice;
        p.state = r.state;
        p.cpu_percent = -1;
        p.gpu_percent = -1;
        p.power_watts = -1;
        p.disk_read_bps = p.disk_write_bps = -1;
        p.net_rx_bps = p.net_tx_bps = -1;
        if (r.threads > 0) threads += r.threads;

        const uint64_t key =
            (static_cast<uint64_t>(static_cast<uint32_t>(r.pid)) << 32) ^ static_cast<uint64_t>(r.start_time);
        ProcessCounters counters{r.cpu_time_ns, r.disk_read,   r.disk_write,  r.net_rx,     r.net_tx,  r.gpu_time_ns,
                                 r.energy_nj,   !r.restricted, r.has_disk_io, r.has_net_io, r.has_gpu, r.has_energy};
        auto found = monitor->previous_processes.find(key);
        const ProcessCounters *previous =
            found != monitor->previous_processes.end() && elapsed > 0 ? &found->second : nullptr;

        // A rate needs the same counter at both ends. Until a process has been seen twice (or the
        // helper has read it twice) its rates are unknown, -1, never a made-up zero.
        if (previous && previous->has_cpu && counters.has_cpu)
            p.cpu_percent = rate(r.cpu_time_ns, previous->cpu_time_ns, elapsed) / 1e7;
        if (previous && previous->has_disk_io && counters.has_disk_io) {
            p.disk_read_bps = rate(r.disk_read, previous->disk_read, elapsed);
            p.disk_write_bps = rate(r.disk_write, previous->disk_write, elapsed);
        }
        if (previous && previous->has_net_io && counters.has_net_io) {
            p.net_rx_bps = rate(r.net_rx, previous->net_rx, elapsed);
            p.net_tx_bps = rate(r.net_tx, previous->net_tx, elapsed);
        }
        // Nanoseconds of GPU time per second of wall time, as a percentage of one GPU.
        if (previous && previous->has_gpu && counters.has_gpu)
            p.gpu_percent = rate(r.gpu_time_ns, previous->gpu_time_ns, elapsed) / 1e7;
        // Nanojoules per second = nanowatts.
        if (previous && previous->has_energy && counters.has_energy && (monitor->capabilities & PC_CAP_PROCESS_ENERGY))
            p.power_watts = rate(r.energy_nj, previous->energy_nj, elapsed) / 1e9;
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
    if (is_protected(monitor, pid)) return PC_ERR_PROTECTED;
    return signal_with_fallback(monitor, pid, force);
}

pc_result pc_process_end_tree(pc_monitor *monitor, int32_t pid) {
    if (!monitor) return PC_ERR_FAILED;
    if (pid <= 1 || pid == platform::self_pid()) return PC_ERR_PROTECTED;

    // The snapshot's process list is stale by up to a refresh and empty when process sampling is
    // off (menu bar only): read the parent links fresh, so a child spawned since is ended too.
    std::vector<platform::ProcessParent> links;
    if (!platform::process_parents(links)) return PC_ERR_FAILED;
    std::unordered_map<int32_t, std::vector<int32_t>> children;
    for (const auto &link : links)
        if (link.ppid != link.pid) children[link.ppid].push_back(link.pid);
    const int32_t self = platform::self_pid();
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
        // Kernel, launchd and Procyon itself stay, whatever the tree says.
        if (target <= 1 || target == self) continue;
        pc_result r = signal_with_fallback(monitor, target, true);
        if (target == pid)
            result = r;
        else if (r == PC_ERR_PERMISSION && result == PC_OK)
            result = PC_ERR_PERMISSION;
    }
    return result;
}

pc_result pc_process_signal(pc_monitor *monitor, int32_t pid, int32_t signal) {
    if (!monitor || !(monitor->capabilities & PC_CAP_SIGNALS)) return PC_ERR_UNSUPPORTED;
    if (!platform::valid_signal(signal)) return PC_ERR_INVALID;
    if (is_protected(monitor, pid)) return PC_ERR_PROTECTED;
    return signal_with_fallback(monitor, pid, signal);
}

pc_result pc_process_suspend(pc_monitor *monitor, int32_t pid) {
    if (is_protected(monitor, pid)) return PC_ERR_PROTECTED;
    return signal_with_fallback(monitor, pid, SIGSTOP);
}

pc_result pc_process_resume(pc_monitor *monitor, int32_t pid) {
    if (is_protected(monitor, pid)) return PC_ERR_PROTECTED;
    return signal_with_fallback(monitor, pid, SIGCONT);
}

pc_result pc_process_set_priority(pc_monitor *monitor, int32_t pid, int32_t nice) {
    if (is_protected(monitor, pid)) return PC_ERR_PROTECTED;
    // Raising priority (lowering nice) is reserved to root on Unix, even for your own processes.
    return with_fallback(
        monitor, [&] { return platform::set_priority(pid, nice); },
        [&](HelperClient &helper) { return helper.set_priority(pid, nice); });
}

pc_result pc_process_set_affinity(pc_monitor *monitor, int32_t pid, uint64_t mask) {
    if (is_protected(monitor, pid)) return PC_ERR_PROTECTED;
    if (mask == 0) return PC_ERR_INVALID;
    return platform::set_affinity(pid, mask);
}

const char *pc_result_message(pc_result result) {
    switch (result) {
        case PC_OK: return "Done";
        case PC_ERR_NOT_FOUND: return "The process no longer exists.";
        case PC_ERR_PERMISSION: return "You don't have permission to do this.";
        case PC_ERR_PROTECTED: return "This process is critical to the system and can't be changed.";
        case PC_ERR_FAILED: return "The operation failed.";
        case PC_ERR_UNSUPPORTED: return "Not supported on this system.";
        case PC_ERR_INVALID: return "Invalid argument.";
    }
    return "Unknown error";
}

pc_result pc_process_details_get(pc_monitor *monitor, int32_t pid, const pc_process_details **out) {
    if (!monitor || !out) return PC_ERR_INVALID;
    *out = nullptr;
    const pc_process *process = find_process(monitor, pid);
    if (!process) return PC_ERR_NOT_FOUND;

    DetailsStorage &storage = monitor->details;
    platform::process_details(pid, storage.data);
    if (!storage.data.complete() && monitor->helper.connected()) {
        platform::Details elevated;
        if (monitor->helper.details(pid, elevated) && elevated.arguments_known + elevated.threads_known >
                                                          storage.data.arguments_known + storage.data.threads_known)
            storage.data = std::move(elevated);
        if (!monitor->helper.connected()) monitor->helper_state = PC_HELPER_LOST;
    }
    const platform::Details &data = storage.data;

    pc_process_details &d = storage.details;
    d = {};
    d.pid = process->pid;
    d.ppid = process->ppid;
    d.uid = process->uid;
    d.nice = process->nice;
    d.state = process->state;
    d.start_time = process->start_time;
    std::memcpy(d.name, process->name, sizeof(d.name));
    std::memcpy(d.user, process->user, sizeof(d.user));
    std::memcpy(d.path, process->path, sizeof(d.path));
    copy_string(d.cwd, sizeof(d.cwd), data.cwd);

    storage.arguments.clear();
    storage.environment.clear();
    for (const auto &a : data.arguments) storage.arguments.push_back(a.c_str());
    for (const auto &e : data.environment) storage.environment.push_back(e.c_str());
    d.arguments_known = data.arguments_known;
    d.argument_count = static_cast<int32_t>(storage.arguments.size());
    d.arguments = storage.arguments.data();
    d.environment_count = static_cast<int32_t>(storage.environment.size());
    d.environment = storage.environment.data();

    storage.threads.clear();
    for (const auto &t : data.threads) {
        pc_thread thread{};
        thread.id = t.id;
        copy_string(thread.name, sizeof(thread.name), t.name);
        thread.cpu_percent = t.cpu_percent;
        thread.user_time_ns = t.user_time_ns;
        thread.system_time_ns = t.system_time_ns;
        thread.priority = t.priority;
        thread.state = t.state;
        storage.threads.push_back(thread);
    }
    d.threads_known = data.threads_known;
    d.thread_count = static_cast<int32_t>(storage.threads.size());
    d.threads = storage.threads.data();
    *out = &d;
    return PC_OK;
}

int32_t pc_monitor_open_files(pc_monitor *monitor, int32_t pid, const pc_open_file **out, bool *complete) {
    if (out) *out = nullptr;
    if (complete) *complete = false;
    if (!monitor || !(monitor->capabilities & PC_CAP_OPEN_FILES)) return 0;
    auto &data = monitor->open_file_data;
    bool all = platform::open_files(pid, data);
    if (!all && monitor->helper.connected()) {
        std::vector<platform::OpenFile> elevated;
        bool elevated_all = false;
        if (monitor->helper.open_files(pid, elevated, elevated_all)) {
            data = std::move(elevated);
            all = elevated_all;
        }
        if (!monitor->helper.connected()) monitor->helper_state = PC_HELPER_LOST;
    }
    monitor->open_files.clear();
    monitor->open_files.reserve(data.size());
    for (const auto &f : data) monitor->open_files.push_back({f.pid, f.fd, f.kind, f.path.c_str()});
    if (complete) *complete = all;
    if (out) *out = monitor->open_files.data();
    return static_cast<int32_t>(monitor->open_files.size());
}

int32_t pc_monitor_connections(pc_monitor *monitor, int32_t pid, const pc_connection **out, bool *complete) {
    if (out) *out = nullptr;
    if (complete) *complete = false;
    if (!monitor || !(monitor->capabilities & PC_CAP_CONNECTIONS)) return 0;
    bool all = platform::connections(pid, monitor->connections);
    if (!all && monitor->helper.connected()) {
        std::vector<pc_connection> elevated;
        bool elevated_all = false;
        if (monitor->helper.connections(pid, elevated, elevated_all)) {
            monitor->connections = std::move(elevated);
            all = elevated_all;
        }
        if (!monitor->helper.connected()) monitor->helper_state = PC_HELPER_LOST;
    }
    if (complete) *complete = all;
    if (out) *out = monitor->connections.data();
    return static_cast<int32_t>(monitor->connections.size());
}

bool pc_battery_get(pc_battery *out) { return out && platform::battery(*out); }

int32_t pc_monitor_power_assertions(pc_monitor *monitor, const pc_power_assertion **out) {
    if (!monitor) return 0;
    monitor->power_assertions.clear();
    for (const auto &a : platform::power_assertions()) {
        pc_power_assertion assertion{};
        assertion.pid = a.pid;
        assertion.on_behalf_of = a.on_behalf_of;
        assertion.kind = a.kind;
        assertion.created = a.created;
        copy_string(assertion.type, sizeof(assertion.type), a.type);
        copy_string(assertion.reason, sizeof(assertion.reason), a.reason);
        if (const pc_process *p = find_process(monitor, a.pid))
            std::memcpy(assertion.process_name, p->name, sizeof(assertion.process_name));
        monitor->power_assertions.push_back(assertion);
    }
    if (out) *out = monitor->power_assertions.data();
    return static_cast<int32_t>(monitor->power_assertions.size());
}

int32_t pc_monitor_services(pc_monitor *monitor, const pc_service **out) {
    if (!monitor) return 0;
    monitor->services = (monitor->capabilities & PC_CAP_SERVICES) ? platform::services() : std::vector<pc_service>{};
    if (out) *out = monitor->services.data();
    return static_cast<int32_t>(monitor->services.size());
}

// System-domain changes need root: only through the helper.
static pc_result control(pc_monitor *monitor, int32_t domain, const char *label, int32_t action) {
    if (!monitor || !label || !platform::valid_service_label(label)) return PC_ERR_INVALID;
    if (domain == PC_DOMAIN_USER) return platform::service_control(domain, label, action);
    if (domain != PC_DOMAIN_SYSTEM) return PC_ERR_INVALID;
    if (!monitor->helper.connected()) return PC_ERR_PERMISSION;
    const pc_result result = monitor->helper.launchd(label, action);
    if (!monitor->helper.connected()) monitor->helper_state = PC_HELPER_LOST;
    return result;
}

pc_result pc_service_control(pc_monitor *monitor, int32_t domain, const char *label, int32_t action) {
    if (!monitor || !(monitor->capabilities & PC_CAP_SERVICES)) return PC_ERR_UNSUPPORTED;
    if (action < PC_SERVICE_START || action > PC_SERVICE_DISABLE) return PC_ERR_INVALID;
    return control(monitor, domain, label, action);
}

int32_t pc_monitor_startup_items(pc_monitor *monitor, const pc_startup_item **out) {
    if (!monitor) return 0;
    monitor->startup_items =
        (monitor->capabilities & PC_CAP_STARTUP) ? platform::startup_items() : std::vector<pc_startup_item>{};
    if (out) *out = monitor->startup_items.data();
    return static_cast<int32_t>(monitor->startup_items.size());
}

int32_t pc_monitor_startup_managed_items(pc_monitor *monitor, const pc_startup_item **out, bool *ready) {
    if (ready) *ready = false;
    if (out) *out = nullptr;
    // Reading the list as a normal user makes the OS ask for an administrator password.
    if (!monitor || !(monitor->capabilities & PC_CAP_STARTUP) || !monitor->helper.connected()) return -1;
    bool done = false;
    if (!monitor->helper.startup_items(monitor->managed_startup_items, done)) {
        monitor->helper_state = PC_HELPER_LOST;
        return -1;
    }
    if (ready) *ready = done;
    if (out) *out = monitor->managed_startup_items.data();
    return static_cast<int32_t>(monitor->managed_startup_items.size());
}

pc_result pc_startup_set_enabled(pc_monitor *monitor, int32_t scope, const char *label, bool enabled) {
    if (!monitor || !(monitor->capabilities & PC_CAP_STARTUP)) return PC_ERR_UNSUPPORTED;
    if (scope == PC_STARTUP_OPEN_AT_LOGIN || scope == PC_STARTUP_APP_BACKGROUND) return PC_ERR_UNSUPPORTED;
    if (scope < PC_STARTUP_USER_AGENT || scope > PC_STARTUP_DAEMON) return PC_ERR_INVALID;
    const int32_t domain = scope == PC_STARTUP_DAEMON ? PC_DOMAIN_SYSTEM : PC_DOMAIN_USER;
    return control(monitor, domain, label, enabled ? PC_SERVICE_ENABLE : PC_SERVICE_DISABLE);
}
