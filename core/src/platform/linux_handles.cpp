// Open files and sockets per process: /proc/<pid>/fd links (like lsof) and the kernel's socket
// tables in /proc/net, matched by socket inode. Another user's descriptors are hidden without root.
#if defined(__linux__)

#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>
#include <unordered_map>

#include "linux_internal.hpp"

namespace procyon::platform {

using namespace linux_internal;

namespace {

std::vector<int32_t> target_pids(int32_t pid) { return pid < 0 ? list_pids() : std::vector<int32_t>{pid}; }

enum class Descriptors { Listed, None, Denied };

// The descriptor numbers and link targets of one process.
Descriptors read_descriptors(int32_t pid, std::vector<std::pair<int32_t, std::string>> &out) {
    out.clear();
    const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
    errno = 0;
    const auto names = list_dir(dir);
    if (names.empty()) return handles_denied(0, errno) ? Descriptors::Denied : Descriptors::None;
    for (const auto &name : names) {
        const std::string target = read_link(dir + "/" + name);
        if (!target.empty()) out.emplace_back(std::atoi(name.c_str()), target);
    }
    return Descriptors::Listed;
}

bool kernel_thread(int32_t pid) {
    ProcStat stat;
    return read_stat(pid, stat) && (stat.kernel_thread() || pid == 2);
}

int32_t file_kind(const std::string &fd_path) {
    struct stat info{};
    if (::stat(fd_path.c_str(), &info) != 0) return PC_FILE_REGULAR;  // deleted or gone: it was a file
    if (S_ISREG(info.st_mode)) return PC_FILE_REGULAR;
    if (S_ISDIR(info.st_mode)) return PC_FILE_DIRECTORY;
    return PC_FILE_OTHER;
}

int32_t tcp_state(int state) {
    switch (state) {
        case 0x01: return PC_TCP_ESTABLISHED;
        case 0x02: return PC_TCP_SYN_SENT;
        case 0x03: return PC_TCP_SYN_RECEIVED;
        case 0x04:
        case 0x05:
        case 0x09:
        case 0x0B: return PC_TCP_CLOSING;  // FIN_WAIT1/2, LAST_ACK, CLOSING
        case 0x06: return PC_TCP_TIME_WAIT;
        case 0x08: return PC_TCP_CLOSE_WAIT;
        case 0x0A: return PC_TCP_LISTEN;
        default: return PC_TCP_CLOSED;
    }
}

}  // namespace

bool handles_denied(int bytes, int error) {
    if (bytes > 0) return false;
    // A process that is gone reports ENOENT/ESRCH, one with nothing open an empty directory; only a
    // refusal hides descriptors that exist.
    return error == EPERM || error == EACCES;
}

bool open_files(int32_t pid, std::vector<OpenFile> &out) {
    out.clear();
    bool complete = true;
    std::vector<std::pair<int32_t, std::string>> fds;
    for (int32_t target : target_pids(pid)) {
        if (kernel_thread(target)) continue;
        const std::string base = "/proc/" + std::to_string(target);
        const Descriptors listed = read_descriptors(target, fds);
        if (listed == Descriptors::Denied) {
            complete = false;
            continue;
        }
        const std::string cwd = read_link(base + "/cwd");
        if (!cwd.empty()) out.push_back({target, -1, PC_FILE_CWD, cwd});
        for (const auto &[fd, path] : fds) {
            // Sockets, pipes and anonymous inodes ("socket:[123]", "anon_inode:[eventfd]") aren't files.
            if (path.empty() || path[0] != '/') continue;
            out.push_back({target, fd, file_kind(base + "/fd/" + std::to_string(fd)), path});
        }
    }
    return complete;
}

std::vector<SocketEntry> parse_proc_net(const std::string &text, int32_t protocol, int32_t family) {
    std::vector<SocketEntry> entries;
    bool header = true;
    for (const auto &line : split(text, '\n')) {
        if (header) {  // "  sl  local_address rem_address   st tx_queue rx_queue ..."
            header = false;
            continue;
        }
        const auto f = split_whitespace(line);
        if (f.size() < 10) continue;
        auto endpoint = [&](const std::string &field, std::string &address, int32_t &port) {
            const auto colon = field.find(':');
            if (colon == std::string::npos) return false;
            const std::string hex = field.substr(0, colon);
            port = static_cast<int32_t>(std::strtol(field.c_str() + colon + 1, nullptr, 16));
            char text_address[INET6_ADDRSTRLEN] = {};
            if (family == 4 && hex.size() == 8) {
                // One 32-bit word in host byte order.
                in_addr a{};
                a.s_addr = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
                inet_ntop(AF_INET, &a, text_address, sizeof(text_address));
                address = a.s_addr == 0 ? "*" : text_address;
            } else if (family == 6 && hex.size() == 32) {
                // Four 32-bit words, each in host byte order.
                in6_addr a{};
                for (int w = 0; w < 4; ++w) {
                    const uint32_t word =
                        static_cast<uint32_t>(std::strtoul(hex.substr(w * 8, 8).c_str(), nullptr, 16));
                    std::memcpy(&a.s6_addr[w * 4], &word, 4);
                }
                inet_ntop(AF_INET6, &a, text_address, sizeof(text_address));
                address = IN6_IS_ADDR_UNSPECIFIED(&a) ? "*" : text_address;
            } else {
                return false;
            }
            return true;
        };
        SocketEntry e;
        e.connection.protocol = protocol;
        e.connection.family = family;
        std::string local, remote;
        int32_t local_port = 0, remote_port = 0;
        if (!endpoint(f[1], local, local_port) || !endpoint(f[2], remote, remote_port)) continue;
        const int state = static_cast<int>(std::strtol(f[3].c_str(), nullptr, 16));
        e.connection.state = protocol == PC_PROTOCOL_TCP ? tcp_state(state) : PC_TCP_NONE;
        e.connection.local_port = local_port;
        copy_string(e.connection.local_address, sizeof(e.connection.local_address), local);
        if (remote_port != 0 || remote != "*") {
            e.connection.remote_port = remote_port;
            copy_string(e.connection.remote_address, sizeof(e.connection.remote_address), remote);
        }
        e.inode = std::strtoull(f[9].c_str(), nullptr, 10);
        entries.push_back(e);
    }
    return entries;
}

bool connections(int32_t pid, std::vector<pc_connection> &out) {
    out.clear();
    // Socket inode → owning pids, from every readable descriptor table.
    std::unordered_map<uint64_t, std::vector<int32_t>> owners;
    bool complete = true;
    std::vector<std::pair<int32_t, std::string>> fds;
    for (int32_t target : target_pids(pid)) {
        if (kernel_thread(target)) continue;
        if (read_descriptors(target, fds) == Descriptors::Denied) {
            complete = false;
            continue;
        }
        for (const auto &[fd, path] : fds) {
            if (!starts_with(path, "socket:[")) continue;
            owners[std::strtoull(path.c_str() + 8, nullptr, 10)].push_back(target);
        }
    }
    if (owners.empty()) return complete;

    std::set<std::tuple<int32_t, int32_t, int32_t, std::string, int32_t, std::string, int32_t>> seen;
    const struct {
        const char *path;
        int32_t protocol, family;
    } tables[] = {{"/proc/net/tcp", PC_PROTOCOL_TCP, 4},
                  {"/proc/net/tcp6", PC_PROTOCOL_TCP, 6},
                  {"/proc/net/udp", PC_PROTOCOL_UDP, 4},
                  {"/proc/net/udp6", PC_PROTOCOL_UDP, 6}};
    std::string text;
    for (const auto &table : tables) {
        if (!read_file(table.path, text)) continue;
        for (const auto &entry : parse_proc_net(text, table.protocol, table.family)) {
            auto found = owners.find(entry.inode);
            if (found == owners.end()) continue;
            for (int32_t owner : found->second) {
                pc_connection c = entry.connection;
                c.pid = owner;
                // A socket shared by a parent and its children (or one process's two descriptors)
                // is one endpoint pair per process.
                if (!seen.insert({c.pid, c.protocol, c.family, c.local_address, c.local_port, c.remote_address,
                                  c.remote_port})
                         .second)
                    continue;
                out.push_back(c);
            }
        }
    }
    return complete;
}

}  // namespace procyon::platform

#endif  // __linux__
