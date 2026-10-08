// Per-process network on Linux: the kernel's TCP byte counters per socket (sock_diag netlink,
// tcp_info.tcpi_bytes_received / tcpi_bytes_acked, Linux 4.2+), readable without privileges, matched
// to processes by socket inode through /proc/<pid>/fd. Only descriptors Procyon may read are matched:
// its own user's processes, every process when it runs as root. UDP sockets have no byte counters,
// so QUIC and other UDP traffic is not attributed.
#if defined(__linux__)

#include <arpa/inet.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "linux_internal.hpp"

namespace procyon::platform {

using namespace linux_internal;

namespace {

struct SocketBytes {
    uint64_t rx = 0, tx = 0;
};

bool loopback(int family, const __be32 *address) {
    if (family == AF_INET) return (ntohl(address[0]) >> 24) == 127;
    // ::1, or an IPv4-mapped loopback (::ffff:127.x.x.x).
    if (address[0] == 0 && address[1] == 0 && address[2] == 0 && ntohl(address[3]) == 1) return true;
    return address[0] == 0 && address[1] == 0 && ntohl(address[2]) == 0xFFFF && (ntohl(address[3]) >> 24) == 127;
}

// A socket as the kernel identifies it: enough to ask for that one socket again.
struct SocketId {
    uint8_t family = 0;
    inet_diag_sockid id{};
};

class DiagSocket {
public:
    DiagSocket() {
        fd_ = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
        if (fd_ >= 0) {
            const timeval timeout{0, 200000};  // a reply never takes this long; never hang the sampler
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        }
    }
    ~DiagSocket() {
        if (fd_ >= 0) ::close(fd_);
    }
    DiagSocket(const DiagSocket &) = delete;
    DiagSocket &operator=(const DiagSocket &) = delete;
    int fd() const { return fd_; }

private:
    int fd_ = -1;
};

using Request = std::pair<nlmsghdr, inet_diag_req_v2>;

Request make_request(uint8_t family, const inet_diag_sockid *id) {
    Request r{};
    r.first.nlmsg_len = sizeof(Request);
    r.first.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    r.first.nlmsg_flags = NLM_F_REQUEST | (id ? 0 : NLM_F_DUMP);
    r.second.sdiag_family = family;
    r.second.sdiag_protocol = IPPROTO_TCP;
    r.second.idiag_states = ~0u;
    r.second.idiag_ext = 1 << (INET_DIAG_INFO - 1);
    if (id) r.second.id = *id;
    return r;
}

// Reads replies until `expected` answers (data or error) arrived, or NLMSG_DONE for a dump.
// Sockets talking only to loopback are skipped, like the machine's own totals skip `lo`.
bool read_replies(int fd, size_t expected, bool dump, std::unordered_map<uint64_t, SocketBytes> &out,
                  std::unordered_map<uint64_t, SocketId> *ids) {
    alignas(nlmsghdr) char buffer[32768];
    size_t answered = 0;
    while (dump || answered < expected) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);  // NLMSG_NEXT counts it down
        if (n <= 0) return false;
        for (auto *h = reinterpret_cast<nlmsghdr *>(buffer); NLMSG_OK(h, static_cast<unsigned>(n));
             h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) return true;
            ++answered;
            if (h->nlmsg_type == NLMSG_ERROR) {
                if (dump) return false;
                continue;  // that socket closed since the last dump
            }
            const auto *diag = static_cast<const inet_diag_msg *>(NLMSG_DATA(h));
            if (loopback(diag->idiag_family, diag->id.idiag_src) && loopback(diag->idiag_family, diag->id.idiag_dst))
                continue;
            int length = static_cast<int>(h->nlmsg_len - NLMSG_LENGTH(sizeof(*diag)));
            for (auto *a = reinterpret_cast<const rtattr *>(diag + 1); RTA_OK(a, length); a = RTA_NEXT(a, length)) {
                if (a->rta_type != INET_DIAG_INFO) continue;
                // Older kernels send a shorter tcp_info: read only what is there.
                const size_t size = RTA_PAYLOAD(a);
                if (size < offsetof(tcp_info, tcpi_bytes_received) + sizeof(uint64_t)) continue;
                tcp_info info{};
                std::memcpy(&info, RTA_DATA(a), std::min(size, sizeof(info)));
                out[diag->idiag_inode] = {info.tcpi_bytes_received, info.tcpi_bytes_acked};
                if (ids) (*ids)[diag->idiag_inode] = {diag->idiag_family, diag->id};
            }
        }
    }
    return true;
}

// Every TCP socket with its counters. The kernel walks its whole established hash for a dump (a few
// milliseconds per family on a machine with a large table), so this runs every 3 seconds only: a new
// connection is counted from the dump that finds it, so the first seconds of a short one are missed.
bool dump_tcp(std::unordered_map<uint64_t, SocketBytes> &out, std::unordered_map<uint64_t, SocketId> &ids) {
    DiagSocket socket;
    if (socket.fd() < 0) return false;
    for (uint8_t family : {uint8_t{AF_INET}, uint8_t{AF_INET6}}) {
        const Request request = make_request(family, nullptr);
        if (::send(socket.fd(), &request, sizeof(request), 0) < 0) return false;
        if (!read_replies(socket.fd(), 0, true, out, &ids)) return false;
    }
    return true;
}

