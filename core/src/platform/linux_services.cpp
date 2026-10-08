// Services are systemd units (system and user managers, through systemctl); startup items are the
// XDG autostart entries the desktop session launches at login (~/.config/autostart, /etc/xdg/autostart).
#if defined(__linux__)

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>

#include "linux_internal.hpp"

namespace procyon::platform {

using namespace linux_internal;

namespace {

const char *const kAutostartPrefix = "xdg:";

const std::string &systemctl() {
    static const std::string path = find_program("systemctl");
    return path;
}

std::vector<std::string> systemctl_args(int32_t domain, std::initializer_list<std::string> args) {
    std::vector<std::string> argv{systemctl()};
    if (domain == PC_DOMAIN_USER) argv.emplace_back("--user");
    argv.insert(argv.end(), args);
    return argv;
}

// Program of an ExecStart value: "{ path=/usr/bin/foo ; argv[]=/usr/bin/foo -x ; … }".
std::string exec_program(const std::string &exec) {
    const auto at = exec.find("path=");
    if (at == std::string::npos) return {};
    const auto end = exec.find(" ;", at);
    return trim(exec.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5));
}

bool unit_enabled(const std::string &state) {
    return state != "disabled" && state != "masked" && state != "masked-runtime" && state != "bad" && !state.empty();
}

// Units the distribution ships (/usr/lib/systemd, /lib/systemd) are part of the OS; units in /etc
// and the user's ~/.config were added by an administrator, a package's postinst or the user.
bool os_unit(const std::string &fragment) {
    return starts_with(fragment, "/usr/lib/systemd/") || starts_with(fragment, "/lib/systemd/") ||
           starts_with(fragment, "/usr/share/systemd/") || starts_with(fragment, "/run/systemd/generator");
}

void add_services(std::vector<pc_service> &out, int32_t domain) {
    std::string text;
    std::set<std::string> names;
    // Installed unit files (also the stopped and disabled ones) and every loaded unit (instances
    // of templates such as getty@tty1.service, transient units).
    if (run(systemctl_args(domain, {"list-unit-files", "--type=service", "--no-legend", "--no-pager", "--plain"}),
            text) == 0)
        for (const auto &line : split(text, '\n')) {
            const auto f = split_whitespace(line);
            if (!f.empty() && f[0].find("@.") == std::string::npos) names.insert(f[0]);
        }
    if (run(systemctl_args(domain, {"list-units", "--all", "--type=service", "--no-legend", "--no-pager", "--plain"}),
            text) == 0)
        for (const auto &line : split(text, '\n')) {
            const auto f = split_whitespace(line);
            if (!f.empty() && f[0].size() > 8 && f[0].compare(f[0].size() - 8, 8, ".service") == 0) names.insert(f[0]);
        }
    if (names.empty()) return;

    std::vector<std::string> argv = systemctl_args(
        domain,
        {"show", "--no-pager",
         "--property=Id,Description,MainPID,ExecMainStatus,FragmentPath,ExecStart,UnitFileState,LoadState", "--"});
    argv.insert(argv.end(), names.begin(), names.end());
    if (run(argv, text, nullptr, 20000) != 0 && text.empty()) return;
    // An alias (dbus-org.freedesktop.Avahi.service) shows as the unit it names: list each unit once.
    std::set<std::string> seen;
    for (const auto &unit : parse_systemctl_show(text)) {
        auto get = [&](const char *key) {
            auto it = unit.find(key);
            return it == unit.end() ? std::string() : it->second;
        };
        if (get("LoadState") != "loaded" || get("Id").empty() || !seen.insert(get("Id")).second) continue;
        pc_service s{};
        copy_string(s.label, sizeof(s.label), get("Id"));
        const std::string description = get("Description");
        copy_string(s.name, sizeof(s.name), description.empty() ? get("Id") : description);
        copy_string(s.program, sizeof(s.program), exec_program(get("ExecStart")));
        copy_string(s.config_path, sizeof(s.config_path), get("FragmentPath"));
        s.domain = domain;
        s.pid = static_cast<int32_t>(std::strtol(get("MainPID").c_str(), nullptr, 10));
        s.last_exit = static_cast<int32_t>(std::strtol(get("ExecMainStatus").c_str(), nullptr, 10));
        const std::string state = get("UnitFileState");
        s.enabled = state.empty() ? s.pid > 0 : unit_enabled(state);
        s.apple = os_unit(get("FragmentPath"));
        out.push_back(s);
    }
}

pc_result systemctl_result(int status, const std::string &err) {
    if (status == 0) return PC_OK;
    if (err.find("Access denied") != std::string::npos || err.find("authentication") != std::string::npos ||
        err.find("Authorization") != std::string::npos || err.find("Permission denied") != std::string::npos)
        return PC_ERR_PERMISSION;
    if (err.find("not found") != std::string::npos || err.find("not loaded") != std::string::npos ||
        err.find("does not exist") != std::string::npos)
        return PC_ERR_NOT_FOUND;
    return PC_ERR_FAILED;
}

// ---- XDG autostart ----

std::string home_directory() {
    const char *home = std::getenv("HOME");
    return home ? home : "";
}

std::string user_autostart_dir() {
    const char *config = std::getenv("XDG_CONFIG_HOME");
    return (config && *config ? std::string(config) : home_directory() + "/.config") + "/autostart";
}

std::vector<std::string> system_autostart_dirs() {
    const char *dirs = std::getenv("XDG_CONFIG_DIRS");
    std::vector<std::string> result;
    for (const auto &d : split(dirs && *dirs ? dirs : "/etc/xdg", ':'))
        if (!d.empty()) result.push_back(d + "/autostart");
    return result;
}

bool desktop_matches(const std::string &list) {
    if (list.empty()) return false;
    const char *current = std::getenv("XDG_CURRENT_DESKTOP");
    if (!current) return false;
    for (const auto &desktop : split(current, ':'))
        for (const auto &entry : split(list, ';'))
            if (!entry.empty() && entry == desktop) return true;
    return false;
}

std::string program_of(const std::string &exec) {
    for (const auto &arg : split_whitespace(exec)) {
        if (basename_of(arg) == "env" || arg.find('=') != std::string::npos) continue;
        if (arg.size() >= 2 && arg.front() == '"') return arg.substr(1, arg.size() - 2);
        return arg;
    }
    return {};
}

// Rewrites the [Desktop Entry] keys of a desktop file's text, adding the ones it lacks.
std::string set_desktop_keys(const std::string &text, const std::map<std::string, std::string> &keys) {
    std::string out;
    std::set<std::string> written;
    bool in_entry = false, entry_seen = false;
    auto flush = [&] {
        for (const auto &[key, value] : keys)
            if (!written.count(key)) out += key + "=" + value + "\n";
        written.clear();
        for (const auto &[key, value] : keys) written.insert(key);
    };
    for (const auto &line : split(text, '\n')) {
        const std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed[0] == '[') {
            if (in_entry) flush();
            in_entry = trimmed == "[Desktop Entry]";
            entry_seen |= in_entry;
        } else if (in_entry) {
            const auto eq = trimmed.find('=');
            const std::string key = eq == std::string::npos ? "" : trim(trimmed.substr(0, eq));
            auto found = keys.find(key);
            if (found != keys.end()) {
                out += key + "=" + found->second + "\n";
                written.insert(key);
                continue;
            }
        }
        out += line + "\n";
    }
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
    if (in_entry) flush();
    if (!entry_seen) {
        out = "[Desktop Entry]\n";
        for (const auto &[key, value] : keys) out += key + "=" + value + "\n";
    }
    return out;
}

