// procyon-helper: the privileged side. Keep it small and paranoid: it runs as root.
#include <poll.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "helper_protocol.hpp"
#include "platform.hpp"
#include "procyon/helper.h"

using namespace procyon;

namespace {

constexpr int kAcceptTimeoutSeconds = 30;

void log(const char *message) { (void)std::fprintf(stderr, "procyon-helper: %s\n", message); }

// Strict decimal parse: the helper runs as root, so malformed arguments must be rejected.
bool parse_number(const char *text, long &out) {
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    out = value;
    return true;
}

bool send_all(int fd, const void *data, size_t size) {
    auto bytes = static_cast<const char *>(data);
    while (size > 0) {
        ssize_t sent = ::send(fd, bytes, size, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool receive_all(int fd, void *data, size_t size) {
    auto bytes = static_cast<char *>(data);
    while (size > 0) {
        ssize_t received = ::recv(fd, bytes, size, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        bytes += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

// Only the launching app (same pid, same user) may talk to us.
bool peer_is_trusted(int fd, pid_t parent, uid_t uid) {
    uid_t peer_uid = 0;
    gid_t peer_gid = 0;
    if (getpeereid(fd, &peer_uid, &peer_gid) != 0 || peer_uid != uid) return false;
#if defined(__APPLE__)
    pid_t peer_pid = 0;
    socklen_t length = sizeof(peer_pid);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer_pid, &length) != 0 || peer_pid != parent) return false;
#endif
    return true;
}

bool serve_sample(int fd) {
    uint32_t count = 0;
    if (!receive_all(fd, &count, sizeof(count)) || count > helper::kMaxPids) return false;
    std::vector<int32_t> pids(count);
    if (!receive_all(fd, pids.data(), count * sizeof(int32_t))) return false;

    std::vector<helper::Counters> reply;
    reply.reserve(count);
    for (int32_t pid : pids) {
        platform::RawProcess raw;
        if (pid < 0 || !platform::read_counters(pid, raw)) continue;
        helper::Counters c;
        c.pid = pid;
        c.threads = raw.threads;
        c.start_time = platform::start_time(pid);
        c.cpu_time_ns = raw.cpu_time_ns;
        c.memory_bytes = raw.memory_bytes;
        c.disk_read = raw.disk_read;
        c.disk_write = raw.disk_write;
        c.has_disk_io = raw.has_disk_io ? 1 : 0;
        reply.push_back(c);
    }
    const auto reply_count = static_cast<uint32_t>(reply.size());
    return send_all(fd, &reply_count, sizeof(reply_count)) &&
           send_all(fd, reply.data(), reply.size() * sizeof(helper::Counters));
}

bool serve_end(int fd, const helper::RequestHeader &header, pid_t parent) {
    int32_t result;
    // Never kernel_task, launchd, ourselves or the app that launched us.
    if (header.pid <= 1 || header.pid == getpid() || header.pid == parent)
        result = PC_ERR_PROTECTED;
    else
        result = platform::signal_process(header.pid, header.flags & 1);
    return send_all(fd, &result, sizeof(result));
}

void serve(int client, pid_t parent, int parent_watch) {
    pollfd fds[2] = {{client, POLLIN, 0}, {parent_watch, POLLIN, 0}};
    while (true) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents) return;  // the app exited
        if (!(fds[0].revents & POLLIN)) return;

        helper::RequestHeader header;
        if (!receive_all(client, &header, sizeof(header)) || header.magic != helper::kMagic) return;
        bool ok = false;
        switch (static_cast<helper::Request>(header.type)) {
            case helper::Request::Hello: {
                helper::HelloReply reply;
                reply.helper_pid = getpid();
                ok = send_all(client, &reply, sizeof(reply));
                break;
            }
            case helper::Request::Sample: ok = serve_sample(client); break;
            case helper::Request::End: ok = serve_end(client, header, parent); break;
        }
        if (!ok) return;
    }
}

}  // namespace

int pc_helper_main(int argc, char **argv) {
    std::string socket_path;
    long parent_arg = -1;
    long uid_arg = -1;
    bool valid = true;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--socket"))
            socket_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--parent"))
            valid &= parse_number(argv[i + 1], parent_arg);
        else if (!std::strcmp(argv[i], "--uid"))
            valid &= parse_number(argv[i + 1], uid_arg);
        else
            valid = false;
    }
    const auto parent = static_cast<pid_t>(parent_arg);
    if (!valid || socket_path.empty() || parent_arg <= 1 || parent_arg > INT32_MAX || uid_arg < 0 ||
        uid_arg > UINT32_MAX) {
        log("usage: procyon-helper --socket <path> --parent <pid> --uid <uid>");
        return 2;
    }
    const auto uid = static_cast<uid_t>(uid_arg);
    if (geteuid() != 0) log("warning: not running as root; system processes stay unreadable");

    // Exit as soon as the app goes away, even if it never connects.
    int kq = kqueue();
    struct kevent watch;
    EV_SET(&watch, parent, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (kq < 0 || kevent(kq, &watch, 1, nullptr, 0, nullptr) != 0) {
        log("parent process not found");
        return 1;
    }

    sockaddr_un address{};
    if (socket_path.size() >= sizeof(address.sun_path)) {
        log("socket path too long");
        return 1;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);

    int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ::unlink(socket_path.c_str());
    const mode_t previous_umask = umask(0177);  // socket born 0600
    const bool bound = server >= 0 && ::bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    umask(previous_umask);
    if (!bound || ::listen(server, 1) != 0 || ::chown(socket_path.c_str(), uid, static_cast<gid_t>(-1)) != 0) {
        log("cannot create socket");
        ::unlink(socket_path.c_str());
        return 1;
    }

    int exit_code = 1;
    pollfd fds[2] = {{server, POLLIN, 0}, {kq, POLLIN, 0}};
    int remaining_ms = kAcceptTimeoutSeconds * 1000;
    while (remaining_ms > 0) {
        if (poll(fds, 2, 1000) < 0 && errno != EINTR) break;
        remaining_ms -= 1000;
        if (fds[1].revents) break;
        if (!(fds[0].revents & POLLIN)) continue;
        int client = ::accept(server, nullptr, nullptr);
        if (client < 0) continue;
        if (!peer_is_trusted(client, parent, uid)) {
            log("rejected untrusted client");
            ::close(client);
            continue;
        }
        int no_sigpipe = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
        // Stop listening before serving: there is only ever one client.
        ::close(server);
        server = -1;
        ::unlink(socket_path.c_str());
        serve(client, parent, kq);
        ::close(client);
        exit_code = 0;
        break;
    }
    if (server >= 0) {
        ::close(server);
        ::unlink(socket_path.c_str());
    }
    ::close(kq);
    return exit_code;
}
