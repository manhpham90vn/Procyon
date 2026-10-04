#include "store.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "platform.hpp"

namespace procyon::ui {

float nice_max(float value) {
    if (!(value > 0)) return 1;
    const float exponent = std::floor(std::log10(value));
    const float base = std::pow(10.0f, exponent);
    const float mantissa = value / base;
    float step = 10;
    for (float candidate : {1.0f, 2.0f, 2.5f, 5.0f, 10.0f}) {
        if (mantissa <= candidate) {
            step = candidate;
            break;
        }
    }
    return step * base;
}

Store::Store() : alert_settings_(AlertSettings::load()) {
    capabilities_ = pc_capabilities();
    pc_system_info_get(&info_);
    monitor_ = pc_monitor_create();
}

Store::~Store() {
    stop();
    flush_history();
    if (monitor_) pc_monitor_destroy(monitor_);
}

bool Store::start(std::function<bool(Snapshot *)> deliver) {
    if (running_) return true;
    deliver_ = std::move(deliver);
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Store::stop() {
    if (!running_) return;
    running_ = false;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Store::set_interval(double seconds) {
    interval_ = std::clamp(seconds, 0.5, 5.0);
    wake_.notify_all();
}

void Store::set_paused(bool paused) {
    // Pausing stops every update, alerts included; the time paused doesn't count toward any
    // alert's duration.
    if (paused && !paused_) evaluator_.reset();
    paused_ = paused;
    wake_.notify_all();
}

void Store::set_process_sampling(bool enabled) {
    window_wants_processes_ = enabled;
    apply_process_sampling();
}

void Store::apply_process_sampling() {
    std::lock_guard lock(monitor_mutex_);
    pc_monitor_set_process_sampling(monitor_, window_wants_processes_ || alert_settings_.watches_apps());
}

void Store::set_records_history(bool enabled) {
    if (records_history_ == enabled) return;
    records_history_ = enabled;
    if (!enabled) flush_history();
}

void Store::flush_history() {
    if (!records_history_) return;
    if (const auto record = recorder_.flush()) history_db_.write(*record);
}

void Store::set_alert_settings(const AlertSettings &settings) {
    // An edited rule starts its clock over, so a lowered threshold doesn't fire at once.
    for (const AlertRule &rule : settings.rules)
        if (alert_settings_.rule(rule.kind) != rule) evaluator_.reset(rule.kind);
    alert_settings_ = settings;
    alert_settings_.save();
    apply_process_sampling();
}

void Store::refresh_now() {
    refresh_requested_ = true;
    wake_.notify_all();
}

void Store::run() {
    platform::sampler_thread_begin();
    auto next = std::chrono::steady_clock::now();
    while (running_) {
        {
            std::unique_lock lock(wake_mutex_);
            if (paused_ && !refresh_requested_)
                wake_.wait(lock, [this] { return !running_ || !paused_ || refresh_requested_; });
            else
                wake_.wait_until(lock, next, [this] { return !running_ || refresh_requested_; });
        }
        if (!running_) break;
        refresh_requested_ = false;
        auto snapshot = std::make_unique<Snapshot>();
        {
            std::lock_guard lock(monitor_mutex_);
            const pc_snapshot *snap = pc_monitor_refresh(monitor_);
            if (snap) copy_snapshot(*snap, *snapshot);
        }
        // The window takes ownership once it accepted the hand-over to its thread.
        if (deliver_ && deliver_(snapshot.get())) snapshot.release();
        next =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(interval_.load() * 1000));
    }
    platform::sampler_thread_end();
}

void Store::copy_snapshot(const pc_snapshot &from, Snapshot &to) {
    to.timestamp = from.timestamp;
    to.interval = from.interval;
    to.cpu_usage = from.cpu_usage;
    to.cpu_user = from.cpu_user;
    to.cpu_system = from.cpu_system;
    to.core_usage.assign(from.core_usage, from.core_usage + std::max(0, from.core_count));
    std::memcpy(to.load_average, from.load_average, sizeof(to.load_average));
    to.cpu_temperature = from.cpu_temperature;
    to.disk_temperature = from.disk_temperature;
    to.memory_total = from.memory_total;
    to.memory_used = from.memory_used;
    to.memory_app = from.memory_app;
    to.memory_wired = from.memory_wired;
    to.memory_compressed = from.memory_compressed;
    to.memory_cached = from.memory_cached;
    to.memory_free = from.memory_free;
    to.swap_total = from.swap_total;
    to.swap_used = from.swap_used;
    to.memory_pressure = from.memory_pressure;
    to.disk_read_bps = from.disk_read_bps;
    to.disk_write_bps = from.disk_write_bps;
    to.net_rx_bps = from.net_rx_bps;
    to.net_tx_bps = from.net_tx_bps;
    to.disk_read_total = from.disk_read_total;
    to.disk_write_total = from.disk_write_total;
    to.net_rx_total = from.net_rx_total;
    to.net_tx_total = from.net_tx_total;
    to.process_count = from.process_count;
    to.thread_count = from.thread_count;
    to.restricted_count = from.restricted_count;
    to.processes.assign(from.processes, from.processes + std::max(0, from.process_count));
    to.gpus.assign(from.gpus, from.gpus + std::max(0, from.gpu_count));
}

void Store::receive(Snapshot *snapshot) {
    std::unique_ptr<Snapshot> owned(snapshot);
    if (!owned) return;
    snapshot_ = std::move(*owned);
    ++ticks_;
    record(snapshot_);
    observe(snapshot_);
}

void Store::observe(const Snapshot &s) {
    if (!records_history_ && !alert_settings_.any_enabled()) return;
    MachineSample sample;
    sample.timestamp = s.timestamp;
    sample.cpu = s.cpu_usage;
    sample.memory_total = s.memory_total;
    sample.memory_fraction = s.memory_total ? static_cast<double>(s.memory_used) / s.memory_total : 0;
    sample.memory_pressure = s.memory_pressure;
    sample.disk_read = s.disk_read_bps;
    sample.disk_write = s.disk_write_bps;
    sample.net_rx = s.net_rx_bps;
    sample.net_tx = s.net_tx_bps;
    if (!s.gpus.empty() && s.gpus[0].utilization >= 0) sample.gpu = s.gpus[0].utilization;
    sample.cpu_temperature = s.cpu_temperature;
    sample.has_processes = !s.processes.empty();

    // The busiest apps by every metric, from one by-app view (each view regroups and sorts every
    // process in the core).
    std::vector<AppSample> busiest;
    if (sample.has_processes) {
        ViewQuery query;
        query.mode = PC_VIEW_GROUPED;
        query.sort_column = PC_COLUMN_CPU;
        query.descending = true;
        std::vector<AppSample> apps;
        for (const Row &row : build_view(query)) {
            if (row.depth > 0) continue;
            AppSample app;
            if (row.is_group()) {
                app.app_id = row.group_id;
                app.name = row.group_name;
            } else if (const pc_process *p =
                           row.process_index >= 0 && row.process_index < static_cast<int32_t>(s.processes.size())
                               ? &s.processes[static_cast<size_t>(row.process_index)]
                               : nullptr) {
                app.app_id = p->app_id;
                app.name = fmt::from_utf8(p->app_name[0] ? p->app_name : p->name);
            } else {
                continue;
            }
            app.cpu = row.cpu_percent;
            app.memory = row.memory_bytes;
            app.disk = row.disk_read_bps < 0 && row.disk_write_bps < 0
                           ? -1
                           : std::max(row.disk_read_bps, 0.0) + std::max(row.disk_write_bps, 0.0);
            app.network = row.net_rx_bps < 0 && row.net_tx_bps < 0
                              ? -1
                              : std::max(row.net_rx_bps, 0.0) + std::max(row.net_tx_bps, 0.0);
            app.gpu = row.gpu_percent;
            apps.push_back(std::move(app));
        }
        // The union of the top ten by each metric (an app missing from them used little).
        constexpr size_t kTop = 10;
        std::vector<char> taken(apps.size(), 0);
        auto take = [&](auto value, double floor) {
            std::vector<size_t> order;
            for (size_t i = 0; i < apps.size(); ++i)
                if (value(apps[i]) > floor) order.push_back(i);
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return value(apps[a]) > value(apps[b]); });
            for (size_t n = 0; n < order.size() && n < kTop; ++n) taken[order[n]] = 1;
        };
        take([](const AppSample &a) { return a.cpu; }, -1);
        take([](const AppSample &a) { return static_cast<double>(a.memory); }, -1);
        take([](const AppSample &a) { return a.disk; }, 0);
        if (has(PC_CAP_PROCESS_NETWORK)) take([](const AppSample &a) { return a.network; }, 0);
        if (has(PC_CAP_PROCESS_GPU)) take([](const AppSample &a) { return a.gpu; }, 0);
        for (size_t i = 0; i < apps.size(); ++i)
            if (taken[i]) busiest.push_back(std::move(apps[i]));
    }

