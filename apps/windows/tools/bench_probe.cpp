// Launches Procyon on one screen, times its first window, then samples its CPU time and private
// working set (the "Memory" column of Task Manager). Prints one JSON object.
//   procyon-bench-probe <Procyon.exe> <page> <warmup seconds> <duration seconds>
// The page `tray` opens the window, closes it to the tray and samples Procyon running in the tray
// only. Alongside, it times a fixed piece of work once a second (`calibration_ns`): on a shared CI
// runner the same work takes longer when the host is busy or slower, which inflates Procyon's CPU
// too. Used by scripts/bench-windows.py; the counterpart of scripts/bench-probe.swift.
#include <windows.h>

#include <winternl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "commands.hpp"

namespace {

// SYSTEM_PROCESS_INFORMATION as the kernel fills it, up to the pid: winternl.h hides the private
// working set behind reserved fields.
struct ProcessEntry {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
};
constexpr NTSTATUS kInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);

const wchar_t *const kClassName = L"ProcyonMainWindow";

LARGE_INTEGER g_frequency{};
volatile uint64_t g_sink = 0;

double now() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(g_frequency.QuadPart);
}

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "bench-probe: %s\n", message);
    std::exit(1);
}

struct Usage {
    double cpu = 0;          // CPU seconds, user + kernel
    uint64_t footprint = 0;  // private working set
};

/// CPU seconds and private working set of the process.
bool usage(HANDLE process, DWORD pid, Usage &out) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return false;
    auto seconds = [](const FILETIME &ft) {
        return static_cast<double>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 1e7;
    };
    out.cpu = seconds(kernel) + seconds(user);

    static std::vector<char> buffer(1 << 20);
    ULONG needed = 0;
    NTSTATUS status =
        NtQuerySystemInformation(SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
    while (status == kInfoLengthMismatch) {
        buffer.resize(static_cast<size_t>(needed) + 65536);
        status = NtQuerySystemInformation(SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()),
                                          &needed);
    }
    if (status < 0) return false;
    size_t offset = 0;
    while (offset + sizeof(ProcessEntry) <= buffer.size()) {
        const auto *entry = reinterpret_cast<const ProcessEntry *>(buffer.data() + offset);
        if (reinterpret_cast<uintptr_t>(entry->UniqueProcessId) == pid) {
            out.footprint = static_cast<uint64_t>(entry->WorkingSetPrivateSize.QuadPart);
            return true;
        }
        if (entry->NextEntryOffset == 0) break;
        offset += entry->NextEntryOffset;
    }
    return false;
}

uint64_t work() {
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (uint64_t i = 0; i < 400000; ++i) x = (x ^ i) * 0xBF58476D1CE4E5B9ull + (x >> 31);
    return x;
}

/// Wall time of a fixed piece of register-only work (about a millisecond), the best of three:
/// preemption and a shared core only ever add to it. A few untimed rounds first: after a second
/// of sleep the core runs at its idle clock, and ramping up takes a few milliseconds, which would
/// otherwise read as a slow host.
uint64_t calibrate() {
    for (int round = 0; round < 8; ++round) g_sink = work();
    uint64_t best = UINT64_MAX;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const double start = now();
        g_sink = work();  // the volatile store keeps the loop from being optimized away
        const double elapsed = now() - start;
        best = std::min(best, static_cast<uint64_t>(elapsed * 1e9));
    }
    return best;
}

