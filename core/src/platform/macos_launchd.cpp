// macOS services and startup items: launchd jobs. Loaded jobs and their state come from
// `launchctl print`, definitions from the LaunchDaemons/LaunchAgents property lists. launchd has
// no public API for this, so the adapter drives /bin/launchctl with fixed arguments.
// Items apps register through ServiceManagement (Open at Login, "Allow in the Background") live in
// the Background Task Management database, read with `sfltool dumpbtm` like System Settings shows it.
#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../platform.hpp"

extern char **environ;

namespace procyon::platform {
namespace {

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

std::string basename_of(const std::string &path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Runs `program` with `args`; returns its exit status (-1 when it couldn't run or was killed after
// `timeout_seconds`) and its stdout.
int run(std::string program, const std::vector<std::string> &args, std::string *output, int timeout_seconds = 15) {
    std::vector<char *> argv;
    argv.push_back(program.data());
    for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return -1;
    // Close-on-exec on both ends: the helper runs several of these at once on different threads,
    // and a child spawned by another thread must not inherit this pipe's write end, or our read
    // would wait for its EOF too. dup2 onto stdout clears the flag on the child's copy alone.
    fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    pid_t child = 0;
    const int spawned = posix_spawn(&child, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    if (spawned != 0) {
        close(pipe_fds[0]);
        return -1;
    }
    char buffer[16384];
    // A tool stuck on its daemon must not hold the caller forever (or queue up behind itself).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool timed_out = false;
    while (true) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd readable = {pipe_fds[0], POLLIN, 0};
        const int ready = left.count() > 0 ? poll(&readable, 1, static_cast<int>(left.count())) : 0;
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            timed_out = ready == 0;
            break;
        }
        // Callers may hold a lock on purpose: BTM dumps must not overlap.
        const ssize_t count =
            read(pipe_fds[0], buffer, sizeof(buffer));  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        if (output) output->append(buffer, static_cast<size_t>(count));
    }
    close(pipe_fds[0]);
    if (timed_out) kill(child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return !timed_out && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int launchctl(const std::vector<std::string> &args, std::string *output = nullptr) {
    return run("/bin/launchctl", args, output);
}

std::string gui_domain(uid_t uid = getuid()) { return "gui/" + std::to_string(uid); }
std::string domain_name(int32_t domain) { return domain == PC_DOMAIN_SYSTEM ? "system" : gui_domain(); }

LoadedServices loaded_services(const std::string &domain) {
    std::string text;
    if (launchctl({"print", domain}, &text) != 0) return {};
    return parse_launchctl_print(text);
}

std::unordered_map<std::string, bool> disabled_services(const std::string &domain) {
    std::string text;
    if (launchctl({"print-disabled", domain}, &text) != 0) return {};
    return parse_launchctl_disabled(text);
}

struct Job {
    std::string label;
    std::string program;
    std::string path;  // the plist
    std::string app_path;
    bool disabled = false;
    bool run_at_load = false;
    bool keep_alive = false;
};

std::string cf_string(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    char buffer[2048];
    return CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer), kCFStringEncodingUTF8)
               ? buffer
               : std::string();
}

bool cf_true(CFTypeRef value) {
    if (!value) return false;
    if (CFGetTypeID(value) == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    return CFGetTypeID(value) == CFDictionaryGetTypeID();  // KeepAlive conditions
}

std::string bundle_of(const std::string &path) {
    auto pos = path.find(".app/");
    return pos == std::string::npos ? std::string() : path.substr(0, pos + 4);
}

// The app that installed the job, from its AssociatedBundleIdentifiers.
std::string app_for_bundle_id(CFTypeRef ids) {
    CFStringRef id = nullptr;
    if (ids && CFGetTypeID(ids) == CFStringGetTypeID()) id = static_cast<CFStringRef>(ids);
    if (ids && CFGetTypeID(ids) == CFArrayGetTypeID() && CFArrayGetCount(static_cast<CFArrayRef>(ids)) > 0) {
        auto first = CFArrayGetValueAtIndex(static_cast<CFArrayRef>(ids), 0);
        if (CFGetTypeID(first) == CFStringGetTypeID()) id = static_cast<CFStringRef>(first);
    }
    if (!id) return {};
    std::string path;
    if (CFArrayRef urls = LSCopyApplicationURLsForBundleIdentifier(id, nullptr)) {
        if (CFArrayGetCount(urls) > 0) {
            auto url = static_cast<CFURLRef>(CFArrayGetValueAtIndex(urls, 0));
            char buffer[1024];
            if (CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8 *>(buffer), sizeof(buffer)))
                path = buffer;
        }
        CFRelease(urls);
    }
    return path;
}

bool read_job(const std::string &path, Job &job) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8 *>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()), false);
    if (!url) return false;
    CFReadStreamRef stream = CFReadStreamCreateWithFile(nullptr, url);
    CFRelease(url);
    if (!stream) return false;
    CFPropertyListRef plist = nullptr;
    if (CFReadStreamOpen(stream)) {
        plist = CFPropertyListCreateWithStream(nullptr, stream, 0, kCFPropertyListImmutable, nullptr, nullptr);
        CFReadStreamClose(stream);
    }
    CFRelease(stream);
    if (!plist) return false;
    bool ok = false;
    if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        auto dict = static_cast<CFDictionaryRef>(plist);
        job.label = cf_string(CFDictionaryGetValue(dict, CFSTR("Label")));
        job.path = path;
        job.program = cf_string(CFDictionaryGetValue(dict, CFSTR("Program")));
        auto args = CFDictionaryGetValue(dict, CFSTR("ProgramArguments"));
        if (job.program.empty() && args && CFGetTypeID(args) == CFArrayGetTypeID() &&
            CFArrayGetCount(static_cast<CFArrayRef>(args)) > 0)
            job.program = cf_string(CFArrayGetValueAtIndex(static_cast<CFArrayRef>(args), 0));
        if (job.program.empty())
            job.program = cf_string(CFDictionaryGetValue(dict, CFSTR("BundleProgram")));  // relative to the app
        job.disabled = cf_true(CFDictionaryGetValue(dict, CFSTR("Disabled")));
        job.run_at_load = cf_true(CFDictionaryGetValue(dict, CFSTR("RunAtLoad")));
        job.keep_alive = cf_true(CFDictionaryGetValue(dict, CFSTR("KeepAlive")));
        job.app_path = bundle_of(job.program);
        if (job.app_path.empty())
            job.app_path = app_for_bundle_id(CFDictionaryGetValue(dict, CFSTR("AssociatedBundleIdentifiers")));
        ok = !job.label.empty();
    }
    CFRelease(plist);
    return ok;
}

