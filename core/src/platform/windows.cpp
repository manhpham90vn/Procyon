// Windows adapter: NtQuerySystemInformation for the process table and CPU, the performance
// counters for memory, IOCTL_DISK_PERFORMANCE and the interface table for machine-wide I/O.
#if defined(_WIN32)

#include "windows_internal.hpp"

#include <winsock2.h>

#include <ws2ipdef.h>

#include <iphlpapi.h>
#include <netioapi.h>
#include <psapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")

namespace procyon::platform {

// ---------------------------------------------------------------------------------------------
// Shared helpers (windows_internal.hpp)
// ---------------------------------------------------------------------------------------------

namespace win {

const Ntdll &ntdll() {
    static const Ntdll table = [] {
        Ntdll t;
        HMODULE module = GetModuleHandleW(L"ntdll.dll");
        if (!module) return t;
        t.query_system = reinterpret_cast<NtQuerySystemInformationFn>(
            reinterpret_cast<void *>(GetProcAddress(module, "NtQuerySystemInformation")));
        t.query_process = reinterpret_cast<NtQueryInformationProcessFn>(
            reinterpret_cast<void *>(GetProcAddress(module, "NtQueryInformationProcess")));
        t.query_object =
            reinterpret_cast<NtQueryObjectFn>(reinterpret_cast<void *>(GetProcAddress(module, "NtQueryObject")));
        t.suspend_process = reinterpret_cast<NtSuspendResumeProcessFn>(
            reinterpret_cast<void *>(GetProcAddress(module, "NtSuspendProcess")));
        t.resume_process = reinterpret_cast<NtSuspendResumeProcessFn>(
            reinterpret_cast<void *>(GetProcAddress(module, "NtResumeProcess")));
        t.get_version =
            reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void *>(GetProcAddress(module, "RtlGetVersion")));
        return t;
    }();
    return table;
}

bool query_system(int information_class, std::vector<char> &buffer) {
    if (!ntdll().query_system) return false;
    if (buffer.size() < 64 * 1024) buffer.resize(64 * 1024);
    for (int attempt = 0; attempt < 8; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS status = ntdll().query_system(static_cast<ULONG>(information_class), buffer.data(),
                                                     static_cast<ULONG>(buffer.size()), &needed);
        if (status >= 0) return true;
        // STATUS_INFO_LENGTH_MISMATCH / STATUS_BUFFER_TOO_SMALL: the table grew, retry with headroom.
        if (static_cast<ULONG>(status) != 0xC0000004 && static_cast<ULONG>(status) != 0xC0000023) return false;
        buffer.resize(std::max<size_t>(needed + needed / 4, buffer.size() * 2));
    }
    return false;
}

std::vector<const ProcessEntry *> process_entries(const std::vector<char> &buffer) {
    std::vector<const ProcessEntry *> out;
    size_t offset = 0;
    while (offset + sizeof(ProcessEntry) <= buffer.size()) {
        auto entry = reinterpret_cast<const ProcessEntry *>(buffer.data() + offset);
        out.push_back(entry);
        if (entry->NextEntryOffset == 0) break;
        offset += entry->NextEntryOffset;
    }
    return out;
}

std::string utf8(const wchar_t *text, size_t length) {
    if (!text || length == 0) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), size, nullptr, nullptr);
    return out;
}

std::string utf8(const std::wstring &text) { return utf8(text.c_str(), text.size()); }

std::string utf8(const UNICODE_STRING &text) { return utf8(text.Buffer, text.Length / sizeof(wchar_t)); }

std::wstring wide(const std::string &text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string lower_ascii(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string basename_of(const std::string &path) {
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

int64_t filetime_to_unix(uint64_t filetime) {
    if (filetime == 0) return 0;
    constexpr uint64_t kEpochDifference = 116444736000000000ull;  // 1601 -> 1970 in 100 ns
    return filetime < kEpochDifference ? 0 : static_cast<int64_t>((filetime - kEpochDifference) / 10000000ull);
}

pc_result error_result(DWORD error) {
    switch (error) {
        case ERROR_SUCCESS: return PC_OK;
        case ERROR_ACCESS_DENIED: return PC_ERR_PERMISSION;
        case ERROR_INVALID_PARAMETER:
        case ERROR_NOT_FOUND:
        case ERROR_SERVICE_DOES_NOT_EXIST: return PC_ERR_NOT_FOUND;
        case ERROR_INVALID_HANDLE: return PC_ERR_INVALID;
        default: return PC_ERR_FAILED;
    }
}

Handle open_process(int32_t pid, DWORD access) {
    if (pid <= 0) return Handle();
    return Handle(OpenProcess(access, FALSE, static_cast<DWORD>(pid)));
}

std::string image_path(HANDLE process) {
    wchar_t buffer[1024];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!QueryFullProcessImageNameW(process, 0, buffer, &size)) return {};
    return utf8(buffer, size);
}

namespace {

struct UserTable {
    std::mutex mutex;
    std::unordered_map<std::string, uint32_t> by_sid;  // "S-1-5-21-..." -> uid
    std::unordered_map<uint32_t, std::string> names;
    uint32_t next = kFirstRegularUser;
};

UserTable &users() {
    static UserTable table;
    return table;
}

// Well-known service accounts get fixed low uids, like root and the daemon accounts on Unix.
bool well_known_uid(const std::string &sid, uint32_t &uid) {
    if (sid == "S-1-5-18") {
        uid = 0;
        return true;
    }
    if (sid == "S-1-5-19") {
        uid = 1;
        return true;
    }
    if (sid == "S-1-5-20") {
        uid = 2;
        return true;
    }
    if (sid.rfind("S-1-5-90-", 0) == 0) {  // Window Manager (dwm)
        uid = 3;
        return true;
    }
    if (sid.rfind("S-1-5-96-", 0) == 0) {  // Font Driver Host
        uid = 4;
        return true;
    }
    return false;
}

std::string account_name(PSID sid) {
    wchar_t name[256], domain[256];
    DWORD name_size = static_cast<DWORD>(std::size(name)), domain_size = static_cast<DWORD>(std::size(domain));
    SID_NAME_USE use;
    if (!LookupAccountSidW(nullptr, sid, name, &name_size, domain, &domain_size, &use)) return {};
    return utf8(name, name_size);
}

}  // namespace

uint32_t process_user(HANDLE process) {
    Handle token;
    {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return kUnknownUser;
        token = Handle(raw);
    }
    alignas(TOKEN_USER) char buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenUser, buffer, sizeof(buffer), &size)) return kUnknownUser;
    PSID sid = reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid;
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(sid, &sid_text)) return kUnknownUser;
    const std::string key = utf8(sid_text, wcslen(sid_text));
    LocalFree(sid_text);

    UserTable &table = users();
    std::lock_guard lock(table.mutex);
    if (auto it = table.by_sid.find(key); it != table.by_sid.end()) return it->second;
    uint32_t uid = 0;
    if (!well_known_uid(key, uid)) uid = table.next++;
    table.by_sid.emplace(key, uid);
    if (!table.names.count(uid)) {
        std::string name = account_name(sid);
        if (name.empty()) name = key;
        table.names.emplace(uid, std::move(name));
    }
    return uid;
}

