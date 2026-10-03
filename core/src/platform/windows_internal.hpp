// Shared by the Windows adapter files: the native API declarations the SDK leaves out, string
// conversion, handle ownership and the process table walk.
#pragma once
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winternl.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../platform.hpp"

namespace procyon::platform::win {

// ---- native API (ntdll) ----

// SYSTEM_PROCESS_INFORMATION as the kernel fills it; winternl.h only names a few fields.
struct ThreadEntry {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    CLIENT_ID ClientId;
    LONG Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
};

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
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    ThreadEntry Threads[1];
};

// Thread states and wait reasons (KTHREAD_STATE / KWAIT_REASON).
constexpr ULONG kThreadRunning = 2;
constexpr ULONG kThreadWaiting = 5;
constexpr ULONG kWaitSuspended = 5;

constexpr int kSystemProcessInformation = 5;
constexpr int kSystemProcessorPerformanceInformation = 8;
constexpr int kSystemPagefileInformation = 18;
constexpr int kSystemExtendedHandleInformation = 64;
constexpr int kSystemMemoryListInformation = 80;
constexpr int kProcessBreakOnTermination = 29;

using NtQuerySystemInformationFn = NTSTATUS(NTAPI *)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationProcessFn = NTSTATUS(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtQueryObjectFn = NTSTATUS(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtSuspendResumeProcessFn = NTSTATUS(NTAPI *)(HANDLE);
using RtlGetVersionFn = NTSTATUS(NTAPI *)(PRTL_OSVERSIONINFOW);

struct Ntdll {
    NtQuerySystemInformationFn query_system = nullptr;
    NtQueryInformationProcessFn query_process = nullptr;
    NtQueryObjectFn query_object = nullptr;
    NtSuspendResumeProcessFn suspend_process = nullptr;
    NtSuspendResumeProcessFn resume_process = nullptr;
    RtlGetVersionFn get_version = nullptr;
};
const Ntdll &ntdll();

// Reads one information class into a growing buffer; false when the call fails.
bool query_system(int information_class, std::vector<char> &buffer);
// Every process entry of a SystemProcessInformation buffer, in table order.
std::vector<const ProcessEntry *> process_entries(const std::vector<char> &buffer);

// ---- handles and strings ----

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : h_(other.release()) {}
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other) {
            reset();
            h_ = other.release();
        }
        return *this;
    }
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return valid(); }
    HANDLE release() {
        HANDLE h = h_;
        h_ = nullptr;
        return h;
    }
    void reset() {
        if (valid()) CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

std::string utf8(const wchar_t *text, size_t length);
std::string utf8(const std::wstring &text);
std::string utf8(const UNICODE_STRING &text);
std::wstring wide(const std::string &text);
std::string lower_ascii(std::string text);
// "C:\Program Files\App\app.exe" -> "app.exe"
std::string basename_of(const std::string &path);

// FILETIME (100 ns since 1601) -> unix seconds.
int64_t filetime_to_unix(uint64_t filetime);
inline uint64_t filetime_value(const FILETIME &ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

pc_result error_result(DWORD error);
inline pc_result last_error_result() { return error_result(GetLastError()); }

// Opens `pid` with the least access that still answers the question.
Handle open_process(int32_t pid, DWORD access);
// The executable path behind a process handle, empty when the OS won't say.
std::string image_path(HANDLE process);
// The security identifier of the process's user as a uid of our own numbering (see users.cpp part
// of windows.cpp): 0 SYSTEM, 1 LOCAL SERVICE, 2 NETWORK SERVICE, 3 Window Manager, 4 Font Driver
// Host, 1000+ everyone else; kUnknownUser when the token can't be read.
constexpr uint32_t kUnknownUser = 0xFFFFFFFFu;
constexpr uint32_t kFirstRegularUser = 1000;
uint32_t process_user(HANDLE process);

// Registry string value, empty when missing.
std::string registry_string(HKEY root, const wchar_t *key, const wchar_t *value);
uint32_t registry_dword(HKEY root, const wchar_t *key, const wchar_t *value, uint32_t fallback = 0);

// The running process whose executable is `path` (case-insensitive), 0 when none. Read from the
// last process table walk, so it costs nothing.
int32_t pid_of_executable(const std::string &path);

// Whether this process runs with administrator rights (an elevated token).
bool is_elevated();

// Expands %SystemRoot% style references and resolves the executable in a command line such as
// `"C:\Program Files\App\app.exe" --flag` or `C:\Windows\system32\rundll32.exe foo`.
std::string executable_of_command(const std::string &command);

// FileDescription of an executable ("Google Chrome"), what Task Manager shows as the name; the
// file name without ".exe" when the resource has none. Cached per path.
std::string product_name(const std::string &path);

// The Windows directory, lower-case ("c:\windows"), and whether `path` lies under it.
std::string windows_directory();
bool under_windows_directory(const std::string &path);

// The working directory of `pid` from its PEB (64-bit processes), empty when unreadable. Opens one
// handle and reads two structures: cheap enough for every process.
std::string process_working_directory(int32_t pid);

}  // namespace procyon::platform::win

#endif  // _WIN32