// `uid`'s home: the helper runs as root but reads on behalf of the app's user.
std::string home_directory(uid_t uid = getuid()) {
    if (const char *home = getenv("HOME"); home && uid == getuid()) return home;
    passwd *pw = getpwuid(uid);
    return pw ? pw->pw_dir : "";
}

// What a directory of plists looked like: its own mtime (entries added or removed), plus the
// count and newest mtime of the plists inside (a plist edited in place leaves the directory's
// mtime alone).
struct DirectoryStamp {
    timespec directory{};
    timespec newest_plist{};
    size_t plist_count = 0;

    bool operator==(const DirectoryStamp &other) const {
        return directory.tv_sec == other.directory.tv_sec && directory.tv_nsec == other.directory.tv_nsec &&
               newest_plist.tv_sec == other.newest_plist.tv_sec && newest_plist.tv_nsec == other.newest_plist.tv_nsec &&
               plist_count == other.plist_count;
    }
};

bool newer(const timespec &a, const timespec &b) {
    return a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec > b.tv_nsec);
}

// Lists the plists of `directory` and stamps them; false when the directory cannot be read.
bool scan_plists(const std::string &directory, std::vector<std::string> &paths, DirectoryStamp &stamp) {
    paths.clear();
    stamp = {};
    struct stat info{};
    if (stat(directory.c_str(), &info) != 0) return false;
    stamp.directory = info.st_mtimespec;
    DIR *dir = opendir(directory.c_str());
    if (!dir) return false;
    while (dirent *item = readdir(dir)) {
        const std::string name = item->d_name;
        if (name.size() < 7 || name.compare(name.size() - 6, 6, ".plist") != 0) continue;
        std::string path = directory;
        path += '/';
        path += name;
        struct stat plist{};
        if (stat(path.c_str(), &plist) != 0) continue;
        if (newer(plist.st_mtimespec, stamp.newest_plist)) stamp.newest_plist = plist.st_mtimespec;
        ++stamp.plist_count;
        paths.push_back(std::move(path));
    }
    closedir(dir);
    return true;
}