std::string registry_string(HKEY root, const wchar_t *key, const wchar_t *value) {
    wchar_t buffer[1024];
    DWORD size = sizeof(buffer), type = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buffer, &size) != ERROR_SUCCESS)
        return {};
    const size_t length = size / sizeof(wchar_t);
    return utf8(buffer, length > 0 && buffer[length - 1] == L'\0' ? length - 1 : length);
}

uint32_t registry_dword(HKEY root, const wchar_t *key, const wchar_t *value, uint32_t fallback) {
    DWORD data = 0, size = sizeof(data);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) return fallback;
    return data;
}

bool is_elevated() {
    static const bool elevated = [] {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
        Handle token(raw);
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
               elevation.TokenIsElevated != 0;
    }();
    return elevated;
}

std::string executable_of_command(const std::string &command) {
    std::wstring expanded = wide(command);
    wchar_t buffer[2048];
    if (DWORD n = ExpandEnvironmentStringsW(expanded.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
        n > 0 && n <= std::size(buffer))
        expanded.assign(buffer, n - 1);
    std::string text = utf8(expanded);
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    if (text[start] == '"') {
        const size_t end = text.find('"', start + 1);
        return end == std::string::npos ? text.substr(start + 1) : text.substr(start + 1, end - start - 1);
    }
    // Unquoted: the executable ends at ".exe" (paths with spaces are common) or at the first space.
    const std::string lowered = lower_ascii(text);
    if (const size_t exe = lowered.find(".exe", start); exe != std::string::npos)
        return text.substr(start, exe + 4 - start);
    const size_t space = text.find(' ', start);
    return text.substr(start, space == std::string::npos ? std::string::npos : space - start);
}

}  // namespace win

// ---------------------------------------------------------------------------------------------
// Process table
// ---------------------------------------------------------------------------------------------

namespace {

using namespace win;

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

// What a process handle tells us beyond the kernel's table, read once per process lifetime.
struct ProcessCache {
    int64_t start_time = 0;
    std::string path;
    uint32_t uid = kUnknownUser;
    bool critical = false;  // ending it would halt Windows (csrss, wininit, ...)
    bool opened = false;    // the handle could be opened; else retried now and then
    uint64_t seen = 0;
};

struct ProcessState {
    std::mutex mutex;
    std::unordered_map<int32_t, ProcessCache> cache;
    std::unordered_map<std::string, int32_t> pid_by_path;  // lower-case path -> a running pid
    std::unordered_set<int32_t> critical;
    uint64_t generation = 0;
    uint64_t compressed_bytes = 0;  // Memory Compression's working set
    bool compression_seen = false;
};

ProcessState &state() {
    static ProcessState s;
    return s;
}

int64_t create_time_unix(const ProcessEntry &entry) {
    return filetime_to_unix(static_cast<uint64_t>(entry.CreateTime.QuadPart));
}

int32_t nice_of(LONG base_priority) {
    if (base_priority >= 24) return -20;  // realtime
    if (base_priority >= 13) return -15;  // high
    if (base_priority >= 10) return -8;   // above normal
    if (base_priority >= 8) return 0;     // normal
    if (base_priority >= 6) return 8;     // below normal
    return 15;                            // idle
}

int32_t state_of(const ProcessEntry &entry) {
    if (entry.NumberOfThreads == 0) return PC_STATE_ZOMBIE;  // exited, a handle keeps the entry
    bool all_suspended = true;
    for (ULONG i = 0; i < entry.NumberOfThreads; ++i) {
        const ThreadEntry &t = entry.Threads[i];
        // Running, ready or standby: on a CPU or about to be, as thread_state reports a thread.
        if (t.ThreadState == kThreadRunning || t.ThreadState == 1 || t.ThreadState == 3) return PC_STATE_RUNNING;
        if (!(t.ThreadState == kThreadWaiting && t.WaitReason == kWaitSuspended)) all_suspended = false;
    }
    return all_suspended ? PC_STATE_STOPPED : PC_STATE_SLEEPING;
}

bool query_critical(HANDLE process) {
    if (!ntdll().query_process) return false;
    ULONG flag = 0;
    ULONG size = 0;
    return ntdll().query_process(process, kProcessBreakOnTermination, &flag, sizeof(flag), &size) >= 0 && flag != 0;
}

// Fills the cached handle-derived fields for one table entry.
void refresh_cache(ProcessState &s, int32_t pid, const ProcessEntry &entry, ProcessCache &c) {
    const int64_t start = create_time_unix(entry);
    const bool fresh = c.seen == 0 || c.start_time != start;
    if (fresh) c = ProcessCache{};
    c.start_time = start;
    c.seen = s.generation;
    if (pid == 0 || pid == 4) {
        c.opened = true;
        c.uid = 0;
        c.critical = true;
        return;
    }
    // A handle that failed is retried every few seconds: rights can change (elevation) and so
    // can the process (a protected process stays unreadable forever, cheaply).
    if (c.opened || (!fresh && s.generation % 10 != 0)) return;
    Handle process = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!process) return;
    c.opened = true;
    c.path = image_path(process.get());
    c.uid = process_user(process.get());
    c.critical = query_critical(process.get());
}

// The processes Windows cannot run without. The kernel's own flag (ProcessBreakOnTermination) is
// the authority, but it needs a handle, and without administrator rights none of these can be
// opened: a name on this list, run by the OS itself (SYSTEM or a token we may not read), is
// protected just the same, as kernel_task and launchd are on macOS.
bool critical_name(const std::string &name) {
    static const char *const kCritical[] = {"smss.exe",       "csrss.exe",          "wininit.exe",  "winlogon.exe",
                                            "services.exe",   "lsass.exe",          "lsaiso.exe",   "registry",
                                            "memcompression", "memory compression", "secure system"};
    const std::string lowered = lower_ascii(name);
    for (const char *critical : kCritical)
        if (lowered == critical) return true;
    return false;
}

std::string entry_name(int32_t pid, const ProcessEntry &entry, const ProcessCache &c) {
    if (pid == 0) return "System Idle Process";
    if (pid == 4) return "System";
    std::string name = utf8(entry.ImageName);
    if (name.empty() && !c.path.empty()) name = basename_of(c.path);
    if (name.empty()) name = "pid " + std::to_string(pid);
    return name;
}