bool write_atomically(const std::string &path, const std::string &text) {
    const std::string temp = path + ".procyon-tmp";
    FILE *file = std::fopen(temp.c_str(), "w");
    if (!file) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fclose(file) != 0 || !written || std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

// Turns an autostart entry on or off the way GNOME's and KDE's settings do: a file of the same
// name in ~/.config/autostart overrides the system one, and Hidden=true there removes it.
pc_result set_autostart(const std::string &file_id, bool enabled) {
    if (file_id.find('/') != std::string::npos || file_id.empty() || file_id[0] == '.') return PC_ERR_INVALID;
    const std::string user_path = user_autostart_dir() + "/" + file_id;
    std::string text;
    if (!read_file(user_path, text)) {
        bool found = false;
        for (const auto &dir : system_autostart_dirs())
            if (read_file(dir + "/" + file_id, text)) {
                found = true;
                break;
            }
        if (!found) return PC_ERR_NOT_FOUND;
        if (enabled) return PC_OK;  // only the system entry exists, and it isn't hidden by us
        ::mkdir((home_directory() + "/.config").c_str(), 0700);
        ::mkdir(user_autostart_dir().c_str(), 0700);
    }
    std::map<std::string, std::string> keys{{"Hidden", enabled ? "false" : "true"}};
    if (enabled && text.find("X-GNOME-Autostart-enabled=false") != std::string::npos)
        keys["X-GNOME-Autostart-enabled"] = "true";
    return write_atomically(user_path, set_desktop_keys(text, keys)) ? PC_OK : PC_ERR_PERMISSION;
}

}  // namespace