// Parsed plists per directory, re-read only when the directory or a plist in it changes. A
// directory that cannot be read right now is not cached: the next call tries again.
std::vector<Job> jobs_in(const std::string &directory) {
    struct Entry {
        DirectoryStamp stamp;
        std::vector<Job> jobs;
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, Entry> cache;

    std::vector<std::string> paths;
    DirectoryStamp stamp;
    if (!scan_plists(directory, paths, stamp)) return {};
    std::lock_guard lock(mutex);
    auto &entry = cache[directory];
    if (entry.stamp == stamp) return entry.jobs;
    entry.jobs.clear();
    entry.stamp = stamp;
    for (const auto &path : paths) {
        Job job;
        if (read_job(path, job)) entry.jobs.push_back(std::move(job));
    }
    return entry.jobs;
}

std::string display_name(const Job *job, const std::string &label) {
    if (job && !job->app_path.empty()) {
        std::string name = basename_of(job->app_path);
        return name.size() > 4 ? name.substr(0, name.size() - 4) : name;
    }
    if (job && !job->program.empty()) return basename_of(job->program);
    return label;
}

bool is_apple(const std::string &label, const std::string &path) {
    return label.rfind("com.apple.", 0) == 0 || path.rfind("/System/", 0) == 0;
}

void add_services(std::vector<pc_service> &out, int32_t domain, const std::vector<std::string> &directories) {
    const std::string name = domain_name(domain);
    const auto loaded = loaded_services(name);
    const auto disabled = disabled_services(name);
    std::unordered_map<std::string, Job> jobs;
    for (const auto &dir : directories)
        for (auto &job : jobs_in(dir)) jobs.emplace(job.label, std::move(job));

    std::unordered_set<std::string> labels;
    for (const auto &[label, _] : loaded) {
        // App instances and XPC services of apps are listed as jobs too; they aren't services.
        if (label.rfind("application.", 0) == 0) continue;
        labels.insert(label);
    }
    // Disabled jobs aren't loaded; keep them so they can be enabled again.
    for (const auto &[label, _] : jobs)
        if (auto it = disabled.find(label); it != disabled.end() && it->second) labels.insert(label);

    for (const auto &label : labels) {
        const auto job = jobs.find(label);
        const Job *definition = job == jobs.end() ? nullptr : &job->second;
        pc_service service{};
        copy_string(service.label, sizeof(service.label), label);
        copy_string(service.name, sizeof(service.name), display_name(definition, label));
        if (definition) {
            copy_string(service.program, sizeof(service.program), definition->program);
            copy_string(service.config_path, sizeof(service.config_path), definition->path);
        }
        service.domain = domain;
        if (auto it = loaded.find(label); it != loaded.end()) {
            service.pid = it->second.pid;
            service.last_exit = it->second.last_exit;
        }
        const auto override_it = disabled.find(label);
        service.enabled = override_it != disabled.end() ? !override_it->second : !(definition && definition->disabled);
        service.apple = is_apple(label, definition ? definition->path : "");
        out.push_back(service);
    }
}

pc_result launchctl_result(int status) {
    switch (status) {
        case 0: return PC_OK;
        case 113: return PC_ERR_NOT_FOUND;  // "Could not find service"
        case 1: return PC_ERR_PERMISSION;   // "Operation not permitted"
        case 150: return PC_ERR_PROTECTED;  // protected by System Integrity Protection
        default: return PC_ERR_FAILED;
    }
}

}  // namespace