void fill_counters(const ProcessEntry &entry, RawProcess &p) {
    p.restricted = false;
    p.cpu_time_ns = static_cast<uint64_t>(entry.UserTime.QuadPart + entry.KernelTime.QuadPart) * 100;
    // Private working set, what Task Manager's Memory column shows.
    p.memory_bytes = entry.WorkingSetPrivateSize.QuadPart > 0 ? entry.WorkingSetPrivateSize.QuadPart
                                                              : static_cast<int64_t>(entry.WorkingSetSize);
    p.has_disk_io = true;
    p.disk_read = static_cast<uint64_t>(entry.ReadTransferCount.QuadPart);
    p.disk_write = static_cast<uint64_t>(entry.WriteTransferCount.QuadPart);
    p.threads = static_cast<int32_t>(entry.NumberOfThreads);
    p.nice = nice_of(entry.BasePriority);
    p.state = state_of(entry);
}

}  // namespace

bool helper_supported() { return false; }

bool protected_pid(int32_t pid) {
    // The idle process (0) and System (4); pids are multiples of 4, nothing lives in between.
    if (pid >= 0 && pid <= 4) return true;
    ProcessState &s = state();
    std::lock_guard lock(s.mutex);
    return s.critical.count(pid) != 0;
}

bool process_parents(std::vector<ProcessParent> &out) {
    out.clear();
    std::vector<char> buffer;
    if (!query_system(kSystemProcessInformation, buffer)) return false;
    const auto entries = process_entries(buffer);
    // The parent pid outlives the parent: once that pid is reused, the kernel still reports it.
    // pc_process_end_tree follows these links to kill a whole tree, so a reused parent pid must
    // not adopt strangers: a parent younger than its "child" is no parent (same rule as processes()).
    std::unordered_map<int32_t, int64_t> starts;
    starts.reserve(entries.size());
    for (const ProcessEntry *entry : entries)
        starts.emplace(static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry->UniqueProcessId)),
                       create_time_unix(*entry));
    out.reserve(entries.size());
    for (const ProcessEntry *entry : entries) {
        const auto pid = static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry->UniqueProcessId));
        auto ppid = static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry->InheritedFromUniqueProcessId));
        const auto parent = starts.find(ppid);
        if (pid != 0 && (parent == starts.end() || parent->second > create_time_unix(*entry))) ppid = -1;
        out.push_back({pid, ppid});
    }
    return true;
}

bool processes(std::vector<RawProcess> &out) {
    out.clear();
    std::vector<char> buffer;
    if (!query_system(kSystemProcessInformation, buffer)) return false;
    const auto entries = process_entries(buffer);
    out.reserve(entries.size());

    ProcessState &s = state();
    std::lock_guard lock(s.mutex);
    ++s.generation;
    s.pid_by_path.clear();
    s.critical.clear();
    s.compression_seen = false;
    for (const ProcessEntry *entry : entries) {
        RawProcess p;
        p.pid = static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry->UniqueProcessId));
        // The idle process is the CPU's spare time, not a process anyone can act on: Task Manager
        // hides it too, and listing it would top every CPU ranking.
        if (p.pid == 0) continue;
        p.ppid = static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry->InheritedFromUniqueProcessId));
        // The parent pid is inherited even after the parent dies and its pid is reused: only
        // trust it when the parent is older than the child.
        ProcessCache &c = s.cache[p.pid];
        refresh_cache(s, p.pid, *entry, c);
        p.start_time = c.start_time;
        p.uid = c.uid;
        p.path = c.path;
        p.name = entry_name(p.pid, *entry, c);
        fill_counters(*entry, p);
        const bool os_owned = p.uid == kUnknownUser || p.uid < kFirstRegularUser;
        if (c.critical || (os_owned && critical_name(p.name))) s.critical.insert(p.pid);
        if (!p.path.empty()) s.pid_by_path.emplace(lower_ascii(p.path), p.pid);
        if (p.name == "MemCompression" || p.name == "Memory Compression") {
            s.compressed_bytes = entry->WorkingSetSize;
            s.compression_seen = true;
        }
        out.push_back(std::move(p));
    }
    // Pids that vanished take their cache entries with them.
    for (auto it = s.cache.begin(); it != s.cache.end();) {
        if (it->second.seen != s.generation)
            it = s.cache.erase(it);
        else
            ++it;
    }
    // Parent links: a reused parent pid is younger than its "child".
    std::unordered_map<int32_t, int64_t> starts;
    for (const auto &p : out) starts.emplace(p.pid, p.start_time);
    for (auto &p : out) {
        auto parent = starts.find(p.ppid);
        if (parent == starts.end() || parent->second > p.start_time) p.ppid = p.pid == 0 ? 0 : -1;
    }
    return true;
}

bool read_counters(int32_t pid, RawProcess &out) {
    std::vector<char> buffer;
    if (!query_system(kSystemProcessInformation, buffer)) return false;
    for (const ProcessEntry *entry : process_entries(buffer)) {
        if (static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry->UniqueProcessId)) != pid) continue;
        fill_counters(*entry, out);
        return true;
    }
    out.restricted = true;
    return false;
}

int64_t start_time(int32_t pid) {
    Handle process = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED ? 0 : -1;
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process.get(), &creation, &exit, &kernel, &user)) return -1;
    return filetime_to_unix(filetime_value(creation));
}

int32_t win::pid_of_executable(const std::string &path) {
    ProcessState &s = state();
    std::lock_guard lock(s.mutex);
    auto it = s.pid_by_path.find(lower_ascii(path));
    return it == s.pid_by_path.end() ? 0 : it->second;
}

// ---------------------------------------------------------------------------------------------
// Machine-wide metrics
// ---------------------------------------------------------------------------------------------

bool cpu_ticks(std::vector<CpuTicks> &out) {
    if (!ntdll().query_system) return false;
    // One record per logical processor of the current processor group; unlike the other classes
    // this one insists on a buffer of exactly that size.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    std::vector<char> buffer(static_cast<size_t>(info.dwNumberOfProcessors) *
                             sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
    ULONG needed = 0;
    NTSTATUS status = ntdll().query_system(kSystemProcessorPerformanceInformation, buffer.data(),
                                           static_cast<ULONG>(buffer.size()), &needed);
    if (status < 0 && needed > 0 && needed != buffer.size()) {
        buffer.resize(needed);
        status = ntdll().query_system(kSystemProcessorPerformanceInformation, buffer.data(),
                                      static_cast<ULONG>(buffer.size()), &needed);
    }
    if (status < 0) return false;
    const size_t count = std::min<size_t>(needed, buffer.size()) / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION);
    out.resize(count);
    auto records = reinterpret_cast<const SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION *>(buffer.data());
    for (size_t i = 0; i < count; ++i) {
        // 100 ns units -> 10 ms ticks, the resolution the portable tick deltas expect. KernelTime
        // includes idle: subtract before rounding, so every counter stays monotonic (a difference
        // of two rounded values can go down by a tick and would wrap the 32-bit delta).
        const uint64_t idle_raw = static_cast<uint64_t>(records[i].IdleTime.QuadPart);
        const uint64_t kernel_raw = static_cast<uint64_t>(records[i].KernelTime.QuadPart);
        out[i].user = static_cast<uint32_t>(static_cast<uint64_t>(records[i].UserTime.QuadPart) / 100000);
        out[i].system = static_cast<uint32_t>((kernel_raw > idle_raw ? kernel_raw - idle_raw : 0) / 100000);
        out[i].idle = static_cast<uint32_t>(idle_raw / 100000);
        out[i].nice = 0;
    }
    return true;
}

