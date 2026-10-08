#include "history.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "platform.hpp"
#include "ui.hpp"

namespace procyon::ui {

// ---- recorder ----

std::optional<MinuteRecord> HistoryRecorder::add(const MachineSample &sample, const std::vector<AppSample> &busiest) {
    const int64_t current = static_cast<int64_t>(sample.timestamp) / 60 * 60;
    std::optional<MinuteRecord> finished;
    if (minute_ && *minute_ != current) finished = record();
    if (!minute_ || *minute_ != current) reset(current);

    ++samples_;
    machine_.cpu += sample.cpu;
    machine_.cpu_peak = std::max(machine_.cpu_peak, sample.cpu);
    machine_.memory += sample.memory_fraction;
    machine_.disk_read += sample.disk_read;
    machine_.disk_write += sample.disk_write;
    machine_.net_rx += sample.net_rx;
    machine_.net_tx += sample.net_tx;
    if (sample.gpu >= 0) {
        machine_.gpu = (machine_.gpu < 0 ? 0 : machine_.gpu) + sample.gpu;
        ++gpu_samples_;
    }
    if (sample.cpu_temperature >= 0) {
        machine_.cpu_temperature =
            (machine_.cpu_temperature < 0 ? 0 : machine_.cpu_temperature) + sample.cpu_temperature;
        ++temperature_samples_;
    }

    // A sample with processes but no busy app still counts: every app used nothing then.
    if (!sample.has_processes && busiest.empty()) return finished;
    ++app_samples_;
    for (const AppSample &app : busiest) {
        auto it = apps_.find(app.app_id);
        if (it == apps_.end()) {
            AppUsage usage;
            usage.app_id = app.app_id;
            usage.name = app.name;
            it = apps_.emplace(app.app_id, std::move(usage)).first;
        } else if (it->second.name.empty()) {
            it->second.name = app.name;
        }
        AppUsage &usage = it->second;
        usage.cpu += std::max(app.cpu, 0.0);
        usage.memory += static_cast<double>(std::max<int64_t>(app.memory, 0));
        usage.disk += std::max(app.disk, 0.0);
        usage.network += std::max(app.network, 0.0);
        usage.gpu += std::max(app.gpu, 0.0);
    }
    return finished;
}

void HistoryRecorder::reset(int64_t minute) {
    minute_ = minute;
    machine_ = MachineMinute{};
    machine_.minute = minute;
    samples_ = gpu_samples_ = temperature_samples_ = app_samples_ = 0;
    apps_.clear();
}

std::optional<MinuteRecord> HistoryRecorder::record() const {
    if (samples_ == 0) return std::nullopt;
    MinuteRecord out;
    MachineMinute &m = out.machine;
    m = machine_;
    m.processes_sampled = app_samples_ > 0;
    const double n = samples_;
    m.cpu /= n;
    m.memory /= n;
    m.disk_read /= n;
    m.disk_write /= n;
    m.net_rx /= n;
    m.net_tx /= n;
    if (m.gpu >= 0) m.gpu /= std::max(gpu_samples_, 1);
    if (m.cpu_temperature >= 0) m.cpu_temperature /= std::max(temperature_samples_, 1);
    // An app missing from a sample's top lists used little then: count it as zero.
    const double a = std::max(app_samples_, 1);
    out.apps.reserve(apps_.size());
    for (const auto &[id, usage] : apps_) {
        AppUsage app = usage;
        app.cpu /= a;
        app.memory /= a;
        app.disk /= a;
        app.network /= a;
        app.gpu /= a;
        out.apps.push_back(std::move(app));
    }
    return out;
}

// ---- database ----

namespace {

constexpr char kMagic[4] = {'P', 'C', 'H', '1'};
constexpr uint32_t kVersion = 1;

int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// Little-endian writer into a byte buffer.
struct Writer {
    std::string bytes;
    template <typename T>
    void raw(const T &value) {
        bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
    }
    void u8(uint8_t v) { raw(v); }
    void u32(uint32_t v) { raw(v); }
    void i64(int64_t v) { raw(v); }
    void f64(double v) { raw(v); }
    void str(const std::string &s) {
        u32(static_cast<uint32_t>(s.size()));
        bytes.append(s);
    }
    void wstr(const std::wstring &s) { str(fmt::to_utf8(s)); }
};

struct Reader {
    const char *at;
    const char *end;
    bool ok = true;
    template <typename T>
    T raw() {
        T value{};
        if (at + sizeof(T) > end) {
            ok = false;
            at = end;
            return value;
        }
        std::memcpy(&value, at, sizeof(T));
        at += sizeof(T);
        return value;
    }
    uint8_t u8() { return raw<uint8_t>(); }
    uint32_t u32() { return raw<uint32_t>(); }
    int64_t i64() { return raw<int64_t>(); }
    double f64() { return raw<double>(); }
    std::string str() {
        const uint32_t size = u32();
        if (!ok || at + size > end || size > (1u << 20)) {
            ok = false;
            at = end;
            return {};
        }
        std::string value(at, size);
        at += size;
        return value;
    }
    std::wstring wstr() { return fmt::from_utf8(str()); }
};

void encode(const MinuteRecord &record, Writer &w) {
    const MachineMinute &m = record.machine;
    w.i64(m.minute);
    w.f64(m.cpu);
    w.f64(m.cpu_peak);
    w.f64(m.memory);
    w.f64(m.disk_read);
    w.f64(m.disk_write);
    w.f64(m.net_rx);
    w.f64(m.net_tx);
    w.f64(m.gpu);
    w.f64(m.cpu_temperature);
    w.u8(m.processes_sampled ? 1 : 0);
    w.u32(static_cast<uint32_t>(record.apps.size()));
    for (const AppUsage &app : record.apps) {
        w.str(app.app_id);
        w.wstr(app.name);
        w.f64(app.cpu);
        w.f64(app.memory);
        w.f64(app.disk);
        w.f64(app.network);
        w.f64(app.gpu);
    }
}

bool decode(Reader &r, MinuteRecord &record) {
    MachineMinute &m = record.machine;
    m.minute = r.i64();
    m.cpu = r.f64();
    m.cpu_peak = r.f64();
    m.memory = r.f64();
    m.disk_read = r.f64();
    m.disk_write = r.f64();
    m.net_rx = r.f64();
    m.net_tx = r.f64();
    m.gpu = r.f64();
    m.cpu_temperature = r.f64();
    m.processes_sampled = r.u8() != 0;
    const uint32_t count = r.u32();
    if (!r.ok || count > 10000) return false;
    record.apps.clear();
    record.apps.reserve(count);
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        AppUsage app;
        app.app_id = r.str();
        app.name = r.wstr();
        app.cpu = r.f64();
        app.memory = r.f64();
        app.disk = r.f64();
        app.network = r.f64();
        app.gpu = r.f64();
        record.apps.push_back(std::move(app));
    }
    return r.ok;
}

std::string read_file(const std::wstring &path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void ensure_directory(const std::wstring &path) { platform::create_parent_directories(path); }

bool file_exists(const std::wstring &path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::path(path), error);
}

}  // namespace