// The "services = { pid status label }" block of `launchctl print <domain>`.
LoadedServices parse_launchctl_print(const std::string &text) {
    LoadedServices result;
    std::istringstream lines(text);
    std::string line;
    bool inside = false;
    while (std::getline(lines, line)) {
        if (!inside) {
            inside = line.find("services = {") != std::string::npos;
            continue;
        }
        if (line.find('}') != std::string::npos) break;
        std::istringstream fields(line);
        std::string pid, status, label;
        if (!(fields >> pid >> status >> label)) continue;
        LoadedService entry;
        entry.pid = static_cast<int32_t>(std::strtol(pid.c_str(), nullptr, 10));
        entry.last_exit = static_cast<int32_t>(std::strtol(status.c_str(), nullptr, 10));  // "-" and "(pe)" read 0
        result[label] = entry;
    }
    return result;
}

// `launchctl print-disabled <domain>`: "label" => disabled|enabled (true|false on older systems).
std::unordered_map<std::string, bool> parse_launchctl_disabled(const std::string &text) {
    std::unordered_map<std::string, bool> result;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto open = line.find('"');
        const auto close = open == std::string::npos ? open : line.find('"', open + 1);
        const auto arrow = line.find("=>");
        if (close == std::string::npos || arrow == std::string::npos) continue;
        const std::string value = line.substr(arrow + 2);
        result[line.substr(open + 1, close - open - 1)] =
            value.find("disabled") != std::string::npos || value.find("true") != std::string::npos;
    }
    return result;
}

bool valid_service_label(const std::string &label) {
    if (label.empty() || label.size() >= 256 || label[0] == '-') return false;
    for (char c : label)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') return false;
    return true;
}

std::vector<pc_service> services() {
    std::vector<pc_service> result;
    add_services(result, PC_DOMAIN_SYSTEM, {"/System/Library/LaunchDaemons", "/Library/LaunchDaemons"});
    add_services(result, PC_DOMAIN_USER,
                 {"/System/Library/LaunchAgents", "/Library/LaunchAgents", home_directory() + "/Library/LaunchAgents"});
    return result;
}

std::vector<pc_startup_item> startup_items() {
    std::vector<pc_startup_item> result;
    const struct {
        int32_t scope;
        std::string directory;
        int32_t domain;
    } locations[] = {
        {PC_STARTUP_USER_AGENT, home_directory() + "/Library/LaunchAgents", PC_DOMAIN_USER},
        {PC_STARTUP_GLOBAL_AGENT, "/Library/LaunchAgents", PC_DOMAIN_USER},
        {PC_STARTUP_DAEMON, "/Library/LaunchDaemons", PC_DOMAIN_SYSTEM},
    };
    std::unordered_map<int32_t, LoadedServices> loaded;
    std::unordered_map<int32_t, std::unordered_map<std::string, bool>> disabled;
    for (const auto &location : locations) {
        const auto jobs = jobs_in(location.directory);
        if (jobs.empty()) continue;
        if (!loaded.count(location.domain)) {
            loaded[location.domain] = loaded_services(domain_name(location.domain));
            disabled[location.domain] = disabled_services(domain_name(location.domain));
        }
        for (const auto &job : jobs) {
            pc_startup_item item{};
            copy_string(item.label, sizeof(item.label), job.label);
            copy_string(item.name, sizeof(item.name), display_name(&job, job.label));
            copy_string(item.program, sizeof(item.program), job.program);
            copy_string(item.config_path, sizeof(item.config_path), job.path);
            copy_string(item.app_path, sizeof(item.app_path), job.app_path);
            item.scope = location.scope;
            item.run_at_load = job.run_at_load;
            item.keep_alive = job.keep_alive;
            const auto &overrides = disabled[location.domain];
            const auto override_it = overrides.find(job.label);
            item.enabled = override_it != overrides.end() ? !override_it->second : !job.disabled;
            const auto &running = loaded[location.domain];
            if (auto it = running.find(job.label); it != running.end()) item.pid = it->second.pid;
            result.push_back(item);
        }
    }
    return result;
}