namespace {

struct MemoryListInformation {
    ULONG_PTR ZeroPageCount;
    ULONG_PTR FreePageCount;
    ULONG_PTR ModifiedPageCount;
    ULONG_PTR ModifiedNoWritePageCount;
    ULONG_PTR BadPageCount;
    ULONG_PTR PageCountByPriority[8];
    ULONG_PTR RepurposedPagesByPriority[8];
    ULONG_PTR ModifiedPageCountPageFile;
};

struct PagefileInformation {
    ULONG NextEntryOffset;
    ULONG TotalSize;  // pages
    ULONG TotalInUse;
    ULONG PeakUsage;
    UNICODE_STRING PageFileName;
};

}  // namespace

bool memory(Memory &out) {
    out = {};
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return false;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uint64_t page = info.dwPageSize;
    out.total = status.ullTotalPhys;

    // Task Manager's composition: in use, modified, standby (cached), free.
    uint64_t standby = 0, free_pages = 0, modified = 0;
    std::vector<char> buffer(sizeof(MemoryListInformation));
    if (ntdll().query_system && ntdll().query_system(kSystemMemoryListInformation, buffer.data(),
                                                     static_cast<ULONG>(buffer.size()), nullptr) >= 0) {
        auto lists = reinterpret_cast<const MemoryListInformation *>(buffer.data());
        for (ULONG_PTR count : lists->PageCountByPriority) standby += count;
        free_pages = lists->ZeroPageCount + lists->FreePageCount;
        modified = lists->ModifiedPageCount;
        standby *= page;
        free_pages *= page;
        modified *= page;
    } else {
        free_pages = status.ullAvailPhys;
    }
    out.free = free_pages;
    out.cached = standby;
    out.used = out.total > out.free + out.cached ? out.total - out.free - out.cached : 0;
    (void)modified;  // counted as in use, like Task Manager's "In use"

    PERFORMANCE_INFORMATION perf{};
    perf.cb = sizeof(perf);
    if (GetPerformanceInfo(&perf, sizeof(perf))) out.wired = static_cast<uint64_t>(perf.KernelNonpaged) * perf.PageSize;
    {
        ProcessState &s = state();
        std::lock_guard lock(s.mutex);
        out.compressed = s.compression_seen ? s.compressed_bytes : 0;
    }
    out.app = out.used > out.wired + out.compressed ? out.used - out.wired - out.compressed : 0;

    // Swap: the page files themselves, not the commit charge.
    std::vector<char> pagefiles;
    if (query_system(kSystemPagefileInformation, pagefiles)) {
        size_t offset = 0;
        while (offset + sizeof(PagefileInformation) <= pagefiles.size()) {
            auto entry = reinterpret_cast<const PagefileInformation *>(pagefiles.data() + offset);
            if (entry->TotalSize == 0 && entry->NextEntryOffset == 0 && offset == 0 && entry->PageFileName.Length == 0)
                break;  // no page file configured
            out.swap_total += static_cast<uint64_t>(entry->TotalSize) * page;
            out.swap_used += static_cast<uint64_t>(entry->TotalInUse) * page;
            if (entry->NextEntryOffset == 0) break;
            offset += entry->NextEntryOffset;
        }
    }

    // Pressure: the kernel's low-memory signal, then the available share.
    static Handle low_memory(CreateMemoryResourceNotification(LowMemoryResourceNotification));
    BOOL low = FALSE;
    const double available = out.total ? static_cast<double>(out.free + out.cached) / out.total : 1;
    if (low_memory && QueryMemoryResourceNotification(low_memory.get(), &low) && low)
        out.pressure = PC_PRESSURE_CRITICAL;
    else if (available < 0.1)
        out.pressure = PC_PRESSURE_WARNING;
    else
        out.pressure = PC_PRESSURE_NORMAL;
    return true;
}

namespace {

// Bytes moved by every physical disk, from the disk class driver's counters.
void disk_counters(IoCounters &out) {
    for (int drive = 0; drive < 64; ++drive) {
        wchar_t path[32];
        (void)swprintf_s(path, L"\\\\.\\PhysicalDrive%d", drive);
        // No read or write access is needed for the statistics IOCTL, so a normal user can ask.
        Handle disk(CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        if (!disk) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND && drive > 8) break;
            continue;
        }
        DISK_PERFORMANCE perf{};
        DWORD size = 0;
        if (DeviceIoControl(disk.get(), IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &size, nullptr)) {
            out.disk_read += static_cast<uint64_t>(perf.BytesRead.QuadPart);
            out.disk_write += static_cast<uint64_t>(perf.BytesWritten.QuadPart);
        }
    }
}

// Machine-wide traffic is what crosses a physical link: virtual adapters (VPN tunnels, Hyper-V
// switches, loopback) carry bytes a second time.
void network_counters(IoCounters &out) {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 &row = table->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || !row.InterfaceAndOperStatusFlags.HardwareInterface) continue;
        out.net_rx += row.InOctets;
        out.net_tx += row.OutOctets;
    }
    FreeMibTable(table);
}

}  // namespace

bool io_counters(IoCounters &out) {
    out = {};
    disk_counters(out);
    network_counters(out);
    return true;
}

// Windows keeps no load average; the UI hides it when every value is 0.
void load_average(double out[3]) { out[0] = out[1] = out[2] = 0; }

