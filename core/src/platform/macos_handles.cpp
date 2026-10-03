// macOS open files and sockets per process, through libproc (what lsof reads).
#if defined(__APPLE__)

#include <arpa/inet.h>
#include <libproc.h>
#include <netinet/in.h>
#include <sys/proc_info.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>

#include "../platform.hpp"

namespace procyon::platform {
namespace {

std::vector<int32_t> target_pids(int32_t pid) {
    if (pid >= 0) return {pid};
    std::vector<int32_t> pids(4096);
    for (int attempt = 0;; ++attempt) {
        const int count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(int32_t)));
        if (count < 0) return {};
        // A full buffer may have cut the list short; retry larger a few times, then take what fits.
        if (static_cast<size_t>(count) < pids.size() || attempt == 2) {
            pids.resize(static_cast<size_t>(count));
            return pids;
        }
        pids.resize(pids.size() * 2);
    }
}

enum class Descriptors {
    Listed,  // `out` holds them
    None,    // the process is gone, a zombie, or has nothing open: nothing to list
    Denied,  // another user's process, hidden from us: the listing is incomplete
};

Descriptors descriptors(int32_t pid, std::vector<proc_fdinfo> &out) {
    out.clear();
    errno = 0;
    const int size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (size <= 0) return handles_denied(size, errno) ? Descriptors::Denied : Descriptors::None;
    // Headroom: descriptors open between the two calls.
    out.resize(static_cast<size_t>(size) / sizeof(proc_fdinfo) + 32);
    errno = 0;
    const int filled =
        proc_pidinfo(pid, PROC_PIDLISTFDS, 0, out.data(), static_cast<int>(out.size() * sizeof(proc_fdinfo)));
    if (filled <= 0) {
        out.clear();
        return handles_denied(filled, errno) ? Descriptors::Denied : Descriptors::None;
    }
    out.resize(static_cast<size_t>(filled) / sizeof(proc_fdinfo));
    return Descriptors::Listed;
}

int32_t file_kind(uint32_t mode) {
    if (S_ISREG(mode)) return PC_FILE_REGULAR;
    if (S_ISDIR(mode)) return PC_FILE_DIRECTORY;
    return PC_FILE_OTHER;
}

int32_t tcp_state(int state) {
    switch (state) {
        case TSI_S_LISTEN: return PC_TCP_LISTEN;
        case TSI_S_SYN_SENT: return PC_TCP_SYN_SENT;
        case TSI_S_SYN_RECEIVED: return PC_TCP_SYN_RECEIVED;
        case TSI_S_ESTABLISHED: return PC_TCP_ESTABLISHED;
        case TSI_S__CLOSE_WAIT: return PC_TCP_CLOSE_WAIT;
        case TSI_S_FIN_WAIT_1:
        case TSI_S_CLOSING:
        case TSI_S_LAST_ACK:
        case TSI_S_FIN_WAIT_2: return PC_TCP_CLOSING;
        case TSI_S_TIME_WAIT: return PC_TCP_TIME_WAIT;
        default: return PC_TCP_CLOSED;
    }
}

// "*" for the unspecified local address, empty for an unconnected peer, else the address as text
// (IPv4-mapped IPv6 shown as IPv4).
void format_address(bool ipv4, const in_addr &v4, const in6_addr &v6, bool local, char *out, size_t capacity) {
    char text[INET6_ADDRSTRLEN] = {};
    if (ipv4 ? v4.s_addr == INADDR_ANY : IN6_IS_ADDR_UNSPECIFIED(&v6))
        (void)std::snprintf(text, sizeof(text), "%s", local ? "*" : "");
    else if (ipv4)
        inet_ntop(AF_INET, &v4, text, sizeof(text));
    else if (IN6_IS_ADDR_V4MAPPED(&v6))
        inet_ntop(AF_INET, &v6.s6_addr[12], text, sizeof(text));
    else
        inet_ntop(AF_INET6, &v6, text, sizeof(text));
    (void)std::snprintf(out, capacity, "%s", text);
}

}  // namespace

