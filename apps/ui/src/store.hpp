// The app's view of the core: a sampler thread drives pc_monitor and hands the UI thread copies
// of each snapshot; everything else (views, actions, listings) runs on the UI thread under the
// same lock, because pc_monitor is single-threaded.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "alerts.hpp"
#include "history.hpp"
#include "procyon/procyon.h"
#include "ui.hpp"

namespace procyon::ui {

// One sample a tick for every live chart; the charts show the last 60 seconds (as the macOS app's
// time-based window does), which is 60 samples at 1 s updates, 120 at 0.5 s and 12 at 5 s.
constexpr double kHistorySeconds = 60;
constexpr size_t kHistoryMaxSamples = 120;  // 60 s at the fastest update speed, 0.5 s
// The samples that span kHistorySeconds at the current update speed (set by Store::set_interval).
size_t history_window();
void set_history_interval(double seconds);

class Series {
public:
    void push(float value) {
        values_.push_back(value);
        if (values_.size() > kHistoryMaxSamples * 4)
            values_.erase(values_.begin(), values_.begin() + kHistoryMaxSamples);
    }
    const float *data() const { return values_.data(); }
    size_t size() const { return values_.size(); }
    float last() const { return values_.empty() ? 0 : values_.back(); }
    float max_recent(size_t window = 0) const {
        if (window == 0) window = history_window();
        float m = 0;
        const size_t start = values_.size() > window ? values_.size() - window : 0;
        for (size_t i = start; i < values_.size(); ++i) m = std::max(m, values_[i]);
        return m;
    }
    void clear() { values_.clear(); }

private:
    std::vector<float> values_;
};

// Rounds a chart scale up to 1, 2, 2.5 or 5 times a power of ten (Sparkline auto-scale).
float nice_max(float value);

// A copy of pc_snapshot the UI thread owns.
struct Snapshot {
    double timestamp = 0;
    double interval = 0;
    double cpu_usage = 0, cpu_user = 0, cpu_system = 0;
    std::vector<double> core_usage;
    double load_average[3] = {0, 0, 0};
    double cpu_temperature = -1, disk_temperature = -1;
    uint64_t memory_total = 0, memory_used = 0, memory_app = 0, memory_wired = 0, memory_compressed = 0,
             memory_cached = 0, memory_free = 0, swap_total = 0, swap_used = 0;
    int32_t memory_pressure = PC_PRESSURE_UNKNOWN;
    double disk_read_bps = 0, disk_write_bps = 0, net_rx_bps = 0, net_tx_bps = 0;
    uint64_t disk_read_total = 0, disk_write_total = 0, net_rx_total = 0, net_tx_total = 0;
    int32_t process_count = 0, thread_count = 0, restricted_count = 0;
    std::vector<pc_process> processes;
    std::vector<pc_gpu> gpus;

    const pc_process *find(int32_t pid) const {
        for (const auto &p : processes)
            if (p.pid == pid) return &p;
        return nullptr;
    }
};

struct History {
    Series cpu, cpu_user, cpu_system;
    std::vector<Series> cores;
    Series memory_used, memory_fraction, swap_used, memory_compressed;
    Series disk_read, disk_write, net_rx, net_tx;
    std::vector<Series> gpu;
    Series cpu_temperature, disk_temperature, gpu_temperature;  // the hottest GPU
    Series app_power;  // watts, sum over processes (hidden without PC_CAP_PROCESS_ENERGY)
};

// A view row the UI owns (pc_row points into the monitor).
struct Row {
    int32_t process_index = -1;
    int32_t parent_row = -1;
    int32_t depth = 0;
    int32_t child_count = 0;
    int32_t process_count = 1;
    double cpu_percent = -1;
    int64_t memory_bytes = -1;
    double disk_read_bps = -1, disk_write_bps = -1, net_rx_bps = -1, net_tx_bps = -1;
    int32_t threads = -1;
    double gpu_percent = -1;
    double power_watts = -1;
    std::string group_id;
    std::wstring group_name;
    int32_t group_pid = 0;
    bool is_group() const { return process_index < 0; }
};

struct ViewQuery {
    int32_t mode = PC_VIEW_FLAT;
    int32_t sort_column = PC_COLUMN_CPU;
    bool descending = true;
    std::string filter;
    int32_t limit = 0;
};

struct ThreadCopy {
    uint64_t id;
    std::wstring name;
    double cpu_percent;
    uint64_t user_time_ns, system_time_ns;
    int32_t priority;
    int32_t state;
};

struct DetailsCopy {
    pc_process_details info{};
    std::wstring cwd;
    std::vector<std::wstring> arguments;
    std::vector<std::wstring> environment;
    std::vector<ThreadCopy> threads;
};

struct OpenFileCopy {
    int32_t pid;
    int32_t fd;
    int32_t kind;
    std::wstring path;
};

class Store {
public:
    Store();
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    // The sampler thread calls `deliver` with each new snapshot; it must hand the pointer to the UI
    // thread (which passes it to receive()) and return true, or return false to drop it.
    bool start(std::function<bool(Snapshot *)> deliver);
    void stop();
    void receive(Snapshot *snapshot);