std::vector<pc_volume> volumes() {
    std::vector<pc_volume> result;
    wchar_t windows_dir[MAX_PATH] = {};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t label[256] = {}, file_system[64] = {};
        if (!GetVolumeInformationW(root, label, 256, nullptr, nullptr, nullptr, file_system, 64)) continue;
        ULARGE_INTEGER available{}, total{}, free_total{};
        if (!GetDiskFreeSpaceExW(root, &available, &total, &free_total)) continue;

        pc_volume volume{};
        copy_string(volume.mount_point, sizeof(volume.mount_point), utf8(root, 3));
        copy_string(volume.file_system, sizeof(volume.file_system), utf8(file_system, wcslen(file_system)));
        volume.total_bytes = total.QuadPart;
        volume.available_bytes = available.QuadPart;
        volume.is_root = windows_dir[0] != 0 && towupper(windows_dir[0]) == root[0];
        volume.is_removable = type == DRIVE_REMOVABLE;
        std::string name = utf8(label, wcslen(label));
        if (name.empty()) name = volume.is_root ? "Windows" : type == DRIVE_REMOVABLE ? "Removable Disk" : "Local Disk";
        name += " (" + utf8(root, 2) + ")";
        copy_string(volume.name, sizeof(volume.name), name);
        result.push_back(volume);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// System info
// ---------------------------------------------------------------------------------------------

namespace {

// EfficiencyClass per logical processor: hybrid chips give performance cores the highest class.
void read_core_kinds(pc_system_info &out) {
    using GetSystemCpuSetInformationFn = BOOL(WINAPI *)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    auto fn = reinterpret_cast<GetSystemCpuSetInformationFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation")));
    if (!fn) return;
    ULONG size = 0;
    fn(nullptr, 0, &size, GetCurrentProcess(), 0);
    if (size == 0) return;
    std::vector<char> buffer(size);
    if (!fn(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), size, &size, GetCurrentProcess(), 0)) return;
    BYTE highest = 0, lowest = 255;
    std::vector<std::pair<ULONG, BYTE>> cores;
    for (size_t offset = 0; offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= size;) {
        auto info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION *>(buffer.data() + offset);
        if (info->Size == 0) break;
        if (info->Type == CpuSetInformation) {
            cores.emplace_back(info->CpuSet.LogicalProcessorIndex, info->CpuSet.EfficiencyClass);
            highest = std::max(highest, info->CpuSet.EfficiencyClass);
            lowest = std::min(lowest, info->CpuSet.EfficiencyClass);
        }
        offset += info->Size;
    }
    if (cores.empty() || highest == lowest) return;
    for (const auto &[index, efficiency] : cores) {
        if (index >= std::size(out.core_kinds)) continue;
        out.core_kinds[index] = static_cast<uint8_t>(efficiency == highest ? PC_CORE_PERFORMANCE : PC_CORE_EFFICIENCY);
    }
    // Physical performance/efficiency cores: logical processors of each class, minus SMT siblings
    // (performance cores on Intel hybrid chips have two threads, efficiency cores one).
    int32_t performance_logical = 0, efficiency_logical = 0;
    for (const auto &[index, efficiency] : cores) (efficiency == highest ? performance_logical : efficiency_logical)++;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<char> relations(length);
    int32_t performance_physical = 0, efficiency_physical = 0;
    if (length && GetLogicalProcessorInformationEx(
                      RelationProcessorCore,
                      reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(relations.data()), &length)) {
        for (DWORD offset = 0; offset < length;) {
            auto rel = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(relations.data() + offset);
            if (rel->Relationship == RelationProcessorCore) {
                // The core's first logical processor tells its class.
                const KAFFINITY mask = rel->Processor.GroupMask[0].Mask;
                ULONG first = 0;
                while (first < 64 && !(mask & (static_cast<KAFFINITY>(1) << first))) ++first;
                const ULONG index = rel->Processor.GroupMask[0].Group * 64 + first;
                bool performance = false;
                for (const auto &[i, e] : cores)
                    if (i == index) performance = e == highest;
                (performance ? performance_physical : efficiency_physical)++;
            }
            offset += rel->Size;
        }
    }
    out.performance_cores = performance_physical ? performance_physical : performance_logical;
    out.efficiency_cores = efficiency_physical ? efficiency_physical : efficiency_logical;
}

int32_t physical_core_count() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<char> buffer(length);
    if (!length ||
        !GetLogicalProcessorInformationEx(
            RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length))
        return 0;
    int32_t cores = 0;
    for (DWORD offset = 0; offset < length;) {
        auto rel = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + offset);
        if (rel->Relationship == RelationProcessorCore) ++cores;
        offset += rel->Size;
    }
    return cores;
}

}  // namespace

bool system_info(pc_system_info &out) {
    std::memset(&out, 0, sizeof(out));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (ntdll().get_version) ntdll().get_version(&version);
    const wchar_t *current = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const uint32_t ubr = registry_dword(HKEY_LOCAL_MACHINE, current, L"UBR");
    std::string product = version.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    if (version.dwMajorVersion < 10) product = "Windows";
    copy_string(out.os_name, sizeof(out.os_name), product);
    std::string display = registry_string(HKEY_LOCAL_MACHINE, current, L"DisplayVersion");
    if (display.empty()) display = registry_string(HKEY_LOCAL_MACHINE, current, L"ReleaseId");
    copy_string(out.os_version, sizeof(out.os_version), display);
    copy_string(out.os_build, sizeof(out.os_build), std::to_string(version.dwBuildNumber) + "." + std::to_string(ubr));
    copy_string(out.kernel, sizeof(out.kernel),
                std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + "." +
                    std::to_string(version.dwBuildNumber) + "." + std::to_string(ubr));
    wchar_t host[256] = {};
    DWORD host_size = static_cast<DWORD>(std::size(host));
    if (GetComputerNameExW(ComputerNameDnsHostname, host, &host_size))
        copy_string(out.hostname, sizeof(out.hostname), utf8(host, host_size));

    const wchar_t *bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    copy_string(out.model_id, sizeof(out.model_id), registry_string(HKEY_LOCAL_MACHINE, bios, L"SystemProductName"));
    // Firmware placeholders ("Default string", "To be filled by O.E.M.") are not names.
    const auto placeholder = [](const std::string &value) {
        const std::string lowered = lower_ascii(value);
        return lowered.empty() || lowered.find("to be filled") != std::string::npos ||
               lowered.find("default string") != std::string::npos || lowered == "system manufacturer" ||
               lowered == "system product name" || lowered == "none";
    };
    std::string manufacturer = registry_string(HKEY_LOCAL_MACHINE, bios, L"SystemManufacturer");
    std::string family = registry_string(HKEY_LOCAL_MACHINE, bios, L"SystemFamily");
    if (placeholder(manufacturer)) manufacturer.clear();
    if (placeholder(family)) family.clear();
    if (placeholder(out.model_id)) out.model_id[0] = '\0';
    copy_string(out.model_name, sizeof(out.model_name),
                family.empty()         ? manufacturer
                : manufacturer.empty() ? family
                                       : manufacturer + " " + family);

    const wchar_t *cpu0 = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    std::string brand = registry_string(HKEY_LOCAL_MACHINE, cpu0, L"ProcessorNameString");
    // Collapse the padding some firmware leaves in the brand string.
    std::string trimmed;
    for (char c : brand)
        if (c != ' ' || (!trimmed.empty() && trimmed.back() != ' ')) trimmed += c;
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    copy_string(out.cpu_brand, sizeof(out.cpu_brand), trimmed);
    out.cpu_frequency_hz = static_cast<uint64_t>(registry_dword(HKEY_LOCAL_MACHINE, cpu0, L"~MHz")) * 1000000;

    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    const char *arch = "x86_64";
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
        case PROCESSOR_ARCHITECTURE_ARM: arch = "arm"; break;
        default: break;
    }
    copy_string(out.arch, sizeof(out.arch), arch);
    out.logical_cores = static_cast<int32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    out.physical_cores = physical_core_count();
    if (!out.physical_cores) out.physical_cores = out.logical_cores;
    read_core_kinds(out);

    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) out.memory_total = status.ullTotalPhys;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    out.boot_time = filetime_to_unix(filetime_value(now) - GetTickCount64() * 10000);
    return true;
}