std::wstring HistoryDatabase::default_path() { return platform::data_file(L"history.bin"); }

HistoryDatabase::HistoryDatabase(std::wstring path) : path_(std::move(path)) {}

void HistoryDatabase::load() {
    if (loaded_) return;
    loaded_ = true;
    const std::string bytes = read_file(path_);
    bool damaged = false;
    if (bytes.size() >= sizeof(kMagic) + sizeof(uint32_t) && std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0) {
        Reader header{bytes.data() + sizeof(kMagic), bytes.data() + bytes.size()};
        if (header.u32() == kVersion) {
            const char *at = header.at;
            const char *end = bytes.data() + bytes.size();
            while (at + sizeof(uint32_t) <= end) {
                uint32_t length = 0;
                std::memcpy(&length, at, sizeof(length));
                at += sizeof(length);
                if (length == 0 || at + length > end) {
                    damaged = true;  // a write cut short: keep what came before it
                    break;
                }
                Reader r{at, at + length};
                MinuteRecord record;
                if (decode(r, record)) minutes_[record.machine.minute] = std::move(record);
                at += length;
            }
        } else {
            damaged = true;
        }
    } else if (!bytes.empty()) {
        damaged = true;
    }
    prune(now_seconds());
    if (damaged) rewrite();
}

void HistoryDatabase::prune(int64_t now) {
    const int64_t cutoff = now - kRetention;
    minutes_.erase(minutes_.begin(), minutes_.lower_bound(cutoff));
}

