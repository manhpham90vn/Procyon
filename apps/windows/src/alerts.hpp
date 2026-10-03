// Alerts: a condition worth a notification (the machine or one app over a threshold for a while),
// the rules the user keeps, and the evaluator that turns samples into events. A port of
// ProcyonKit's Alerts.swift; the rules live in the registry with the other settings.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "history.hpp"

namespace procyon::ui {

enum class AlertKind {
    Cpu,             // whole-machine CPU, percent of every core
    Memory,          // memory in use, percent of physical memory
    MemoryPressure,  // the OS reports critical memory pressure (threshold unused)
    Temperature,     // hottest CPU sensor, Celsius
    AppCpu,          // one app's CPU, percent of one core
    AppMemory,       // one app's memory, gigabytes
};
constexpr AlertKind kAlertKinds[] = {AlertKind::Cpu,         AlertKind::Memory, AlertKind::MemoryPressure,
                                     AlertKind::Temperature, AlertKind::AppCpu, AlertKind::AppMemory};

const wchar_t *alert_title(AlertKind kind);
const char *alert_key(AlertKind kind);  // storage name
bool alert_is_per_app(AlertKind kind);  // watches individual apps (needs per-process sampling)
double alert_default_threshold(AlertKind kind);
double alert_threshold_min(AlertKind kind);
double alert_threshold_max(AlertKind kind);
double alert_step(AlertKind kind);
// The threshold as the user reads it ("90%", "95°C", "8 GB").
std::wstring alert_threshold_text(AlertKind kind, double threshold, bool fahrenheit);

struct AlertRule {
    AlertKind kind = AlertKind::Cpu;
    bool enabled = false;
    double threshold = 0;
    double duration = 60;  // how long the condition must hold before notifying, seconds

    bool operator==(const AlertRule &o) const {
        return kind == o.kind && enabled == o.enabled && threshold == o.threshold && duration == o.duration;
    }
    bool operator!=(const AlertRule &o) const { return !(*this == o); }
};

// Every rule, persisted under HKCU\Software\Procyon\Alerts.
struct AlertSettings {
    static constexpr double kDurations[] = {10, 30, 60, 300, 900};

    std::vector<AlertRule> rules;

    AlertSettings();
    static AlertSettings load();
    void save() const;

    AlertRule &rule(AlertKind kind);
    const AlertRule &rule(AlertKind kind) const;
    bool any_enabled() const;
    bool watches_apps() const;
};

// One notification to show.
struct AlertEvent {
    AlertKind kind = AlertKind::Cpu;
    std::wstring title;
    std::wstring message;
    double time = 0;     // unix seconds
    std::string app_id;  // the app it is about, for per-app rules
};

// Turns samples into alerts: a rule fires once its condition has held for its duration, then stays
// quiet for `cooldown` (per app for per-app rules) so a busy machine doesn't flood the user.
class AlertEvaluator {
public:
    double cooldown = 15 * 60;

    // `apps`: the busiest apps of this sample (grouped rows).
    std::vector<AlertEvent> evaluate(const MachineSample &sample, const std::vector<AppSample> &apps,
                                     const AlertSettings &settings, double now);
    // Forgets how long every condition has held: after a pause, the time it was not watched must
    // not count toward a rule's duration.
    void reset() { since_.clear(); }
    // Forgets how long `kind`'s conditions have held (for every app, for a per-app rule): a changed
    // threshold or duration starts the clock over instead of firing at once.
    void reset(AlertKind kind);

private:
    std::unordered_map<std::string, double> since_;  // when each condition (rule, or rule + app) started holding
    std::unordered_map<std::string, double> last_fired_;
};

}  // namespace procyon::ui
