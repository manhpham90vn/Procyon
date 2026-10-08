// Battery from /sys/class/power_supply, temperatures from hwmon, and what keeps the machine awake
// from systemd-logind's inhibitor locks.
#if defined(__linux__)

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include "linux_internal.hpp"

namespace procyon::platform {

using namespace linux_internal;

namespace {

// The hwmon inputs to read each tick, found once: CPU package/die sensors and NVMe/SATA drives.
struct Sensors {
    std::vector<std::string> cpu;
    std::vector<std::string> disk;
};

const Sensors &sensors() {
    static const Sensors found = [] {
        Sensors s;
        std::vector<std::string> fallback;  // ACPI thermal zones when no CPU driver is loaded
        for (const auto &h : list_dir("/sys/class/hwmon")) {
            const std::string dir = "/sys/class/hwmon/" + h;
            const std::string name = read_line(dir + "/name");
            for (const auto &file : list_dir(dir)) {
                if (!starts_with(file, "temp") || file.size() < 12 || file.compare(file.size() - 6, 6, "_input") != 0)
                    continue;
                const std::string input = dir + "/" + file;
                const std::string label = read_line(dir + "/" + file.substr(0, file.size() - 6) + "_label");
                if (name == "coretemp") {
                    // "Package id 0" covers the whole die; per-core sensors only when it is missing.
                    if (starts_with(label, "Package")) s.cpu.insert(s.cpu.begin(), input);
                } else if (name == "k10temp" || name == "zenpower") {
                    if (label == "Tctl" || label == "Tdie") s.cpu.push_back(input);
                } else if (name == "cpu_thermal" || name == "soc_thermal") {
                    s.cpu.push_back(input);
                } else if (name == "acpitz") {
                    fallback.push_back(input);
                } else if (name == "nvme" || name == "drivetemp") {
                    if (label.empty() || label == "Composite") s.disk.push_back(input);
                }
            }
        }
        if (s.cpu.empty()) s.cpu = fallback;
        return s;
    }();
    return found;
}

double hottest(const std::vector<std::string> &inputs) {
    double best = -1;
    for (const auto &input : inputs) {
        const int64_t milli = read_int(input, -1);
        if (milli > 0 && milli < 150000) best = std::max(best, milli / 1000.0);
    }
    return best;
}

std::string battery_dir() {
    for (const auto &name : list_dir("/sys/class/power_supply")) {
        const std::string dir = "/sys/class/power_supply/" + name;
        // Peripherals (mice, headsets) are batteries of scope "Device": not the machine's.
        if (read_line(dir + "/type") == "Battery" && read_line(dir + "/scope") != "Device" &&
            read_int(dir + "/present", 1) == 1)
            return dir;
    }
    return {};
}

}  // namespace

double cpu_temperature() { return hottest(sensors().cpu); }

void temperatures(double &cpu, double &disk) {
    cpu = hottest(sensors().cpu);
    disk = hottest(sensors().disk);
}

bool battery(pc_battery &out) {
    out = {};
    out.minutes_to_empty = out.minutes_to_full = -1;
    out.cycle_count = -1;
    out.health = -1;
    out.design_capacity_mah = out.max_capacity_mah = -1;
    out.temperature = -1;
    const std::string dir = battery_dir();

    // Any online mains or USB-C supply means the machine runs on external power.
    for (const auto &name : list_dir("/sys/class/power_supply")) {
        const std::string supply = "/sys/class/power_supply/" + name;
        const std::string type = read_line(supply + "/type");
        if ((type == "Mains" || starts_with(type, "USB")) && read_int(supply + "/online", 0) == 1) {
            out.on_ac_power = true;
            const int64_t max_power = read_int(supply + "/power_max", -1);  // µW, some USB-C ports
            copy_string(out.adapter, sizeof(out.adapter),
                        max_power > 0     ? std::to_string(max_power / 1000000) + " W " + type + " adapter"
                        : type == "Mains" ? "AC adapter"
                                          : type + " power");
        }
    }
    if (dir.empty()) return true;  // a desktop: present = false
    out.present = true;

    const std::string status = read_line(dir + "/status");
    out.charging = status == "Charging";
    out.fully_charged = status == "Full";
    const int64_t capacity = read_int(dir + "/capacity", -1);

    // Energy in µWh and power in µW, or charge in µAh and current in µA (then times the voltage).
    const double voltage = read_int(dir + "/voltage_now", -1) / 1e6;
    const double design_voltage = [&] {
        const int64_t v = read_int(dir + "/voltage_min_design", -1);
        return v > 0 ? v / 1e6 : voltage;
    }();
    double now_wh = -1, full_wh = -1, design_wh = -1, watts = -1;
    if (exists(dir + "/energy_now")) {
        now_wh = read_int(dir + "/energy_now", -1) / 1e6;
        full_wh = read_int(dir + "/energy_full", -1) / 1e6;
        design_wh = read_int(dir + "/energy_full_design", -1) / 1e6;
        const int64_t power = read_int(dir + "/power_now", -1);
        if (power >= 0) watts = power / 1e6;
        if (design_voltage > 0) {
            if (design_wh > 0) out.design_capacity_mah = static_cast<int32_t>(design_wh / design_voltage * 1000);
            if (full_wh > 0) out.max_capacity_mah = static_cast<int32_t>(full_wh / design_voltage * 1000);
        }
    } else if (exists(dir + "/charge_now")) {
        const double now_ah = read_int(dir + "/charge_now", -1) / 1e6;
        const double full_ah = read_int(dir + "/charge_full", -1) / 1e6;
        const double design_ah = read_int(dir + "/charge_full_design", -1) / 1e6;
        if (design_ah > 0) out.design_capacity_mah = static_cast<int32_t>(design_ah * 1000);
        if (full_ah > 0) out.max_capacity_mah = static_cast<int32_t>(full_ah * 1000);
        if (design_voltage > 0) {
            now_wh = now_ah * design_voltage;
            full_wh = full_ah * design_voltage;
            design_wh = design_ah * design_voltage;
        }
        const int64_t current = read_int(dir + "/current_now", -1);
        if (current >= 0 && voltage > 0) watts = current / 1e6 * voltage;
    }

    out.level = capacity >= 0                ? std::min<int64_t>(capacity, 100) / 100.0
                : full_wh > 0 && now_wh >= 0 ? now_wh / full_wh
                                             : 0;
    if (full_wh > 0 && design_wh > 0) out.health = std::min(full_wh / design_wh, 1.5);
    if (watts > 0) {
        const bool discharging = status == "Discharging";
        out.power_watts = discharging ? -watts : watts;
        if (discharging && now_wh > 0) out.minutes_to_empty = static_cast<int32_t>(now_wh / watts * 60);
        if (out.charging && full_wh > now_wh && now_wh >= 0)
            out.minutes_to_full = static_cast<int32_t>((full_wh - now_wh) / watts * 60);
    }
    const int64_t cycles = read_int(dir + "/cycle_count", -1);
    if (cycles > 0) out.cycle_count = static_cast<int32_t>(cycles);  // 0 means "not reported"
    const int64_t temp = read_int(dir + "/temp", -1);                // tenths of a degree, rarely present
    if (temp > 0 && temp < 1000) out.temperature = temp / 10.0;
    const std::string health = read_line(dir + "/health");
    if (!health.empty() && health != "Unknown") copy_string(out.condition, sizeof(out.condition), health);
    return true;
}

// busctl --json=short call … ListInhibitors: {"type":"a(ssssuu)","data":[[["what","who","why",
// "mode",uid,pid],…]]}. A minimal reader for that one shape. Exposed for tests.
std::vector<PowerAssertion> parse_inhibitors(const std::string &json) {
    std::vector<PowerAssertion> result;
    const auto data = json.find("\"data\"");
    if (data == std::string::npos) return result;
    size_t i = json.find('[', data);
    if (i == std::string::npos) return result;
    ++i;  // inside the outer array: the method's single return value
    i = json.find('[', i);
    if (i == std::string::npos) return result;
    ++i;  // inside the array of structs
    for (;;) {
        while (i < json.size() && (json[i] == ' ' || json[i] == ',')) ++i;
        if (i >= json.size() || json[i] != '[') break;
        ++i;
        std::vector<std::string> fields;
        while (i < json.size() && json[i] != ']') {
            if (json[i] == '"') {
                std::string value;
                for (++i; i < json.size() && json[i] != '"'; ++i) {
                    if (json[i] == '\\' && i + 1 < json.size()) {
                        ++i;
                        if (json[i] == 'u' && i + 4 < json.size()) {
                            const long code = std::strtol(json.substr(i + 1, 4).c_str(), nullptr, 16);
                            if (code < 0x80) value += static_cast<char>(code);
                            i += 4;
                            continue;
                        }
                        value += json[i] == 'n' ? ' ' : json[i];
                    } else {
                        value += json[i];
                    }
                }
                ++i;
                fields.push_back(value);
            } else if (json[i] == '-' || std::isdigit(static_cast<unsigned char>(json[i]))) {
                const size_t start = i;
                while (i < json.size() && (json[i] == '-' || std::isdigit(static_cast<unsigned char>(json[i])))) ++i;
                fields.push_back(json.substr(start, i - start));
            } else {
                ++i;
            }
        }
        ++i;  // past ']'
        if (fields.size() < 6) continue;
        const std::string &what = fields[0], &who = fields[1], &why = fields[2], &mode = fields[3];
        // "delay" locks only postpone a suspend for a moment (to lock the screen first).
        if (mode != "block") continue;
        PowerAssertion a;
        for (const auto &kind : split(what, ':')) {
            if (kind == "sleep") a.kind |= PC_ASSERT_SYSTEM_SLEEP;
            if (kind == "idle") a.kind |= PC_ASSERT_DISPLAY_SLEEP;
        }
        if (a.kind == 0) continue;  // shutdown, lid switch and power-key handling
        a.pid = static_cast<int32_t>(std::strtol(fields[5].c_str(), nullptr, 10));
        a.type = what;
        a.reason = why.empty() ? who : who.empty() || why.find(who) != std::string::npos ? why : who + " · " + why;
        result.push_back(a);
    }
    return result;
}

std::vector<std::string> busctl_values(const std::string &json) {
    // busctl --json=short wraps a reply as {"type":"…","data":[…]}: every string and number inside
    // "data", in order, whatever the nesting ("ao" → paths, "s" → one string, "u" → one number).
    std::vector<std::string> values;
    const auto data = json.find("\"data\"");
    if (data == std::string::npos) return values;
    for (size_t i = json.find('[', data); i != std::string::npos && i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            std::string value;
            for (++i; i < json.size() && json[i] != '"'; ++i) {
                if (json[i] == '\\' && i + 1 < json.size()) {
                    ++i;
                    if (json[i] == 'u' && i + 4 < json.size()) {
                        const long code = std::strtol(json.substr(i + 1, 4).c_str(), nullptr, 16);
                        if (code < 0x80) value += static_cast<char>(code);
                        i += 4;
                        continue;
                    }
                    value += json[i] == 'n' ? ' ' : json[i];
                } else {
                    value += json[i];
                }
            }
            values.push_back(value);
        } else if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            const size_t start = i;
            while (i + 1 < json.size() && (json[i + 1] == '.' || std::isdigit(static_cast<unsigned char>(json[i + 1]))))
                ++i;
            values.push_back(json.substr(start, i - start + 1));
        }
    }
    return values;
}