namespace {

// Whether memory compression is on: the Memory Compression process exists then (it can be
// switched off with Disable-MMAgent, and the figure is meaningless without it).
bool memory_compression_present() {
    std::vector<char> buffer;
    if (!query_system(kSystemProcessInformation, buffer)) return false;
    for (const ProcessEntry *entry : process_entries(buffer)) {
        const std::string name = utf8(entry->ImageName);
        if (name == "MemCompression" || name == "Memory Compression") return true;
    }
    return false;
}

}  // namespace

uint32_t capabilities() {
    uint32_t caps = PC_CAP_PROCESS_DISK_IO | PC_CAP_MEMORY_PRESSURE | PC_CAP_SWAP | PC_CAP_PRIORITY | PC_CAP_SUSPEND |
                    PC_CAP_CPU_AFFINITY | PC_CAP_SERVICES | PC_CAP_STARTUP | PC_CAP_OPEN_FILES | PC_CAP_CONNECTIONS;
    if (memory_compression_present()) caps |= PC_CAP_MEMORY_COMPRESSED;
    pc_system_info info;
    if (system_info(info) && info.performance_cores > 0 && info.efficiency_cores > 0) caps |= PC_CAP_HYBRID_CORES;
    if (process_network_available()) caps |= PC_CAP_PROCESS_NETWORK;
    if (!gpus().empty()) caps |= PC_CAP_GPU;
    if (process_gpu_available()) caps |= PC_CAP_PROCESS_GPU;
    if (cpu_temperature() >= 0) caps |= PC_CAP_TEMPERATURE;
    pc_battery battery_info;
    if (battery(battery_info) && battery_info.present) caps |= PC_CAP_BATTERY;
    return caps;
}

// ---------------------------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------------------------

std::string user_name(uint32_t uid) {
    if (uid == kUnknownUser) return {};
    UserTable &table = users();
    std::lock_guard lock(table.mutex);
    auto it = table.names.find(uid);
    if (it != table.names.end()) return it->second;
    // Well-known accounts that were assigned without a token (System, pid 4).
    switch (uid) {
        case 0: return "SYSTEM";
        case 1: return "LOCAL SERVICE";
        case 2: return "NETWORK SERVICE";
        case 3: return "DWM";
        case 4: return "Font Driver Host";
        default: return std::to_string(uid);
    }
}

std::string win::windows_directory() {
    static const std::string directory = [] {
        wchar_t buffer[MAX_PATH] = {};
        GetWindowsDirectoryW(buffer, MAX_PATH);
        return lower_ascii(utf8(buffer, wcslen(buffer)));
    }();
    return directory;
}

bool win::under_windows_directory(const std::string &path) {
    const std::string root = windows_directory();
    if (root.empty() || path.size() <= root.size()) return false;
    const std::string lowered = lower_ascii(path);
    return lowered.compare(0, root.size(), root) == 0 && (lowered[root.size()] == '\\' || lowered[root.size()] == '/');
}

// FileDescription from the executable's version resource ("Google Chrome"), what Task Manager
// shows as the process name; the file name without ".exe" when there is none.
std::string win::product_name(const std::string &path) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> cache;
    const std::string key = lower_ascii(path);
    std::lock_guard lock(mutex);
    if (auto it = cache.find(key); it != cache.end()) return it->second;

    std::string name = basename_of(path);
    if (const std::string lowered = lower_ascii(name);
        lowered.size() > 4 && lowered.compare(lowered.size() - 4, 4, ".exe") == 0)
        name.resize(name.size() - 4);
    const std::wstring wpath = wide(path);
    DWORD handle = 0;
    if (const DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &handle); size > 0) {
        std::vector<char> data(size);
        if (GetFileVersionInfoW(wpath.c_str(), 0, size, data.data())) {
            struct Translation {
                WORD language, codepage;
            } *translations = nullptr;
            UINT length = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translations),
                               &length) &&
                length >= sizeof(Translation)) {
                for (UINT i = 0; i < length / sizeof(Translation); ++i) {
                    wchar_t query[64];
                    (void)swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[i].language,
                                     translations[i].codepage);
                    wchar_t *value = nullptr;
                    UINT value_length = 0;
                    if (VerQueryValueW(data.data(), query, reinterpret_cast<void **>(&value), &value_length) && value &&
                        value_length > 1) {
                        std::string description = utf8(value, wcsnlen(value, value_length));
                        while (!description.empty() && (description.back() == ' ' || description.back() == '\0'))
                            description.pop_back();
                        if (!description.empty()) {
                            name = description;
                            break;
                        }
                    }
                }
            }
        }
    }
    cache.emplace(key, name);
    return name;
}

AppIdentity app_identity(const RawProcess &process) {
    // Windows has no bundles: every instance of one executable is the same app. What lives under
    // the Windows directory (svchost, dwm, the shell's helpers) is the OS, not an app: like the
    // daemons of /System on macOS, it groups by name and is not flagged PC_PROC_APP_BUNDLE, so
    // the "app" views and icons stay about the programs the user installed.
    if (process.path.empty()) return {"exe:" + process.name, process.name};
    if (under_windows_directory(process.path)) return {"exe:" + process.name, product_name(process.path)};
    return {process.path, product_name(process.path)};
}

bool is_system_process(const RawProcess &process) {
    if (process.pid <= 4) return true;
    if (process.uid < kFirstRegularUser) return true;  // SYSTEM and the service accounts
    // A process whose token we can't read belongs to another user or is protected: treat it
    // with the same care as a system one.
    if (process.uid == kUnknownUser) return true;
    // The OS's own programs in the user's session (explorer, sihost, RuntimeBroker, ctfmon), like
    // Finder and the Dock under /System on macOS: ending one needs confirmation.
    return under_windows_directory(process.path);
}

int32_t self_pid() { return static_cast<int32_t>(GetCurrentProcessId()); }

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------