bool HistoryDatabase::append(const MinuteRecord &record) {
    ensure_directory(path_);
    const bool fresh = !file_exists(path_);
    std::ofstream file(std::filesystem::path(path_), std::ios::binary | std::ios::app);
    if (!file) return false;
    if (fresh) {
        file.write(kMagic, sizeof(kMagic));
        const uint32_t version = kVersion;
        file.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }
    Writer w;
    encode(record, w);
    const auto length = static_cast<uint32_t>(w.bytes.size());
    file.write(reinterpret_cast<const char *>(&length), sizeof(length));
    file.write(w.bytes.data(), static_cast<std::streamsize>(w.bytes.size()));
    file.close();
    if (fresh) platform::file_written(path_);
    return static_cast<bool>(file);
}

// Writes the retained minutes afresh: the file otherwise grows with every replaced minute and
// every minute older than a day.
void HistoryDatabase::rewrite() {
    ensure_directory(path_);
    const std::wstring temp = path_ + L".tmp";
    {
        std::ofstream file(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
        if (!file) return;
        file.write(kMagic, sizeof(kMagic));
        const uint32_t version = kVersion;
        file.write(reinterpret_cast<const char *>(&version), sizeof(version));
        for (const auto &[minute, record] : minutes_) {
            Writer w;
            encode(record, w);
            const auto length = static_cast<uint32_t>(w.bytes.size());
            file.write(reinterpret_cast<const char *>(&length), sizeof(length));
            file.write(w.bytes.data(), static_cast<std::streamsize>(w.bytes.size()));
        }
        if (!file) return;
    }
    std::error_code error;
    std::filesystem::rename(std::filesystem::path(temp), std::filesystem::path(path_), error);  // replaces
    if (!error) platform::file_written(path_);
}

void HistoryDatabase::write(const MinuteRecord &record) {
    load();
    minutes_[record.machine.minute] = record;
    prune(std::max(now_seconds(), record.machine.minute));
    ++writes_;
    // A replaced minute (the one in progress is written again when sampling stops) and the day-old
    // ones pile up in the file: rewrite it now and then.
    if (writes_ % 360 == 0)
        rewrite();
    else
        append(record);
}

std::vector<MachineMinute> HistoryDatabase::machine(int64_t since) {
    load();
    std::vector<MachineMinute> out;
    for (auto it = minutes_.lower_bound(since); it != minutes_.end(); ++it) out.push_back(it->second.machine);
    return out;
}

std::vector<AppUsage> HistoryDatabase::apps(int64_t from, int64_t to, AppOrder order, size_t limit) {
    load();
    std::unordered_map<std::string, AppUsage> totals;
    int sampled_minutes = 0;
    for (auto it = minutes_.lower_bound(from); it != minutes_.end() && it->first < to; ++it) {
        if (it->second.machine.processes_sampled) ++sampled_minutes;
        for (const AppUsage &app : it->second.apps) {
            AppUsage &total = totals[app.app_id];
            if (total.app_id.empty()) {
                total.app_id = app.app_id;
                total.name = app.name;
            }
            total.cpu += app.cpu;
            total.memory += app.memory;
            total.disk += app.disk;
            total.network += app.network;
            total.gpu += app.gpu;
        }
    }
    const double n = std::max(sampled_minutes, 1);
    std::vector<AppUsage> out;
    out.reserve(totals.size());
    for (auto &[id, total] : totals) {
        total.cpu /= n;
        total.memory /= n;
        total.disk /= n;
        total.network /= n;
        total.gpu /= n;
        out.push_back(std::move(total));
    }
    const auto value = [order](const AppUsage &a) {
        switch (order) {
            case AppOrder::Cpu: return a.cpu;
            case AppOrder::Memory: return a.memory;
            case AppOrder::Disk: return a.disk;
            case AppOrder::Network: return a.network;
            case AppOrder::Gpu: return a.gpu;
        }
        return a.cpu;
    };
    std::sort(out.begin(), out.end(), [&](const AppUsage &a, const AppUsage &b) { return value(a) > value(b); });
    if (out.size() > limit) out.resize(limit);
    return out;
}

int64_t HistoryDatabase::size() {
    std::error_code error;
    const auto size = std::filesystem::file_size(std::filesystem::path(path_), error);
    return error ? 0 : static_cast<int64_t>(size);
}

void HistoryDatabase::clear() {
    load();
    minutes_.clear();
    std::error_code error;
    std::filesystem::remove(std::filesystem::path(path_), error);
}

}  // namespace procyon::ui