uint64_t median(std::vector<uint64_t> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

struct Search {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK find_main_window(HWND hwnd, LPARAM lparam) {
    auto *search = reinterpret_cast<Search *>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid) return TRUE;
    wchar_t name[64] = {};
    GetClassNameW(hwnd, name, 64);
    if (std::wcscmp(name, kClassName) != 0) return TRUE;
    search->found = hwnd;
    return FALSE;
}

HWND main_window(DWORD pid) {
    Search search{pid, nullptr};
    EnumWindows(&find_main_window, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

/// Whether the main window is on screen with a normal size.
bool shows_window(HWND hwnd) {
    RECT rect{};
    return hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetWindowRect(hwnd, &rect) &&
           rect.right - rect.left > 200 && rect.bottom - rect.top > 200;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: procyon-bench-probe <Procyon.exe> <page> <warmup s> <duration s>\n");
        return 2;
    }
    const std::wstring exe = argv[1];
    const std::wstring page = argv[2];
    const bool tray_only = page == L"tray";
    const double warmup = _wtof(argv[3]), duration = _wtof(argv[4]);
    QueryPerformanceFrequency(&g_frequency);

    // Calibrate before launching: a zero duration (startup only) has no sampling loop.
    std::vector<uint64_t> calibrations;
    for (int i = 0; i < 5; ++i) calibrations.push_back(calibrate());

    // The spec's 1 s refresh and close-to-tray, no settings read or written.
    std::wstring command = L"\"" + exe + L"\" --page " + (tray_only ? L"overview" : page) + L" --no-settings";
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process{};
    const double launched = now();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup_info,
                        &process))
        fail("can't start the app");
    CloseHandle(process.hThread);
    const DWORD pid = process.dwProcessId;

    HWND hwnd = nullptr;
    double startup = -1;
    while (now() - launched < 20) {
        hwnd = main_window(pid);
        if (shows_window(hwnd)) {
            startup = now() - launched;
            break;
        }
        if (WaitForSingleObject(process.hProcess, 5) == WAIT_OBJECT_0) fail("the app exited before showing a window");
    }
    if (tray_only) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);  // closes to the tray
        const double closed = now();
        while (IsWindowVisible(hwnd) && now() - closed < 10) Sleep(50);
        if (IsWindowVisible(hwnd)) {
            TerminateProcess(process.hProcess, 1);
            fail("the window didn't close to the tray");
        }
    }

    Sleep(static_cast<DWORD>(warmup * 1000));
    Usage first;
    if (!usage(process.hProcess, pid, first)) {
        TerminateProcess(process.hProcess, 1);
        fail("can't read the usage of the app");
    }
    const double start = now();
    std::vector<uint64_t> footprints;
    Usage last = first;
    while (now() - start < duration) {
        Sleep(1000);
        Usage sample;
        if (!usage(process.hProcess, pid, sample)) break;
        footprints.push_back(sample.footprint);
        last = sample;
        calibrations.push_back(calibrate());
    }
    const double elapsed = now() - start;

    // Quit through the app's own command (WM_CLOSE would only hide it to the tray).
    if (hwnd) PostMessageW(hwnd, WM_COMMAND, IDM_QUIT, 0);
    if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);

    // A zero duration only times the launch.
    const double cpu = footprints.empty() ? 0 : (last.cpu - first.cpu) / elapsed * 100;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    uint64_t total = 0;
    for (uint64_t f : footprints) total += f;
    std::vector<uint64_t> sorted = footprints;
    std::sort(sorted.begin(), sorted.end());
    const uint64_t average = footprints.empty() ? 0 : total / footprints.size();
    const uint64_t p90 = footprints.empty() ? 0 : sorted[(sorted.size() - 1) * 9 / 10];
    const uint64_t peak = footprints.empty() ? 0 : sorted.back();

    char startup_text[32] = "null";
    if (startup >= 0) std::snprintf(startup_text, sizeof(startup_text), "%.4f", startup);
    std::printf(
        "{\"page\":\"%ls\",\"startup_seconds\":%s,\"cpu_percent_of_core\":%.4f,"
        "\"cpu_percent_of_machine\":%.4f,\"memory_average_bytes\":%llu,\"memory_p90_bytes\":%llu,"
        "\"memory_peak_bytes\":%llu,\"calibration_ns\":%llu}\n",
        page.c_str(), startup_text, cpu, cpu / info.dwNumberOfProcessors, static_cast<unsigned long long>(average),
        static_cast<unsigned long long>(p90), static_cast<unsigned long long>(peak),
        static_cast<unsigned long long>(median(calibrations)));
    return 0;
}