namespace {

// Signal numbers the portable layer uses (BSD numbering, see monitor.cpp).
constexpr int32_t kSigKill = 9;
constexpr int32_t kSigTerm = 15;
constexpr int32_t kSigStop = 17;
constexpr int32_t kSigCont = 19;

struct WindowSearch {
    DWORD pid;
    std::vector<HWND> windows;
};

BOOL CALLBACK collect_windows(HWND hwnd, LPARAM param) {
    auto search = reinterpret_cast<WindowSearch *>(param);
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner == search->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) search->windows.push_back(hwnd);
    return TRUE;
}

pc_result terminate(int32_t pid) {
    Handle process = open_process(pid, PROCESS_TERMINATE);
    if (!process) return last_error_result();
    return TerminateProcess(process.get(), 1) ? PC_OK : last_error_result();
}

pc_result nt_result(NTSTATUS status) {
    if (status >= 0) return PC_OK;
    switch (static_cast<ULONG>(status)) {
        case 0xC0000022: return PC_ERR_PERMISSION;  // STATUS_ACCESS_DENIED
        case 0xC000000B:                            // STATUS_INVALID_CID
        case 0xC000010A: return PC_ERR_NOT_FOUND;   // STATUS_PROCESS_IS_TERMINATING
        default: return PC_ERR_FAILED;
    }
}

}  // namespace

bool valid_signal(int32_t signal) {
    return signal == kSigKill || signal == kSigTerm || signal == kSigStop || signal == kSigCont;
}

pc_result send_signal(int32_t pid, int32_t signal) {
    if (!valid_signal(signal)) return PC_ERR_INVALID;
    switch (signal) {
        case kSigKill: return terminate(pid);
        case kSigTerm: {
            // Like Task Manager's End task: a window gets a close request, a background process is
            // ended.
            WindowSearch search{static_cast<DWORD>(pid), {}};
            EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&search));
            if (search.windows.empty()) return terminate(pid);
            // Make sure the process exists and may be touched before posting.
            Handle process = open_process(pid, PROCESS_TERMINATE);
            if (!process) return last_error_result();
            for (HWND hwnd : search.windows) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return PC_OK;
        }
        case kSigStop:
        case kSigCont: {
            const auto fn = signal == kSigStop ? ntdll().suspend_process : ntdll().resume_process;
            if (!fn) return PC_ERR_UNSUPPORTED;
            Handle process = open_process(pid, PROCESS_SUSPEND_RESUME);
            if (!process) return last_error_result();
            return nt_result(fn(process.get()));
        }
        default: return PC_ERR_INVALID;
    }
}

pc_result signal_process(int32_t pid, bool force) { return send_signal(pid, force ? kSigKill : kSigTerm); }

pc_result set_priority(int32_t pid, int32_t nice) {
    if (nice < -20 || nice > 20) return PC_ERR_INVALID;
    // Realtime is left out on purpose: it starves the system and needs a privilege anyway.
    // Each class covers the nice values nearest the one nice_of reports for it (-15, -8, 0, 8, 15), so
    // a value read back sets the same class, and macOS's steps (-10, -5, 0, 5, 10, 20) land on the
    // class of the same name: High, Above normal, Normal, Below normal, Low.
    DWORD priority_class = NORMAL_PRIORITY_CLASS;
    if (nice <= -10)
        priority_class = HIGH_PRIORITY_CLASS;
    else if (nice < 0)
        priority_class = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (nice == 0)
        priority_class = NORMAL_PRIORITY_CLASS;
    else if (nice < 10)
        priority_class = BELOW_NORMAL_PRIORITY_CLASS;
    else
        priority_class = IDLE_PRIORITY_CLASS;
    Handle process = open_process(pid, PROCESS_SET_INFORMATION);
    if (!process) return last_error_result();
    return SetPriorityClass(process.get(), priority_class) ? PC_OK : last_error_result();
}

pc_result set_affinity(int32_t pid, uint64_t mask) {
    if (mask == 0) return PC_ERR_INVALID;
    Handle process = open_process(pid, PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION);
    if (!process) return last_error_result();
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(process.get(), &process_mask, &system_mask)) return last_error_result();
    if ((mask & ~static_cast<uint64_t>(system_mask)) != 0) return PC_ERR_INVALID;
    return SetProcessAffinityMask(process.get(), static_cast<DWORD_PTR>(mask)) ? PC_OK : last_error_result();
}

// ---------------------------------------------------------------------------------------------
// Details: command line, working directory, environment, threads
// ---------------------------------------------------------------------------------------------

namespace {

constexpr int kProcessCommandLineInformation = 60;

struct RtlDriveLetterCurdir {
    USHORT Flags;
    USHORT Length;
    ULONG TimeStamp;
    STRING DosPath;
};

// RTL_USER_PROCESS_PARAMETERS of a 64-bit process, up to the environment block.
struct UserProcessParameters {
    ULONG MaximumLength;
    ULONG Length;
    ULONG Flags;
    ULONG DebugFlags;
    HANDLE ConsoleHandle;
    ULONG ConsoleFlags;
    HANDLE StandardInput;
    HANDLE StandardOutput;
    HANDLE StandardError;
    UNICODE_STRING CurrentDirectoryPath;
    HANDLE CurrentDirectoryHandle;
    UNICODE_STRING DllPath;
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
    PVOID Environment;
    ULONG StartingX;
    ULONG StartingY;
    ULONG CountX;
    ULONG CountY;
    ULONG CountCharsX;
    ULONG CountCharsY;
    ULONG FillAttribute;
    ULONG WindowFlags;
    ULONG ShowWindowFlags;
    UNICODE_STRING WindowTitle;
    UNICODE_STRING DesktopInfo;
    UNICODE_STRING ShellInfo;
    UNICODE_STRING RuntimeData;
    RtlDriveLetterCurdir CurrentDirectories[32];
    ULONG_PTR EnvironmentSize;
    ULONG_PTR EnvironmentVersion;
};

bool read_memory(HANDLE process, const void *address, void *out, size_t size) {
    SIZE_T read = 0;
    return address && ReadProcessMemory(process, address, out, size, &read) && read == size;
}

std::string read_remote_string(HANDLE process, const UNICODE_STRING &text) {
    if (!text.Buffer || text.Length == 0 || text.Length > 64 * 1024) return {};
    std::wstring buffer(text.Length / sizeof(wchar_t), L'\0');
    if (!read_memory(process, text.Buffer, buffer.data(), text.Length)) return {};
    return utf8(buffer);
}

void split_arguments(const std::wstring &command_line, Details &out) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(command_line.c_str(), &argc);
    if (!argv) return;
    for (int i = 0; i < argc; ++i) out.arguments.push_back(utf8(argv[i], wcslen(argv[i])));
    LocalFree(argv);
    out.arguments_known = true;
}