// The counters of known sockets, each looked up by its id: a hash lookup per socket in the kernel,
// a batch of requests per send.
bool query_tcp(const std::vector<SocketId> &sockets, std::unordered_map<uint64_t, SocketBytes> &out) {
    DiagSocket socket;
    if (socket.fd() < 0) return false;
    constexpr size_t kBatch = 128;
    std::vector<Request> batch;
    for (size_t first = 0; first < sockets.size(); first += kBatch) {
        batch.clear();
        for (size_t i = first; i < std::min(sockets.size(), first + kBatch); ++i)
            batch.push_back(make_request(sockets[i].family, &sockets[i].id));
        if (::send(socket.fd(), batch.data(), batch.size() * sizeof(Request), 0) < 0) return false;
        if (!read_replies(socket.fd(), batch.size(), false, out, nullptr)) return false;
    }
    return true;
}

struct State {
    std::mutex mutex;
    // Which process owns a socket, from the last descriptor scan.
    std::unordered_map<uint64_t, uint64_t> owner;  // inode → process key
    // Sockets the last scan found no readable owner for (other users', the kernel's): a new scan
    // wouldn't find one either, so they don't trigger it.
    std::unordered_set<uint64_t> unowned;
    double scanned = 0;
    // The sockets of readable processes from the last dump, to look up one by one in between.
    std::unordered_map<uint64_t, SocketId> ids;
    double dumped = 0;
    // The counters of each socket at the previous tick, and what each process has moved since it
    // was first seen: summing deltas keeps a process's total from dropping when a socket closes.
    std::unordered_map<uint64_t, SocketBytes> previous;
    std::unordered_map<uint64_t, SocketBytes> totals;  // process key → bytes
};

State &state() {
    static State s;
    return s;
}

uint64_t process_key(int32_t pid, int64_t start_time) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(pid)) << 32) ^ static_cast<uint64_t>(start_time);
}

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Socket inodes of every readable process. Walking every descriptor is the expensive part, so it runs
// only when sockets of unknown owner appeared, and at most every two seconds.
void scan_owners(const std::vector<RawProcess> &processes, std::unordered_map<uint64_t, uint64_t> &owner) {
    owner.clear();
    for (const auto &p : processes) {
        if (!p.has_disk_io) continue;  // another user's process: its descriptors are hidden
        const uint64_t key = process_key(p.pid, p.start_time);
        const std::string dir = "/proc/" + std::to_string(p.pid) + "/fd";
        for (const auto &name : list_dir(dir)) {
            char target[64];
            const ssize_t n = ::readlink((dir + "/" + name).c_str(), target, sizeof(target) - 1);
            if (n <= 8 || std::strncmp(target, "socket:[", 8) != 0) continue;
            target[n] = '\0';
            owner[std::strtoull(target + 8, nullptr, 10)] = key;
        }
    }
}

}  // namespace

bool process_network_available() {
    static const bool available = [] {
        std::unordered_map<uint64_t, SocketBytes> sockets;
        std::unordered_map<uint64_t, SocketId> ids;
        return dump_tcp(sockets, ids);
    }();
    return available;
}

void process_network(std::vector<RawProcess> &processes) {
    if (!process_network_available()) return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    const double now = now_seconds();
    // A full dump every 3 s finds new sockets; in between only the known ones are read.
    std::unordered_map<uint64_t, SocketBytes> sockets;
    if (now - s.dumped >= 3 || s.dumped == 0) {
        std::unordered_map<uint64_t, SocketId> ids;
        if (!dump_tcp(sockets, ids)) return;
        s.ids = std::move(ids);
        s.dumped = now;
    } else {
        std::vector<SocketId> known;
        known.reserve(s.ids.size());
        for (const auto &[inode, id] : s.ids)
            if (s.owner.count(inode)) known.push_back(id);
        if (!query_tcp(known, sockets)) return;
    }
    bool unknown = false;
    for (const auto &[inode, bytes] : sockets)
        if (inode != 0 && !s.owner.count(inode) && !s.unowned.count(inode)) unknown = true;
    if ((unknown && now - s.scanned >= 2) || now - s.scanned >= 30) {
        scan_owners(processes, s.owner);
        s.scanned = now;
        s.unowned.clear();
        for (const auto &[inode, bytes] : sockets)
            if (!s.owner.count(inode)) s.unowned.insert(inode);
    }

    std::unordered_map<uint64_t, SocketBytes> previous;
    previous.reserve(sockets.size());
    for (const auto &[inode, bytes] : sockets) {
        auto owner = s.owner.find(inode);
        if (owner == s.owner.end()) continue;
        // A socket counts from the tick it is first seen with an owner: the bytes it moved before (up to
        // a dump interval for a new one) would land in one tick and show as a rate it never had.
        SocketBytes delta{};
        if (auto before = s.previous.find(inode); before != s.previous.end()) {
            delta.rx = bytes.rx >= before->second.rx ? bytes.rx - before->second.rx : bytes.rx;
            delta.tx = bytes.tx >= before->second.tx ? bytes.tx - before->second.tx : bytes.tx;
        }
        SocketBytes &total = s.totals[owner->second];
        total.rx += delta.rx;
        total.tx += delta.tx;
        previous[inode] = bytes;
    }
    s.previous = std::move(previous);

    std::unordered_map<uint64_t, SocketBytes> totals;
    totals.reserve(processes.size());
    for (auto &p : processes) {
        if (!p.has_disk_io) continue;
        const uint64_t key = process_key(p.pid, p.start_time);
        const SocketBytes total = s.totals.count(key) ? s.totals[key] : SocketBytes{};
        p.has_net_io = true;
        p.net_rx = total.rx;
        p.net_tx = total.tx;
        totals[key] = total;
    }
    s.totals = std::move(totals);  // processes that exited are forgotten
}

}  // namespace procyon::platform

#endif  // __linux__