std::vector<std::map<std::string, std::string>> parse_systemctl_show(const std::string &text) {
    std::vector<std::map<std::string, std::string>> units;
    std::map<std::string, std::string> current;
    for (const auto &line : split(text, '\n')) {
        if (line.empty()) {
            if (!current.empty()) units.push_back(std::move(current));
            current.clear();
            continue;
        }
        const auto eq = line.find('=');
        if (eq != std::string::npos) current[line.substr(0, eq)] = line.substr(eq + 1);
    }
    if (!current.empty()) units.push_back(std::move(current));
    return units;
}

DesktopEntry parse_desktop_entry(const std::string &text) {
    DesktopEntry entry;
    bool in_entry = false;
    for (const auto &raw : split(text, '\n')) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            in_entry = line == "[Desktop Entry]";
            continue;
        }
        const auto eq = line.find('=');
        if (!in_entry || eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "Name")
            entry.name = value;
        else if (key == "Exec")
            entry.exec = value;
        else if (key == "Type")
            entry.type = value;
        else if (key == "Hidden")
            entry.hidden = value == "true";
        else if (key == "NoDisplay")
            entry.no_display = value == "true";
        else if (key == "X-GNOME-Autostart-enabled")
            entry.autostart_enabled = value != "false";
        else if (key == "OnlyShowIn")
            entry.only_show_in = value;
        else if (key == "NotShowIn")
            entry.not_show_in = value;
        else if (key == "X-GNOME-Autostart-Phase")
            entry.session_phase = !value.empty();
    }
    return entry;
}

bool valid_service_label(const std::string &label) {
    if (label.empty() || label.size() >= 256 || label[0] == '-') return false;
    // Unit names: ASCII letters, digits, ":-_.\" and "@" for instances; "xdg:<file>.desktop" for
    // autostart entries. Nothing reaches a shell, but a label is passed as one argument after "--".
    for (char c : label)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_' && c != '@' && c != ':' &&
            c != '\\')
            return false;
    return true;
}

std::vector<pc_service> services() {
    std::vector<pc_service> result;
    if (systemctl().empty()) return result;
    add_services(result, PC_DOMAIN_SYSTEM);
    add_services(result, PC_DOMAIN_USER);
    return result;
}

pc_result service_control(int32_t domain, const std::string &label, int32_t action) {
    if (!valid_service_label(label) || (domain != PC_DOMAIN_SYSTEM && domain != PC_DOMAIN_USER)) return PC_ERR_INVALID;
    if (starts_with(label, kAutostartPrefix)) {
        if (domain != PC_DOMAIN_USER) return PC_ERR_INVALID;
        if (action == PC_SERVICE_ENABLE || action == PC_SERVICE_DISABLE)
            return set_autostart(label.substr(4), action == PC_SERVICE_ENABLE);
        return PC_ERR_UNSUPPORTED;
    }
    if (systemctl().empty()) return PC_ERR_UNSUPPORTED;
    const char *verb = nullptr;
    switch (action) {
        case PC_SERVICE_START: verb = "start"; break;
        case PC_SERVICE_STOP: verb = "stop"; break;
        case PC_SERVICE_RESTART: verb = "restart"; break;
        case PC_SERVICE_ENABLE: verb = "enable"; break;
        case PC_SERVICE_DISABLE: verb = "disable"; break;
        default: return PC_ERR_INVALID;
    }
    // A unit that doesn't exist is answered without asking polkit for a password first.
    std::string out, err;
    if (run(systemctl_args(domain, {"show", "--property=LoadState", "--value", "--", label}), out) != 0)
        return PC_ERR_FAILED;
    if (trim(out) == "not-found") return PC_ERR_NOT_FOUND;
    // System units: systemctl asks for authorization through polkit (the desktop's password
    // dialog) unless Procyon runs as root.
    const int status = run(systemctl_args(domain, {verb, "--", label}), out, &err, 120000);
    return systemctl_result(status, err);
}