bool handles_denied(int bytes, int error) {
    if (bytes > 0) return false;
    // libproc reports a missing or exiting process as ESRCH and an empty table as 0 bytes with no
    // error; only a refusal hides descriptors that exist.
    return error == EPERM || error == EACCES;
}

bool open_files(int32_t pid, std::vector<OpenFile> &out) {
    out.clear();
    bool complete = true;
    std::vector<proc_fdinfo> fds;
    for (int32_t target : target_pids(pid)) {
        if (target == 0) continue;  // kernel_task has no descriptors to show
        proc_vnodepathinfo cwd{};
        if (proc_pidinfo(target, PROC_PIDVNODEPATHINFO, 0, &cwd, sizeof(cwd)) == sizeof(cwd) &&
            cwd.pvi_cdir.vip_path[0] != '\0')
            out.push_back({target, -1, PC_FILE_CWD, cwd.pvi_cdir.vip_path});
        if (descriptors(target, fds) == Descriptors::Denied) complete = false;
        for (const auto &fd : fds) {
            if (fd.proc_fdtype != PROX_FDTYPE_VNODE) continue;
            vnode_fdinfowithpath info{};
            if (proc_pidfdinfo(target, fd.proc_fd, PROC_PIDFDVNODEPATHINFO, &info, sizeof(info)) != sizeof(info))
                continue;
            if (info.pvip.vip_path[0] == '\0') continue;
            out.push_back({target, fd.proc_fd, file_kind(info.pvip.vip_vi.vi_stat.vst_mode), info.pvip.vip_path});
        }
    }
    return complete;
}

bool connections(int32_t pid, std::vector<pc_connection> &out) {
    out.clear();
    bool complete = true;
    std::vector<proc_fdinfo> fds;
    // A socket shared by several descriptors (dup, fork) is listed once per process.
    std::set<std::tuple<int32_t, int32_t, std::string, int32_t, std::string, int32_t, int32_t>> seen;
    for (int32_t target : target_pids(pid)) {
        if (target == 0) continue;
        if (descriptors(target, fds) == Descriptors::Denied) complete = false;
        for (const auto &fd : fds) {
            if (fd.proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            socket_fdinfo info{};
            if (proc_pidfdinfo(target, fd.proc_fd, PROC_PIDFDSOCKETINFO, &info, sizeof(info)) != sizeof(info)) continue;
            const auto &socket = info.psi;
            if (socket.soi_family != AF_INET && socket.soi_family != AF_INET6) continue;
            pc_connection c{};
            c.pid = target;
            const in_sockinfo *in = nullptr;
            if (socket.soi_kind == SOCKINFO_TCP) {
                in = &socket.soi_proto.pri_tcp.tcpsi_ini;
                c.protocol = PC_PROTOCOL_TCP;
                c.state = tcp_state(socket.soi_proto.pri_tcp.tcpsi_state);
            } else if (socket.soi_kind == SOCKINFO_IN && socket.soi_type == SOCK_DGRAM) {
                in = &socket.soi_proto.pri_in;
                c.protocol = PC_PROTOCOL_UDP;
                c.state = PC_TCP_NONE;
            } else {
                continue;
            }
            const bool mapped = (in->insi_vflag & INI_IPV6) && IN6_IS_ADDR_V4MAPPED(&in->insi_laddr.ina_6);
            c.family = (in->insi_vflag & INI_IPV4) || mapped ? 4 : 6;
            c.local_port = ntohs(static_cast<uint16_t>(in->insi_lport));
            c.remote_port = ntohs(static_cast<uint16_t>(in->insi_fport));
            const bool ipv4 = (in->insi_vflag & INI_IPV4) != 0;
            format_address(ipv4, in->insi_laddr.ina_46.i46a_addr4, in->insi_laddr.ina_6, true, c.local_address,
                           sizeof(c.local_address));
            format_address(ipv4, in->insi_faddr.ina_46.i46a_addr4, in->insi_faddr.ina_6, false, c.remote_address,
                           sizeof(c.remote_address));
            if (!seen.emplace(c.pid, c.protocol, c.local_address, c.local_port, c.remote_address, c.remote_port,
                              c.state)
                     .second)
                continue;
            out.push_back(c);
        }
    }
    return complete;
}

}  // namespace procyon::platform

#endif