// The command line through the information class that works for every bitness (Windows 8.1+).
bool read_command_line(HANDLE process, Details &out) {
    if (!ntdll().query_process) return false;
    ULONG size = 0;
    ntdll().query_process(process, kProcessCommandLineInformation, nullptr, 0, &size);
    if (size == 0 || size > 1 << 20) return false;
    std::vector<char> buffer(size);
    if (ntdll().query_process(process, kProcessCommandLineInformation, buffer.data(), size, &size) < 0) return false;
    auto text = reinterpret_cast<const UNICODE_STRING *>(buffer.data());
    split_arguments(std::wstring(text->Buffer, text->Length / sizeof(wchar_t)), out);
    return out.arguments_known;
}

// RTL_USER_PROCESS_PARAMETERS of a 64-bit process through its PEB; false for 32-bit processes
// (another layout) and whenever the memory can't be read.
bool read_process_parameters(HANDLE process, UserProcessParameters &params) {
#if defined(_WIN64)
    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64) || wow64) return false;
    if (!ntdll().query_process) return false;
    PROCESS_BASIC_INFORMATION basic{};
    ULONG size = 0;
    if (ntdll().query_process(process, ProcessBasicInformation, &basic, sizeof(basic), &size) < 0) return false;
    PEB peb{};
    if (!read_memory(process, basic.PebBaseAddress, &peb, sizeof(peb))) return false;
    return read_memory(process, peb.ProcessParameters, &params, sizeof(params));
#else
    (void)process;
    (void)params;
    return false;
#endif
}

std::string current_directory(HANDLE process, const UserProcessParameters &params) {
    std::string cwd = read_remote_string(process, params.CurrentDirectoryPath);
    while (!cwd.empty() && cwd.back() == '\\' && cwd.size() > 3) cwd.pop_back();
    return cwd;
}

// Working directory and environment live in the PEB; only a 64-bit reader of a 64-bit process
// knows the layout.
void read_peb(HANDLE process, Details &out) {
#if defined(_WIN64)
    UserProcessParameters params{};
    if (!read_process_parameters(process, params)) return;
    out.cwd = current_directory(process, params);
    if (!out.arguments_known) {
        const std::string command = read_remote_string(process, params.CommandLine);
        if (!command.empty()) split_arguments(wide(command), out);
    }
    if (!params.Environment) return;
    size_t environment_size = params.EnvironmentSize;
    if (environment_size == 0 || environment_size > 2 << 20) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQueryEx(process, params.Environment, &region, sizeof(region)) != sizeof(region)) return;
        const auto base = static_cast<const char *>(region.BaseAddress);
        const auto start = static_cast<const char *>(params.Environment);
        environment_size = std::min<size_t>(region.RegionSize - static_cast<size_t>(start - base), 2 << 20);
    }
    std::wstring block(environment_size / sizeof(wchar_t), L'\0');
    if (!read_memory(process, params.Environment, block.data(), block.size() * sizeof(wchar_t))) return;
    size_t at = 0;
    while (at < block.size() && block[at] != L'\0') {
        const size_t end = block.find(L'\0', at);
        const std::wstring entry = block.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        if (entry.find(L'=') != 0) out.environment.push_back(utf8(entry));  // "=C:=..." drive entries are internal
        if (end == std::wstring::npos) break;
        at = end + 1;
    }
#else
    (void)process;
    (void)out;
#endif
}

int32_t thread_state(const ThreadEntry &t) {
    if (t.ThreadState == kThreadRunning) return PC_STATE_RUNNING;
    if (t.ThreadState == kThreadWaiting) return t.WaitReason == kWaitSuspended ? PC_STATE_STOPPED : PC_STATE_SLEEPING;
    return t.ThreadState == 1 || t.ThreadState == 3 ? PC_STATE_RUNNING : PC_STATE_UNKNOWN;  // ready, standby
}

std::string thread_name(DWORD thread_id) {
    using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static const auto fn = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    if (!fn) return {};
    Handle thread(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id));
    if (!thread) return {};
    PWSTR description = nullptr;
    if (FAILED(fn(thread.get(), &description)) || !description) return {};
    std::string name = utf8(description, wcslen(description));
    LocalFree(description);
    return name;
}

// Each thread's CPU time at the previous details read, by thread id and creation time (ids are
// reused): the next read turns the difference into a recent CPU share, as on macOS and Linux.
struct ThreadSample {
    int64_t created = 0;
    uint64_t cpu_ns = 0;
    double at = 0;
};
std::mutex thread_mutex;
std::unordered_map<uint64_t, ThreadSample> thread_samples;

void read_threads(int32_t pid, Details &out) {
    using namespace std::chrono;
    const double now = duration<double>(steady_clock::now().time_since_epoch()).count();
    std::lock_guard lock(thread_mutex);
    std::vector<char> buffer;
    if (!query_system(kSystemProcessInformation, buffer)) return;
    for (const ProcessEntry *entry : process_entries(buffer)) {
        if (static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry->UniqueProcessId)) != pid) continue;
        for (ULONG i = 0; i < entry->NumberOfThreads; ++i) {
            const ThreadEntry &t = entry->Threads[i];
            ThreadInfo thread;
            thread.id = reinterpret_cast<uintptr_t>(t.ClientId.UniqueThread);
            thread.name = thread_name(static_cast<DWORD>(thread.id));
            thread.cpu_percent = -1;
            thread.user_time_ns = static_cast<uint64_t>(t.UserTime.QuadPart) * 100;
            thread.system_time_ns = static_cast<uint64_t>(t.KernelTime.QuadPart) * 100;
            const uint64_t cpu = thread.user_time_ns + thread.system_time_ns;
            const auto previous = thread_samples.find(thread.id);
            if (previous != thread_samples.end() && previous->second.created == t.CreateTime.QuadPart &&
                now > previous->second.at && cpu >= previous->second.cpu_ns)
                thread.cpu_percent =
                    static_cast<double>(cpu - previous->second.cpu_ns) / 1e7 / (now - previous->second.at);
            thread_samples[thread.id] = {t.CreateTime.QuadPart, cpu, now};
            thread.priority = t.Priority;
            thread.state = thread_state(t);
            out.threads.push_back(std::move(thread));
        }
        if (thread_samples.size() > 20000) thread_samples.clear();
        out.threads_known = true;
        return;
    }
}

}  // namespace

bool process_details(int32_t pid, Details &out) {
    out = {};
    read_threads(pid, out);
    Handle process = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ);
    if (!process) process = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (process) {
        read_command_line(process.get(), out);
        read_peb(process.get(), out);
    }
    return out.complete();
}

std::string win::process_working_directory(int32_t pid) {
    Handle process = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ);
    if (!process) return {};
    UserProcessParameters params{};
    if (!read_process_parameters(process.get(), params)) return {};
    return current_directory(process.get(), params);
}

}  // namespace procyon::platform

#endif  // _WIN32
