// Open files per process (the system handle table) and TCP/UDP sockets (the IP helper tables).
#if defined(_WIN32)

#include "windows_internal.hpp"

#include <winsock2.h>

#include <ws2ipdef.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <tcpmib.h>
#include <udpmib.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace procyon::platform {

using namespace win;

namespace {

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

struct HandleEntry {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

struct HandleInformation {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    HandleEntry Handles[1];
};

constexpr ULONG kObjectNameInformation = 1;

// The type index of file handles: found by looking our own file handle up in the table.
USHORT file_type_index(const std::vector<char> &table) {
    static USHORT cached = 0;
    if (cached) return cached;
    wchar_t windows_dir[MAX_PATH] = {};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    Handle probe(CreateFileW(windows_dir, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!probe) return 0;
    // The probe is not in `table` (it was read before): read a fresh table just once.
    std::vector<char> fresh;
    if (!query_system(kSystemExtendedHandleInformation, fresh)) return 0;
    auto info = reinterpret_cast<const HandleInformation *>(fresh.data());
    const ULONG_PTR self = GetCurrentProcessId();
    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const HandleEntry &e = info->Handles[i];
        if (e.UniqueProcessId == self && e.HandleValue == reinterpret_cast<ULONG_PTR>(probe.get())) {
            cached = e.ObjectTypeIndex;
            break;
        }
    }
    (void)table;
    return cached;
}

// "\Device\HarddiskVolume3" -> "C:" for every drive letter.
std::vector<std::pair<std::wstring, std::wstring>> device_prefixes() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t drive[] = {static_cast<wchar_t>(L'A' + i), L':', L'\0'};
        wchar_t target[512] = {};
        if (QueryDosDeviceW(drive, target, 512) > 0) out.emplace_back(target, drive);
    }
    // Longest prefixes first, so a mount point under another volume wins.
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
    return out;
}

std::string dos_path(const std::wstring &native, const std::vector<std::pair<std::wstring, std::wstring>> &prefixes) {
    for (const auto &[device, drive] : prefixes) {
        if (native.size() >= device.size() && _wcsnicmp(native.c_str(), device.c_str(), device.size()) == 0 &&
            (native.size() == device.size() || native[device.size()] == L'\\'))
            return utf8(drive + native.substr(device.size()));
    }
    return utf8(native);
}

// Named pipes and some devices block NtQueryObject when another thread has a synchronous
// operation pending: GetFileType never blocks and tells them apart.
bool nameable(HANDLE handle, int32_t &kind) {
    switch (GetFileType(handle)) {
        case FILE_TYPE_DISK: return true;
        case FILE_TYPE_PIPE:
        case FILE_TYPE_CHAR:
        default: kind = PC_FILE_OTHER; return false;
    }
}

std::wstring object_name(HANDLE handle) {
    if (!ntdll().query_object) return {};
    std::vector<char> buffer(1024);
    ULONG needed = 0;
    NTSTATUS status =
        ntdll().query_object(handle, kObjectNameInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
    if (static_cast<ULONG>(status) == 0xC0000004 && needed > 0 && needed < 64 * 1024) {  // STATUS_INFO_LENGTH_MISMATCH
        buffer.resize(needed);
        status = ntdll().query_object(handle, kObjectNameInformation, buffer.data(), static_cast<ULONG>(buffer.size()),
                                      &needed);
    }
    if (status < 0) return {};
    auto name = reinterpret_cast<const UNICODE_STRING *>(buffer.data());
    if (!name->Buffer || name->Length == 0) return {};
    return std::wstring(name->Buffer, name->Length / sizeof(wchar_t));
}

}  // namespace

bool handles_denied(int bytes, int error) {
    if (bytes > 0) return false;
    // Only a refusal hides descriptors that exist; a process that is gone has nothing to list.
    return error == ERROR_ACCESS_DENIED;
}