    if (records_history_) {
        if (const auto finished = recorder_.add(sample, busiest)) history_db_.write(*finished);
    }
    if (alert_settings_.any_enabled()) {
        for (AlertEvent &event : evaluator_.evaluate(sample, busiest, alert_settings_, s.timestamp)) {
            recent_alerts_.insert(recent_alerts_.begin(), event);
            if (recent_alerts_.size() > 50) recent_alerts_.resize(50);
            if (on_alert_) on_alert_(event);
        }
    }
}

void Store::record(const Snapshot &s) {
    history_.cpu.push(static_cast<float>(s.cpu_usage));
    history_.cpu_user.push(static_cast<float>(s.cpu_user));
    history_.cpu_system.push(static_cast<float>(s.cpu_system));
    if (history_.cores.size() != s.core_usage.size()) history_.cores.assign(s.core_usage.size(), Series{});
    for (size_t i = 0; i < s.core_usage.size(); ++i) history_.cores[i].push(static_cast<float>(s.core_usage[i]));
    history_.memory_used.push(static_cast<float>(s.memory_used));
    history_.memory_fraction.push(s.memory_total ? static_cast<float>(s.memory_used) / s.memory_total : 0);
    history_.swap_used.push(static_cast<float>(s.swap_used));
    history_.memory_compressed.push(static_cast<float>(s.memory_compressed));
    history_.disk_read.push(static_cast<float>(s.disk_read_bps));
    history_.disk_write.push(static_cast<float>(s.disk_write_bps));
    history_.net_rx.push(static_cast<float>(s.net_rx_bps));
    history_.net_tx.push(static_cast<float>(s.net_tx_bps));
    if (history_.gpu.size() != s.gpus.size()) history_.gpu.assign(s.gpus.size(), Series{});
    for (size_t i = 0; i < s.gpus.size(); ++i)
        history_.gpu[i].push(s.gpus[i].utilization >= 0 ? static_cast<float>(s.gpus[i].utilization) : 0);
    history_.cpu_temperature.push(s.cpu_temperature >= 0 ? static_cast<float>(s.cpu_temperature) : 0);
    history_.disk_temperature.push(s.disk_temperature >= 0 ? static_cast<float>(s.disk_temperature) : 0);
    double gpu_hottest = -1;
    for (const pc_gpu &g : s.gpus) gpu_hottest = std::max(gpu_hottest, g.temperature);
    history_.gpu_temperature.push(gpu_hottest >= 0 ? static_cast<float>(gpu_hottest) : 0);
    double power = 0;
    for (const auto &p : s.processes)
        if (p.power_watts > 0) power += p.power_watts;
    history_.app_power.push(static_cast<float>(power));
}