std::vector<pc_startup_item> startup_items() {
    std::vector<pc_startup_item> result;
    // File id → entry; a user file overrides the system one of the same name.
    struct Found {
        std::string path;
        DesktopEntry entry;
        int32_t scope;
    };
    std::map<std::string, Found> entries;
    const auto system_dirs = system_autostart_dirs();
    for (auto dir = system_dirs.rbegin(); dir != system_dirs.rend(); ++dir)
        for (const auto &file : list_dir(*dir)) {
            std::string text;
            if (file.size() > 8 && file.compare(file.size() - 8, 8, ".desktop") == 0 &&
                read_file(*dir + "/" + file, text))
                entries[file] = {*dir + "/" + file, parse_desktop_entry(text), PC_STARTUP_GLOBAL_AGENT};
        }
    const std::string user_dir = user_autostart_dir();
    for (const auto &file : list_dir(user_dir)) {
        std::string text;
        if (file.size() <= 8 || file.compare(file.size() - 8, 8, ".desktop") != 0 ||
            !read_file(user_dir + "/" + file, text))
            continue;
        DesktopEntry entry = parse_desktop_entry(text);
        auto system = entries.find(file);
        if (system != entries.end()) {
            // An override keeps the system entry's identity; it only switches it on or off.
            DesktopEntry merged = system->second.entry;
            merged.hidden = entry.hidden;
            merged.autostart_enabled = entry.autostart_enabled;
            if (!entry.exec.empty()) merged.exec = entry.exec;
            system->second.entry = merged;
            system->second.path = user_dir + "/" + file;
        } else {
            entries[file] = {user_dir + "/" + file, entry, PC_STARTUP_USER_AGENT};
        }
    }

    // Running instances: processes of this user whose program matches the entry's.
    std::unordered_map<std::string, int32_t> running;
    const uid_t me = ::getuid();
    for (int32_t pid : list_pids()) {
        struct stat info{};
        if (::stat(("/proc/" + std::to_string(pid)).c_str(), &info) != 0 || info.st_uid != me) continue;
        const std::string exe = basename_of(read_link("/proc/" + std::to_string(pid) + "/exe"));
        if (!exe.empty() && !running.count(exe)) running[exe] = pid;
    }

    for (const auto &[file, found] : entries) {
        const DesktopEntry &e = found.entry;
        if (found.scope == PC_STARTUP_GLOBAL_AGENT && found.path.rfind(user_dir, 0) != 0 && e.hidden) continue;
        // Session components (gnome-keyring, settings daemons) start in a session phase, and
        // entries for other desktops never run here: neither is something to switch.
        if (e.session_phase || (!e.only_show_in.empty() && !desktop_matches(e.only_show_in)) ||
            desktop_matches(e.not_show_in) || (e.no_display && found.scope == PC_STARTUP_GLOBAL_AGENT))
            continue;
        if (!e.type.empty() && e.type != "Application") continue;
        pc_startup_item item{};
        copy_string(item.label, sizeof(item.label), kAutostartPrefix + file);
        copy_string(item.name, sizeof(item.name), e.name.empty() ? file.substr(0, file.size() - 8) : e.name);
        std::string program = program_of(e.exec);
        if (!program.empty() && program[0] != '/') {
            const std::string resolved = find_program(program);
            if (!resolved.empty()) program = resolved;
        }
        copy_string(item.program, sizeof(item.program), program);
        copy_string(item.config_path, sizeof(item.config_path), found.path);
        item.scope = found.scope;
        auto pid = running.find(basename_of(program));
        item.pid = pid == running.end() ? 0 : pid->second;
        item.enabled = !e.hidden && e.autostart_enabled;
        item.run_at_load = true;
        result.push_back(item);
    }
    return result;
}

// macOS's Open at Login and background items have no Linux counterpart beyond autostart.
std::vector<pc_startup_item> managed_startup_items(uint32_t) { return {}; }

}  // namespace procyon::platform

#endif  // __linux__