    uint32_t capabilities() const { return capabilities_; }
    bool has(pc_capability flag) const { return (capabilities_ & flag) != 0; }
    const pc_system_info &system_info() const { return info_; }
    const Snapshot &snapshot() const { return snapshot_; }
    const History &history() const { return history_; }
    bool has_snapshot() const { return ticks_ > 0; }
    uint64_t ticks() const { return ticks_; }

    double interval() const { return interval_; }
    void set_interval(double seconds);
    bool paused() const { return paused_; }
    void set_paused(bool paused);
    // Whether the window needs per-process data; alert rules that watch apps keep it on anyway.
    void set_process_sampling(bool enabled);
    void refresh_now();

    // ---- history (the last 24 hours on disk) and alerts ----
    HistoryDatabase &history_database() { return history_db_; }
    bool records_history() const { return records_history_; }
    void set_records_history(bool enabled);
    // Writes the minute in progress (call before the process exits).
    void flush_history();
    const AlertSettings &alert_settings() const { return alert_settings_; }
    void set_alert_settings(const AlertSettings &settings);
    // Alerts raised this session, newest first.
    const std::vector<AlertEvent> &recent_alerts() const { return recent_alerts_; }
    void set_alert_handler(std::function<void(const AlertEvent &)> handler) { on_alert_ = std::move(handler); }

    // ---- everything below locks the monitor briefly ----
    std::vector<Row> build_view(const ViewQuery &query);
    pc_result end_process(int32_t pid, bool force);
    pc_result end_tree(int32_t pid);
    pc_result suspend(int32_t pid);
    pc_result resume(int32_t pid);
    pc_result set_priority(int32_t pid, int32_t nice);
    pc_result send_signal(int32_t pid, int32_t signal);
    pc_result set_affinity(int32_t pid, uint64_t mask);
    std::optional<DetailsCopy> details(int32_t pid);
    std::vector<OpenFileCopy> open_files(int32_t pid, bool &complete);
    std::vector<pc_connection> connections(int32_t pid, bool &complete);
    std::vector<pc_volume> volumes();
    // Services take about a second to list on Linux (systemctl over every unit): they are listed on a
    // worker thread, as the macOS app does, while the Services screen shows the last list.
    // request_services() starts a listing unless one runs; services() is the last one, with the
    // number of listings so far (0: none yet) so the page knows when to take a new one.
    void request_services();
    std::vector<pc_service> services(uint64_t *generation = nullptr) const;
    bool services_loading() const { return services_busy_.load(); }
    pc_result service_control(int32_t domain, const std::string &label, int32_t action);
    std::vector<pc_startup_item> startup_items();
    pc_result startup_set_enabled(int32_t scope, const std::string &label, bool enabled);
    std::vector<pc_power_assertion> power_assertions();
    std::optional<pc_battery> battery();

private:
    void run();
    void copy_snapshot(const pc_snapshot &from, Snapshot &to);
    void record(const Snapshot &snapshot);
    // Feeds the minute recorder and the alert rules with this snapshot's machine figures and
    // busiest apps.
    void observe(const Snapshot &snapshot);
    void apply_process_sampling();

    pc_monitor *monitor_ = nullptr;
    std::mutex monitor_mutex_;
    uint32_t capabilities_ = 0;
    pc_system_info info_{};

    std::thread thread_;
    std::thread services_thread_;
    std::atomic<bool> services_busy_{false};
    std::atomic<bool> services_again_{false};  // asked during a listing: list once more after it
    mutable std::mutex services_mutex_;
    std::vector<pc_service> services_;
    uint64_t services_generation_ = 0;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> refresh_requested_{false};
    std::atomic<double> interval_{1.0};
    std::function<bool(Snapshot *)> deliver_;

    Snapshot snapshot_;
    History history_;
    uint64_t ticks_ = 0;

    bool window_wants_processes_ = true;
    bool records_history_ = true;
    HistoryRecorder recorder_;
    HistoryDatabase history_db_;
    AlertSettings alert_settings_;
    AlertEvaluator evaluator_;
    std::vector<AlertEvent> recent_alerts_;
    std::function<void(const AlertEvent &)> on_alert_;
};

}  // namespace procyon::ui