std::vector<Row> Store::build_view(const ViewQuery &query) {
    std::vector<Row> out;
    pc_view_query q{};
    q.mode = query.mode;
    q.sort_column = query.sort_column;
    q.descending = query.descending;
    q.filter = query.filter.c_str();
    q.limit = query.limit;
    std::lock_guard lock(monitor_mutex_);
    const pc_row *rows = nullptr;
    const int32_t count = pc_monitor_build_view(monitor_, &q, &rows);
    out.reserve(static_cast<size_t>(std::max(0, count)));
    for (int32_t i = 0; i < count; ++i) {
        const pc_row &r = rows[i];
        Row row;
        row.process_index = r.process_index;
        row.parent_row = r.parent_row;
        row.depth = r.depth;
        row.child_count = r.child_count;
        row.process_count = r.process_count;
        row.cpu_percent = r.cpu_percent;
        row.memory_bytes = r.memory_bytes;
        row.disk_read_bps = r.disk_read_bps;
        row.disk_write_bps = r.disk_write_bps;
        row.net_rx_bps = r.net_rx_bps;
        row.net_tx_bps = r.net_tx_bps;
        row.threads = r.threads;
        row.gpu_percent = r.gpu_percent;
        row.power_watts = r.power_watts;
        if (r.group_id) row.group_id = r.group_id;
        if (r.group_name) row.group_name = fmt::from_utf8(r.group_name);
        row.group_pid = r.group_pid;
        out.push_back(std::move(row));
    }
    return out;
}

pc_result Store::end_process(int32_t pid, bool force) {
    std::lock_guard lock(monitor_mutex_);
    return pc_process_end(monitor_, pid, force);
}

pc_result Store::end_tree(int32_t pid) {
    std::lock_guard lock(monitor_mutex_);
    return pc_process_end_tree(monitor_, pid);
}