namespace {

const std::string &busctl() {
    static const std::string path = find_program("busctl");
    return path;
}

// One method call on the user's session bus, its reply's values; empty when it failed.
std::vector<std::string> session_call(const std::string &object, const std::string &interface,
                                      const std::string &method) {
    std::string out;
    if (run({busctl(), "--user", "--json=short", "call", "org.gnome.SessionManager", object, interface, method}, out,
            nullptr, 2000) != 0)
        return {};
    return busctl_values(out);
}

// The program a desktop entry runs ("flatpak run --command=x id" runs x), "" when there is no entry.
std::string desktop_program(const std::string &app_id) {
    std::vector<std::string> dirs;
    const char *home = std::getenv("HOME");
    if (home) dirs.push_back(std::string(home) + "/.local/share/applications");
    const char *data_dirs = std::getenv("XDG_DATA_DIRS");
    for (const auto &d : split(data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share", ':'))
        if (!d.empty()) dirs.push_back(d + "/applications");
    dirs.push_back("/var/lib/flatpak/exports/share/applications");
    dirs.push_back("/var/lib/snapd/desktop/applications");
    for (const auto &dir : dirs) {
        std::string text;
        if (!read_file(dir + "/" + app_id + ".desktop", text)) continue;
        const DesktopEntry entry = parse_desktop_entry(text);
        std::string program;
        for (const auto &arg : split_whitespace(entry.exec)) {
            if (starts_with(arg, "--command=")) return basename_of(arg.substr(10));
            if (program.empty() && basename_of(arg) != "env" && arg.find('=') == std::string::npos)
                program = basename_of(arg);
        }
        if (program != "flatpak") return program;
    }
    return {};
}

// The running process of this user that an app id stands for: its desktop entry's program, else a
// process named like the id ("firefox"). 0 when none runs.
int32_t app_pid(const std::string &app_id) {
    if (app_id.empty()) return 0;
    std::string program = desktop_program(app_id);
    if (program.empty()) program = basename_of(app_id);
    const std::string comm = program.substr(0, 15);
    const uid_t me = ::getuid();
    int32_t found = 0;
    for (int32_t pid : list_pids()) {
        struct stat info{};
        if (::stat(("/proc/" + std::to_string(pid)).c_str(), &info) != 0 || info.st_uid != me) continue;
        const std::string exe = basename_of(read_link("/proc/" + std::to_string(pid) + "/exe"));
        ProcStat stat;
        if (exe == program || (read_stat(pid, stat) && stat.comm == comm)) {
            // The app's first process: a browser's helpers are its children.
            if (found == 0 || pid < found) found = pid;
        }
    }
    return found;
}

// What GNOME's session manager blocks: apps that ask through the Inhibit portal or
// org.freedesktop.ScreenSaver (video players, browsers playing video) land here, not in logind.
std::vector<PowerAssertion> session_inhibitors() {
    std::vector<PowerAssertion> result;
    for (const auto &path : session_call("/org/gnome/SessionManager", "org.gnome.SessionManager", "GetInhibitors")) {
        if (!starts_with(path, "/org/gnome/SessionManager/")) continue;
        const char *interface = "org.gnome.SessionManager.Inhibitor";
        const auto flags = session_call(path, interface, "GetFlags");
        const uint32_t bits = flags.empty() ? 0 : static_cast<uint32_t>(std::strtoul(flags[0].c_str(), nullptr, 10));
        PowerAssertion a;
        // 4: suspend, 8: idle (the screen stays on). Logout and user-switch locks keep nothing awake.
        if (bits & 4) a.kind |= PC_ASSERT_SYSTEM_SLEEP;
        if (bits & 8) a.kind |= PC_ASSERT_DISPLAY_SLEEP;
        if (a.kind == 0) continue;
        const auto app = session_call(path, interface, "GetAppId");
        const auto reason = session_call(path, interface, "GetReason");
        const std::string app_id = app.empty() ? "" : app[0];
        // A registered session client knows its pid; portal and D-Bus callers usually aren't one.
        const auto client = session_call(path, interface, "GetClientId");
        if (!client.empty() && starts_with(client[0], "/org/gnome/SessionManager/Client")) {
            const auto pid = session_call(client[0], "org.gnome.SessionManager.Client", "GetUnixProcessId");
            if (!pid.empty()) a.pid = static_cast<int32_t>(std::strtol(pid[0].c_str(), nullptr, 10));
        }
        if (a.pid <= 0) a.pid = app_pid(app_id);
        a.type = bits & 4 ? (bits & 8 ? "GNOME: suspend+idle" : "GNOME: suspend") : "GNOME: idle";
        const std::string why = reason.empty() ? "" : reason[0];
        a.reason = a.pid > 0 || app_id.empty() ? why : app_id + (why.empty() ? "" : " · " + why);
        result.push_back(a);
    }
    return result;
}

}  // namespace

std::vector<PowerAssertion> power_assertions() {
    if (busctl().empty()) return {};
    std::vector<PowerAssertion> result;
    std::string out;
    if (run({busctl(), "--system", "--json=short", "call", "org.freedesktop.login1", "/org/freedesktop/login1",
             "org.freedesktop.login1.Manager", "ListInhibitors"},
            out, nullptr, 3000) == 0)
        result = parse_inhibitors(out);
    for (auto &a : session_inhibitors()) result.push_back(std::move(a));
    return result;
}

}  // namespace procyon::platform

#endif  // __linux__