bool open_files(int32_t pid, std::vector<OpenFile> &out) {
    out.clear();
    std::vector<char> table;
    if (!query_system(kSystemExtendedHandleInformation, table)) return false;
    const USHORT file_type = file_type_index(table);
    if (!file_type) return false;
    const auto prefixes = device_prefixes();
    auto info = reinterpret_cast<const HandleInformation *>(table.data());

    bool complete = true;
    std::unordered_map<ULONG_PTR, Handle> processes;
    std::unordered_set<ULONG_PTR> denied;
    const ULONG_PTR self = GetCurrentProcessId();
    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const HandleEntry &e = info->Handles[i];
        if (e.ObjectTypeIndex != file_type) continue;
        if (pid >= 0 && e.UniqueProcessId != static_cast<ULONG_PTR>(pid)) continue;
        if (e.UniqueProcessId == 0 || e.UniqueProcessId == 4) continue;  // the kernel's own handles
        if (denied.count(e.UniqueProcessId)) continue;
        HANDLE source = nullptr;
        if (e.UniqueProcessId == self) {
            source = GetCurrentProcess();
        } else {
            auto it = processes.find(e.UniqueProcessId);
            if (it == processes.end()) {
                Handle opened = open_process(static_cast<int32_t>(e.UniqueProcessId), PROCESS_DUP_HANDLE);
                if (!opened) {
                    if (handles_denied(0, static_cast<int>(GetLastError()))) complete = false;
                    denied.insert(e.UniqueProcessId);
                    continue;
                }
                it = processes.emplace(e.UniqueProcessId, std::move(opened)).first;
            }
            source = it->second.get();
        }
        HANDLE raw = nullptr;
        if (!DuplicateHandle(source, reinterpret_cast<HANDLE>(e.HandleValue), GetCurrentProcess(), &raw, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            continue;
        Handle duplicate(raw);
        OpenFile file;
        file.pid = static_cast<int32_t>(e.UniqueProcessId);
        file.fd = static_cast<int32_t>(e.HandleValue);
        file.kind = PC_FILE_REGULAR;
        if (!nameable(duplicate.get(), file.kind)) continue;  // pipes, consoles: no path to show
        const std::wstring name = object_name(duplicate.get());
        if (name.empty() || name.rfind(L"\\Device\\", 0) != 0) continue;
        file.path = dos_path(name, prefixes);
        FILE_BASIC_INFO basic{};
        if (GetFileInformationByHandleEx(duplicate.get(), FileBasicInfo, &basic, sizeof(basic)) &&
            (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            file.kind = PC_FILE_DIRECTORY;
        out.push_back(std::move(file));
    }

    // Working directories keep a volume busy like an open file.
    std::vector<int32_t> targets;
    if (pid >= 0) {
        targets.push_back(pid);
    } else {
        std::vector<ProcessParent> links;
        process_parents(links);
        for (const auto &link : links)
            if (link.pid > 4) targets.push_back(link.pid);
    }
    for (int32_t target : targets) {
        // Only the PEB is read: process_details would also walk the process table for the
        // threads, once per process, which made a machine-wide listing quadratic.
        const std::string cwd = process_working_directory(target);
        if (cwd.empty()) continue;
        OpenFile file;
        file.pid = target;
        file.fd = -1;
        file.kind = PC_FILE_CWD;
        file.path = cwd;
        out.push_back(std::move(file));
    }
    return complete;
}

namespace {

int32_t tcp_state(DWORD state) {
    switch (state) {
        case MIB_TCP_STATE_LISTEN: return PC_TCP_LISTEN;
        case MIB_TCP_STATE_SYN_SENT: return PC_TCP_SYN_SENT;
        case MIB_TCP_STATE_SYN_RCVD: return PC_TCP_SYN_RECEIVED;
        case MIB_TCP_STATE_ESTAB: return PC_TCP_ESTABLISHED;
        case MIB_TCP_STATE_CLOSE_WAIT: return PC_TCP_CLOSE_WAIT;
        case MIB_TCP_STATE_FIN_WAIT1:
        case MIB_TCP_STATE_FIN_WAIT2:
        case MIB_TCP_STATE_CLOSING:
        case MIB_TCP_STATE_LAST_ACK: return PC_TCP_CLOSING;
        case MIB_TCP_STATE_TIME_WAIT: return PC_TCP_TIME_WAIT;
        case MIB_TCP_STATE_CLOSED:
        case MIB_TCP_STATE_DELETE_TCB: return PC_TCP_CLOSED;
        default: return PC_TCP_NONE;
    }
}

std::string address4(DWORD address) {
    if (address == 0) return "*";
    in_addr in{};
    in.s_addr = address;
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &in, text, sizeof(text));
    return text;
}

// An IPv6 table address as text. A dual-stack socket's IPv4 peers show up in the IPv6 table as
// IPv4-mapped addresses (::ffff:10.0.0.1): they are IPv4 connections and are reported as such,
// like macOS does, so `family` becomes 4. "*" for the unspecified address, like IPv4.
std::string address6(const UCHAR *bytes, int32_t &family) {
    in6_addr in{};
    std::memcpy(&in, bytes, sizeof(in));
    bool any = true;
    for (unsigned char b : in.s6_addr) any &= b == 0;
    if (any) return "*";
    if (IN6_IS_ADDR_V4MAPPED(&in)) {
        family = 4;
        in_addr v4{};
        std::memcpy(&v4, &in.s6_addr[12], sizeof(v4));
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &v4, text, sizeof(text));
        return text;
    }
    char text[INET6_ADDRSTRLEN] = {};
    inet_ntop(AF_INET6, &in, text, sizeof(text));
    return text;
}

template <typename Row>
bool wanted(int32_t pid, const Row &row) {
    return pid < 0 || static_cast<int32_t>(row.dwOwningPid) == pid;
}

template <typename Table>
bool fetch(Table *&table, std::vector<char> &buffer, ULONG family, bool tcp, int table_class) {
    DWORD size = 0;
    if (tcp)
        GetExtendedTcpTable(nullptr, &size, FALSE, family, static_cast<TCP_TABLE_CLASS>(table_class), 0);
    else
        GetExtendedUdpTable(nullptr, &size, FALSE, family, static_cast<UDP_TABLE_CLASS>(table_class), 0);
    if (size == 0) return false;
    buffer.resize(size + 4096);
    size = static_cast<DWORD>(buffer.size());
    const DWORD result =
        tcp ? GetExtendedTcpTable(buffer.data(), &size, FALSE, family, static_cast<TCP_TABLE_CLASS>(table_class), 0)
            : GetExtendedUdpTable(buffer.data(), &size, FALSE, family, static_cast<UDP_TABLE_CLASS>(table_class), 0);
    if (result != NO_ERROR) return false;
    table = reinterpret_cast<Table *>(buffer.data());
    return true;
}

}  // namespace

bool connections(int32_t pid, std::vector<pc_connection> &out) {
    out.clear();
    std::vector<char> buffer;
    MIB_TCPTABLE_OWNER_PID *tcp4 = nullptr;
    if (fetch(tcp4, buffer, AF_INET, true, TCP_TABLE_OWNER_PID_ALL)) {
        for (DWORD i = 0; i < tcp4->dwNumEntries; ++i) {
            const MIB_TCPROW_OWNER_PID &row = tcp4->table[i];
            if (!wanted(pid, row)) continue;
            pc_connection c{};
            c.pid = static_cast<int32_t>(row.dwOwningPid);
            c.protocol = PC_PROTOCOL_TCP;
            c.family = 4;
            c.state = tcp_state(row.dwState);
            c.local_port = static_cast<int32_t>(ntohs(static_cast<u_short>(row.dwLocalPort)));
            copy_string(c.local_address, sizeof(c.local_address), address4(row.dwLocalAddr));
            if (c.state != PC_TCP_LISTEN) {
                c.remote_port = static_cast<int32_t>(ntohs(static_cast<u_short>(row.dwRemotePort)));
                copy_string(c.remote_address, sizeof(c.remote_address), address4(row.dwRemoteAddr));
            }
            out.push_back(c);
        }
    }
    MIB_TCP6TABLE_OWNER_PID *tcp6 = nullptr;
    if (fetch(tcp6, buffer, AF_INET6, true, TCP_TABLE_OWNER_PID_ALL)) {
        for (DWORD i = 0; i < tcp6->dwNumEntries; ++i) {
            const MIB_TCP6ROW_OWNER_PID &row = tcp6->table[i];
            if (!wanted(pid, row)) continue;
            pc_connection c{};
            c.pid = static_cast<int32_t>(row.dwOwningPid);
            c.protocol = PC_PROTOCOL_TCP;
            c.family = 6;
            c.state = tcp_state(row.dwState);
            c.local_port = static_cast<int32_t>(ntohs(static_cast<u_short>(row.dwLocalPort)));
            copy_string(c.local_address, sizeof(c.local_address), address6(row.ucLocalAddr, c.family));
            if (c.state != PC_TCP_LISTEN) {
                c.remote_port = static_cast<int32_t>(ntohs(static_cast<u_short>(row.dwRemotePort)));
                copy_string(c.remote_address, sizeof(c.remote_address), address6(row.ucRemoteAddr, c.family));
            }
            out.push_back(c);
        }
    }
    MIB_UDPTABLE_OWNER_PID *udp4 = nullptr;
    if (fetch(udp4, buffer, AF_INET, false, UDP_TABLE_OWNER_PID)) {
        for (DWORD i = 0; i < udp4->dwNumEntries; ++i) {
            const MIB_UDPROW_OWNER_PID &row = udp4->table[i];
            if (!wanted(pid, row)) continue;
            pc_connection c{};
            c.pid = static_cast<int32_t>(row.dwOwningPid);
            c.protocol = PC_PROTOCOL_UDP;
            c.family = 4;
            c.state = PC_TCP_NONE;
            c.local_port = static_cast<int32_t>(ntohs(static_cast<u_short>(row.dwLocalPort)));
            copy_string(c.local_address, sizeof(c.local_address), address4(row.dwLocalAddr));
            out.push_back(c);
        }
    }
    MIB_UDP6TABLE_OWNER_PID *udp6 = nullptr;
    if (fetch(udp6, buffer, AF_INET6, false, UDP_TABLE_OWNER_PID)) {
        for (DWORD i = 0; i < udp6->dwNumEntries; ++i) {
            const MIB_UDP6ROW_OWNER_PID &row = udp6->table[i];
            if (!wanted(pid, row)) continue;
            pc_connection c{};
            c.pid = static_cast<int32_t>(row.dwOwningPid);
            c.protocol = PC_PROTOCOL_UDP;
            c.family = 6;
            c.state = PC_TCP_NONE;
            c.local_port = static_cast<int32_t>(ntohs(static_cast<u_short>(row.dwLocalPort)));
            copy_string(c.local_address, sizeof(c.local_address), address6(row.ucLocalAddr, c.family));
            out.push_back(c);
        }
    }
    // The tables list every process's sockets without privileges.
    return true;
}

}  // namespace procyon::platform

#endif  // _WIN32
