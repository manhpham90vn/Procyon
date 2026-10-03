#include "alerts.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "procyon/procyon.h"

namespace procyon::ui {

namespace {

const wchar_t *const kAlertsKey = L"Software\\Procyon\\Alerts";

std::wstring fixed(double value, int digits) {
    wchar_t buffer[64];
    (void)swprintf_s(buffer, L"%.*f", digits, value);
    return buffer;
}

std::wstring duration_text(double seconds) {
    if (seconds >= 60) return std::to_wstring(static_cast<int>(seconds / 60)) + L" min";
    return std::to_wstring(static_cast<int>(seconds)) + L" s";
}

}  // namespace

const wchar_t *alert_title(AlertKind kind) {
    switch (kind) {
        case AlertKind::Cpu: return L"CPU usage";
        case AlertKind::Memory: return L"Memory usage";
        case AlertKind::MemoryPressure: return L"Memory pressure is critical";
        case AlertKind::Temperature: return L"CPU temperature";
        case AlertKind::AppCpu: return L"An app's CPU";
        case AlertKind::AppMemory: return L"An app's memory";
    }
    return L"";
}

const char *alert_key(AlertKind kind) {
    switch (kind) {
        case AlertKind::Cpu: return "cpu";
        case AlertKind::Memory: return "memory";
        case AlertKind::MemoryPressure: return "memoryPressure";
        case AlertKind::Temperature: return "temperature";
        case AlertKind::AppCpu: return "appCPU";
        case AlertKind::AppMemory: return "appMemory";
    }
    return "";
}

bool alert_is_per_app(AlertKind kind) { return kind == AlertKind::AppCpu || kind == AlertKind::AppMemory; }

double alert_default_threshold(AlertKind kind) {
    switch (kind) {
        case AlertKind::Cpu: return 90;
        case AlertKind::Memory: return 90;
        case AlertKind::MemoryPressure: return 0;
        case AlertKind::Temperature: return 95;
        case AlertKind::AppCpu: return 150;
        case AlertKind::AppMemory: return 8;
    }
    return 0;
}

double alert_threshold_min(AlertKind kind) {
    switch (kind) {
        case AlertKind::Cpu:
        case AlertKind::Memory: return 50;
        case AlertKind::MemoryPressure: return 0;
        case AlertKind::Temperature: return 60;
        case AlertKind::AppCpu: return 50;
        case AlertKind::AppMemory: return 1;
    }
    return 0;
}

double alert_threshold_max(AlertKind kind) {
    switch (kind) {
        case AlertKind::Cpu:
        case AlertKind::Memory: return 100;
        case AlertKind::MemoryPressure: return 0;
        case AlertKind::Temperature: return 110;
        case AlertKind::AppCpu: return 800;
        case AlertKind::AppMemory: return 64;
    }
    return 0;
}

double alert_step(AlertKind kind) {
    switch (kind) {
        case AlertKind::Cpu:
        case AlertKind::Memory:
        case AlertKind::Temperature: return 5;
        case AlertKind::MemoryPressure: return 1;
        case AlertKind::AppCpu: return 50;
        case AlertKind::AppMemory: return 1;
    }
    return 1;
}

std::wstring alert_threshold_text(AlertKind kind, double threshold, bool fahrenheit) {
    switch (kind) {
        case AlertKind::Cpu:
        case AlertKind::Memory:
        case AlertKind::AppCpu: return fixed(threshold, 0) + L"%";
        case AlertKind::Temperature:
            return fahrenheit ? fixed(threshold * 9 / 5 + 32, 0) + L"°F" : fixed(threshold, 0) + L"°C";
        case AlertKind::AppMemory: return fixed(threshold, 0) + L" GB";
        case AlertKind::MemoryPressure: return L"";
    }
    return L"";
}

// ---- settings ----

AlertSettings::AlertSettings() {
    for (AlertKind kind : kAlertKinds) {
        AlertRule rule;
        rule.kind = kind;
        rule.threshold = alert_default_threshold(kind);
        rules.push_back(rule);
    }
}

AlertRule &AlertSettings::rule(AlertKind kind) {
    for (AlertRule &r : rules)
        if (r.kind == kind) return r;
    AlertRule fresh;
    fresh.kind = kind;
    fresh.threshold = alert_default_threshold(kind);
    rules.push_back(fresh);
    return rules.back();
}

const AlertRule &AlertSettings::rule(AlertKind kind) const {
    for (const AlertRule &r : rules)
        if (r.kind == kind) return r;
    return rules.front();
}

bool AlertSettings::any_enabled() const {
    return std::any_of(rules.begin(), rules.end(), [](const AlertRule &r) { return r.enabled; });
}

bool AlertSettings::watches_apps() const {
    return std::any_of(rules.begin(), rules.end(),
                       [](const AlertRule &r) { return r.enabled && alert_is_per_app(r.kind); });
}

AlertSettings AlertSettings::load() {
    AlertSettings settings;  // kinds added after the settings were saved start off
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAlertsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return settings;
    for (AlertRule &rule : settings.rules) {
        const std::string base = alert_key(rule.kind);
        auto dword = [&](const char *suffix, DWORD fallback) {
            const std::string name = base + "." + suffix;
            const std::wstring wide(name.begin(), name.end());
            DWORD value = fallback, size = sizeof(value);
            if (RegQueryValueExW(key, wide.c_str(), nullptr, nullptr, reinterpret_cast<BYTE *>(&value), &size) !=
                ERROR_SUCCESS)
                return fallback;
            return value;
        };
        rule.enabled = dword("enabled", 0) != 0;
        rule.threshold = std::clamp(static_cast<double>(dword("threshold", static_cast<DWORD>(rule.threshold))),
                                    alert_threshold_min(rule.kind), alert_threshold_max(rule.kind));
        rule.duration = dword("duration", static_cast<DWORD>(rule.duration));
        bool known = false;
        for (double d : kDurations) known |= d == rule.duration;
        if (!known) rule.duration = 60;
    }
    RegCloseKey(key);
    return settings;
}

void AlertSettings::save() const {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAlertsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    for (const AlertRule &rule : rules) {
        const std::string base = alert_key(rule.kind);
        auto dword = [&](const char *suffix, DWORD value) {
            const std::string name = base + "." + suffix;
            const std::wstring wide(name.begin(), name.end());
            RegSetValueExW(key, wide.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
        };
        dword("enabled", rule.enabled ? 1 : 0);
        dword("threshold", static_cast<DWORD>(std::lround(rule.threshold)));
        dword("duration", static_cast<DWORD>(std::lround(rule.duration)));
    }
    RegCloseKey(key);
}

// ---- evaluator ----

namespace {

struct Candidate {
    std::string key;
    double value;
    std::string app_id;
    std::wstring name;
};

// (key, value in the rule's unit, app) for each thing the rule watches.
std::vector<Candidate> candidates(const AlertRule &rule, const MachineSample &s, const std::vector<AppSample> &apps) {
    const std::string kind = alert_key(rule.kind);
    std::vector<Candidate> out;
    switch (rule.kind) {
        case AlertKind::Cpu: out.push_back({kind, s.cpu * 100, {}, {}}); break;
        case AlertKind::Memory:
            if (s.memory_total > 0) out.push_back({kind, s.memory_fraction * 100, {}, {}});
            break;
        case AlertKind::MemoryPressure:
            out.push_back({kind, s.memory_pressure == PC_PRESSURE_CRITICAL ? 1.0 : 0.0, {}, {}});
            break;
        case AlertKind::Temperature:
            if (s.cpu_temperature >= 0) out.push_back({kind, s.cpu_temperature, {}, {}});
            break;
        case AlertKind::AppCpu:
            for (const AppSample &app : apps)
                if (app.cpu >= 0) out.push_back({kind + ":" + app.app_id, app.cpu, app.app_id, app.name});
            break;
        case AlertKind::AppMemory:
            for (const AppSample &app : apps)
                if (app.memory >= 0)
                    out.push_back({kind + ":" + app.app_id, static_cast<double>(app.memory) / 1073741824.0, app.app_id,
                                   app.name});
            break;
    }
    return out;
}

bool exceeds(const AlertRule &rule, double value) {
    return rule.kind == AlertKind::MemoryPressure ? value >= 1 : value >= rule.threshold;
}

AlertEvent event_for(const AlertRule &rule, const Candidate &c, double now) {
    const std::wstring minutes = duration_text(rule.duration);
    const std::wstring name = c.name.empty() ? L"An app" : c.name;
    AlertEvent event;
    event.kind = rule.kind;
    event.time = now;
    event.app_id = c.app_id;
    switch (rule.kind) {
        case AlertKind::Cpu:
            event.title = L"CPU is busy";
            event.message = fixed(c.value, 0) + L"% of the CPU has been in use for " + minutes + L".";
            break;
        case AlertKind::Memory:
            event.title = L"Memory is almost full";
            event.message = fixed(c.value, 0) + L"% of memory has been in use for " + minutes + L".";
            break;
        case AlertKind::MemoryPressure:
            event.title = L"Memory pressure is critical";
            event.message = L"Windows is short of memory: apps may slow down. Close apps you don't need.";
            break;
        case AlertKind::Temperature:
            event.title = L"Your PC is hot";
            event.message = L"The CPU has been at " + fixed(c.value, 0) + L"°C for " + minutes + L".";
            break;
        case AlertKind::AppCpu:
            event.title = L"“" + name + L"” is using a lot of CPU";
            event.message = fixed(c.value, 0) + L"% CPU for " + minutes + L". It may be stuck.";
            break;
        case AlertKind::AppMemory:
            event.title = L"“" + name + L"” is using a lot of memory";
            event.message = fixed(c.value, 1) + L" GB for " + minutes + L".";
            break;
    }
    return event;
}

}  // namespace

std::vector<AlertEvent> AlertEvaluator::evaluate(const MachineSample &sample, const std::vector<AppSample> &apps,
                                                 const AlertSettings &settings, double now) {
    std::vector<AlertEvent> events;
    std::unordered_map<std::string, bool> holding;
    for (const AlertRule &rule : settings.rules) {
        if (!rule.enabled) continue;
        for (const Candidate &c : candidates(rule, sample, apps)) {
            if (!exceeds(rule, c.value)) continue;
            holding[c.key] = true;
            const auto started = since_.emplace(c.key, now).first->second;
            const auto fired = last_fired_.find(c.key);
            if (now - started < rule.duration || (fired != last_fired_.end() && now - fired->second < cooldown))
                continue;
            last_fired_[c.key] = now;
            events.push_back(event_for(rule, c, now));
        }
    }
    // A condition that stopped holding starts over.
    for (auto it = since_.begin(); it != since_.end();) {
        if (holding.count(it->first))
            ++it;
        else
            it = since_.erase(it);
    }
    return events;
}

void AlertEvaluator::reset(AlertKind kind) {
    const std::string key = alert_key(kind);
    const std::string prefix = key + ":";
    for (auto it = since_.begin(); it != since_.end();) {
        if (it->first == key || it->first.compare(0, prefix.size(), prefix) == 0)
            it = since_.erase(it);
        else
            ++it;
    }
}

}  // namespace procyon::ui