pc_result service_control(int32_t domain, const std::string &label, int32_t action) {
    if (!valid_service_label(label) || (domain != PC_DOMAIN_SYSTEM && domain != PC_DOMAIN_USER)) return PC_ERR_INVALID;
    const std::string target = domain_name(domain) + "/" + label;
    switch (action) {
        case PC_SERVICE_START: return launchctl_result(launchctl({"kickstart", target}));
        case PC_SERVICE_RESTART: return launchctl_result(launchctl({"kickstart", "-k", target}));
        case PC_SERVICE_STOP: return launchctl_result(launchctl({"kill", "SIGTERM", target}));
        case PC_SERVICE_ENABLE: return launchctl_result(launchctl({"enable", target}));
        case PC_SERVICE_DISABLE: return launchctl_result(launchctl({"disable", target}));
        default: return PC_ERR_INVALID;
    }
}

namespace {

// One record of `sfltool dumpbtm`: "Key: value" lines under " #N:".
using BtmRecord = std::unordered_map<std::string, std::string>;

std::string trim(const std::string &text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Records of `user` plus the machine-wide ones (UID 0 and -2), in dump order.
std::vector<BtmRecord> btm_records(const std::string &dump, uid_t user) {
    const std::string own_uid = std::to_string(user);
    std::vector<BtmRecord> records;
    std::istringstream lines(dump);
    std::string line;
    bool wanted = false;
    BtmRecord *current = nullptr;
    while (std::getline(lines, line)) {
        if (line.find("Records for UID ") != std::string::npos) {
            std::istringstream fields(line.substr(line.find("UID ") + 4));
            std::string uid;
            fields >> uid;
            wanted = uid == own_uid || uid == "0" || uid == "-2";
            current = nullptr;
            continue;
        }
        if (!wanted) continue;
        // " #3:" starts a record; deeper "    #1: id" lines list embedded items.
        if (line.size() > 2 && line[0] == ' ' && line[1] == '#' && line.back() == ':') {
            current = &records.emplace_back();
            continue;
        }
        const auto colon = line.find(": ");
        if (!current || colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        if (!key.empty() && key[0] != '#') current->emplace(key, trim(line.substr(colon + 2)));
    }
    return records;
}

std::string field(const BtmRecord &record, const char *key) {
    auto it = record.find(key);
    return it == record.end() || it->second == "(null)" ? std::string() : it->second;
}

// "[enabled, allowed, notified] (0xb)" -> the bracketed words.
bool disposition_has(const BtmRecord &record, const std::string &word) {
    const std::string value = field(record, "Disposition");
    const auto open = value.find('['), close = value.find(']');
    if (open == std::string::npos || close == std::string::npos) return false;
    std::istringstream words(value.substr(open + 1, close - open - 1));
    std::string token;
    while (std::getline(words, token, ','))
        if (trim(token) == word) return true;
    return false;
}

// "login item (0x4)" -> 0x4.
uint64_t type_bits(const BtmRecord &record) {
    const std::string value = field(record, "Type");
    const auto open = value.rfind("(0x");
    return open == std::string::npos ? 0 : std::strtoull(value.c_str() + open + 1, nullptr, 16);
}

constexpr uint64_t kBtmApp = 0x2, kBtmLoginItem = 0x4, kBtmAgent = 0x8, kBtmDaemon = 0x10;
constexpr uint64_t kBtmBackgroundTasks = 0x2000, kBtmLegacy = 0x10000;

}  // namespace

std::vector<pc_startup_item> managed_startup_items(uint32_t user) {
    std::string dump;
    {
        // backgroundtaskmanagementd answers one dump at a time, in about three seconds.
        static std::mutex one_at_a_time;
        std::lock_guard lock(one_at_a_time);
        if (run("/usr/bin/sfltool", {"dumpbtm"}, &dump, 30) != 0 || dump.empty()) return {};
    }
    return parse_managed_startup_items(std::move(dump), user, loaded_services(gui_domain(user)),
                                       loaded_services("system"));
}

std::vector<pc_startup_item> parse_managed_startup_items(std::string dump, uint32_t user,
                                                         const LoadedServices &user_jobs,
                                                         const LoadedServices &system_jobs) {
    std::vector<pc_startup_item> result;
    // The dump writes home folders as "/Users/<uid>/…"; put the real home back.
    const std::string redacted = "/Users/" + std::to_string(user) + "/";
    const std::string home = home_directory(user) + "/";
    for (size_t at = dump.find(redacted); at != std::string::npos; at = dump.find(redacted, at + home.size()))
        dump.replace(at, redacted.size(), home);
    const auto records = btm_records(dump, user);

    std::unordered_map<std::string, const BtmRecord *> by_id;
    for (const auto &record : records) by_id.emplace(field(record, "Identifier"), &record);

    std::unordered_set<std::string> seen;
    for (const auto &record : records) {
        const uint64_t type = type_bits(record);
        const std::string identifier = field(record, "Identifier");
        // Legacy agents and daemons are plain launchd plists, listed (and switchable) by startup_items().
        if ((type & kBtmLegacy) || identifier.empty() || !seen.insert(identifier).second) continue;
        const bool open_at_login = type == kBtmApp && disposition_has(record, "enabled");
        const bool background =
            type == kBtmLoginItem || type == kBtmAgent || type == kBtmDaemon || type == kBtmBackgroundTasks;
        if (!open_at_login && !background) continue;

        const BtmRecord *parent = nullptr;
        if (auto it = by_id.find(field(record, "Parent Identifier")); it != by_id.end()) parent = it->second;
        const std::string parent_path = parent ? field(*parent, "URL") : std::string();
        std::string url = field(record, "URL");
        // Embedded items name their path inside the parent app ("Contents/Library/LoginItems/…").
        if (!url.empty() && url[0] != '/' && !parent_path.empty()) {
            std::string inside = parent_path;
            inside += '/';
            inside += url;
            url = std::move(inside);
        }

        pc_startup_item item{};
        // The launchd label: the identifier without its numeric type prefix ("4.com.example.helper").
        const auto dot = identifier.find('.');
        const std::string label = dot != std::string::npos && std::isdigit(static_cast<unsigned char>(identifier[0]))
                                      ? identifier.substr(dot + 1)
                                      : identifier;
        copy_string(item.label, sizeof(item.label), label);
        copy_string(item.name, sizeof(item.name), field(record, "Name"));
        copy_string(item.program, sizeof(item.program),
                    field(record, "Executable Path").empty() ? url : field(record, "Executable Path"));
        copy_string(item.config_path, sizeof(item.config_path), url.size() > 6 && url.ends_with(".plist") ? url : "");
        // The outermost app, for the icon: a helper inside 1Password.app shows 1Password's.
        const std::string app = !bundle_of(url).empty()         ? bundle_of(url)
                                : url.ends_with(".app")         ? url
                                : parent_path.ends_with(".app") ? parent_path
                                                                : std::string();
        copy_string(item.app_path, sizeof(item.app_path), app);
        if (parent) copy_string(item.parent_name, sizeof(item.parent_name), field(*parent, "Name"));
        item.scope = open_at_login ? PC_STARTUP_OPEN_AT_LOGIN : PC_STARTUP_APP_BACKGROUND;
        // Turned off in System Settings → "Allow in the Background" clears "allowed".
        item.enabled = disposition_has(record, "enabled") && disposition_has(record, "allowed");
        item.managed_by_os = true;
        const auto &jobs = type == kBtmDaemon ? system_jobs : user_jobs;
        if (auto it = jobs.find(label); it != jobs.end()) item.pid = it->second.pid;
        result.push_back(item);
    }
    return result;
}

}  // namespace procyon::platform

#endif  // __APPLE__
