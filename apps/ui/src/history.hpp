// The last 24 hours, minute by minute: the recorder averages each minute's samples, the database
// keeps the minutes on disk (a port of ProcyonKit's History.swift; a flat record file instead of
// SQLite, which Windows doesn't ship).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace procyon::ui {

// Machine-wide figures for one minute: averages, plus the CPU peak. Unknown values are -1.
struct MachineMinute {
    int64_t minute = 0;  // unix time of the minute's start
    double cpu = 0;
    double cpu_peak = 0;
    double memory = 0;  // fraction of physical memory in use
    double disk_read = 0;
    double disk_write = 0;
    double net_rx = 0;
    double net_tx = 0;
    double gpu = -1;
    double cpu_temperature = -1;
    // Whether per-process data was sampled during the minute (false while only the tray was
    // showing): minutes without it don't count against apps' averages.
    bool processes_sampled = true;
};

// One app's average use over a minute, or over a range of minutes when summed by the database.
struct AppUsage {
    std::string app_id;
    std::wstring name;
    double cpu = 0;  // percent of one core
    double memory = 0;
    double disk = 0;
    double network = 0;
    double gpu = 0;
};

// What one minute adds to the history.
struct MinuteRecord {
    MachineMinute machine;
    std::vector<AppUsage> apps;
};

// What the recorder and the alert evaluator read from a snapshot (the Store fills these).
struct MachineSample {
    double timestamp = 0;
    double cpu = 0;
    double memory_fraction = 0;
    uint64_t memory_total = 0;
    int32_t memory_pressure = -1;
    double disk_read = 0, disk_write = 0, net_rx = 0, net_tx = 0;
    double gpu = -1;
    double cpu_temperature = -1;
    bool has_processes = false;  // the snapshot carried processes
};

// A busy app of one sample: a grouped top-level row. Unknown rates are -1.
struct AppSample {
    std::string app_id;
    std::wstring name;
    double cpu = -1;
    int64_t memory = -1;
    double disk = -1;  // read + write
    double network = -1;
    double gpu = -1;
};

// Averages the samples of the current minute; hands out a record when the minute is over.
class HistoryRecorder {
public:
    // `busiest`: the top apps of this sample by each metric (grouped rows, possibly overlapping).
    std::optional<MinuteRecord> add(const MachineSample &sample, const std::vector<AppSample> &busiest);
    // The minute in progress so far (averaged over its samples), to write when sampling stops or
    // the app quits: a later sample of the same minute replaces it. Empty before the first sample.
    std::optional<MinuteRecord> flush() const { return record(); }

private:
    void reset(int64_t minute);
    std::optional<MinuteRecord> record() const;

    std::optional<int64_t> minute_;
    MachineMinute machine_;
    int samples_ = 0, gpu_samples_ = 0, temperature_samples_ = 0, app_samples_ = 0;
    std::unordered_map<std::string, AppUsage> apps_;
};

enum class AppOrder { Cpu, Memory, Disk, Network, Gpu };

// The history on disk: one record per minute with the busy apps of that minute, appended to a
// file in the data folder (%LOCALAPPDATA%\Procyon, ~/.local/share/procyon) and compacted now and then. Everything is
// also kept in memory (a day is 1,440 minutes), so queries don't touch the disk.
class HistoryDatabase {
public:
    static constexpr int64_t kRetention = 24 * 3600;  // how far back the history goes, seconds

    static std::wstring default_path();
    explicit HistoryDatabase(std::wstring path = default_path());

    void write(const MinuteRecord &record);
    // Minutes from `since` (unix seconds) on, oldest first.
    std::vector<MachineMinute> machine(int64_t since);
    // Each app's average over the minutes in `from ..< to` during which processes were sampled (a
    // sampled minute it wasn't busy counts as zero; minutes without process data don't count),
    // biggest first by `order`.
    std::vector<AppUsage> apps(int64_t from, int64_t to, AppOrder order, size_t limit = 10);
    // Bytes the history takes on disk.
    int64_t size();
    void clear();

private:
    void load();
    void prune(int64_t now);
    bool append(const MinuteRecord &record);
    void rewrite();

    std::wstring path_;
    bool loaded_ = false;
    std::map<int64_t, MinuteRecord> minutes_;
    int writes_ = 0;
};

}  // namespace procyon::ui