pc_result Store::suspend(int32_t pid) {
    std::lock_guard lock(monitor_mutex_);
    return pc_process_suspend(monitor_, pid);
}

pc_result Store::resume(int32_t pid) {
    std::lock_guard lock(monitor_mutex_);
    return pc_process_resume(monitor_, pid);
}

pc_result Store::set_priority(int32_t pid, int32_t nice) {
    std::lock_guard lock(monitor_mutex_);
    return pc_process_set_priority(monitor_, pid, nice);
}

pc_result Store::set_affinity(int32_t pid, uint64_t mask) {
    std::lock_guard lock(monitor_mutex_);
    return pc_process_set_affinity(monitor_, pid, mask);
}

std::optional<DetailsCopy> Store::details(int32_t pid) {
    std::lock_guard lock(monitor_mutex_);
    const pc_process_details *d = nullptr;
    if (pc_process_details_get(monitor_, pid, &d) != PC_OK || !d) return std::nullopt;
    DetailsCopy copy;
    copy.info = *d;
    copy.info.arguments = nullptr;
    copy.info.environment = nullptr;
    copy.info.threads = nullptr;
    copy.cwd = fmt::from_utf8(d->cwd);
    for (int32_t i = 0; i < d->argument_count; ++i) copy.arguments.push_back(fmt::from_utf8(d->arguments[i]));
    for (int32_t i = 0; i < d->environment_count; ++i) copy.environment.push_back(fmt::from_utf8(d->environment[i]));
    for (int32_t i = 0; i < d->thread_count; ++i) {
        const pc_thread &t = d->threads[i];
        copy.threads.push_back(
            {t.id, fmt::from_utf8(t.name), t.cpu_percent, t.user_time_ns, t.system_time_ns, t.priority, t.state});
    }
    return copy;
}

std::vector<OpenFileCopy> Store::open_files(int32_t pid, bool &complete) {
    std::lock_guard lock(monitor_mutex_);
    const pc_open_file *files = nullptr;
    const int32_t count = pc_monitor_open_files(monitor_, pid, &files, &complete);
    std::vector<OpenFileCopy> out;
    out.reserve(static_cast<size_t>(std::max(0, count)));
    for (int32_t i = 0; i < count; ++i)
        out.push_back({files[i].pid, files[i].fd, files[i].kind, fmt::from_utf8(files[i].path ? files[i].path : "")});
    return out;
}

std::vector<pc_connection> Store::connections(int32_t pid, bool &complete) {
    std::lock_guard lock(monitor_mutex_);
    const pc_connection *rows = nullptr;
    const int32_t count = pc_monitor_connections(monitor_, pid, &rows, &complete);
    return std::vector<pc_connection>(rows, rows + std::max(0, count));
}

std::vector<pc_volume> Store::volumes() {
    std::lock_guard lock(monitor_mutex_);
    const pc_volume *rows = nullptr;
    const int32_t count = pc_monitor_volumes(monitor_, &rows);
    return std::vector<pc_volume>(rows, rows + std::max(0, count));
}

std::vector<pc_service> Store::services() {
    std::lock_guard lock(monitor_mutex_);
    const pc_service *rows = nullptr;
    const int32_t count = pc_monitor_services(monitor_, &rows);
    return std::vector<pc_service>(rows, rows + std::max(0, count));
}

pc_result Store::service_control(int32_t domain, const std::string &label, int32_t action) {
    std::lock_guard lock(monitor_mutex_);
    return pc_service_control(monitor_, domain, label.c_str(), action);
}

std::vector<pc_startup_item> Store::startup_items() {
    std::lock_guard lock(monitor_mutex_);
    const pc_startup_item *rows = nullptr;
    const int32_t count = pc_monitor_startup_items(monitor_, &rows);
    return std::vector<pc_startup_item>(rows, rows + std::max(0, count));
}

pc_result Store::startup_set_enabled(int32_t scope, const std::string &label, bool enabled) {
    std::lock_guard lock(monitor_mutex_);
    return pc_startup_set_enabled(monitor_, scope, label.c_str(), enabled);
}

std::vector<pc_power_assertion> Store::power_assertions() {
    std::lock_guard lock(monitor_mutex_);
    const pc_power_assertion *rows = nullptr;
    const int32_t count = pc_monitor_power_assertions(monitor_, &rows);
    return std::vector<pc_power_assertion>(rows, rows + std::max(0, count));
}

std::optional<pc_battery> Store::battery() {
    pc_battery b{};
    if (!pc_battery_get(&b) || !b.present) return std::nullopt;
    return b;
}

}  // namespace procyon::ui
