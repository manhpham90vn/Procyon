// procyon-helper: the privileged side. Keep it small and paranoid: it runs as root.
#include <poll.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <Security/Security.h>
#include <bsm/libbsm.h>
#include <launch.h>
#include <membership.h>
#endif

#include "helper_protocol.hpp"
#include "platform.hpp"
#include "procyon/helper.h"

using namespace procyon;

namespace {

constexpr int kAcceptTimeoutSeconds = 30;
// Daemon mode: launchd keeps the socket and relaunches us on the next connection.
constexpr int kDaemonIdleSeconds = 60;

// Clients being served in daemon mode; static so detached threads never outlive it.
std::atomic<int> active_clients{0};

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
        c.has_energy = raw.has_energy ? 1 : 0;
        c.energy_nj = raw.energy_nj;
        reply.push_back(c);
    }
    const auto reply_count = static_cast<uint32_t>(reply.size());
    return send_all(fd, &reply_count, sizeof(reply_count)) &&
           send_all(fd, reply.data(), reply.size() * sizeof(helper::Counters));
}

// Never kernel_task, launchd, ourselves or the app we serve.
bool is_protected(int32_t pid, pid_t client_pid) { return pid <= 1 || pid == getpid() || pid == client_pid; }

bool send_result(int fd, int32_t result) { return send_all(fd, &result, sizeof(result)); }

bool serve_signal(int fd, const helper::RequestHeader &header, pid_t client_pid) {
    const auto signal = static_cast<int32_t>(header.flags);
    if (is_protected(header.pid, client_pid)) return send_result(fd, PC_ERR_PROTECTED);
    if (!platform::valid_signal(signal)) return send_result(fd, PC_ERR_INVALID);
    return send_result(fd, platform::send_signal(header.pid, signal));
}

bool serve_priority(int fd, const helper::RequestHeader &header, pid_t client_pid) {
    if (is_protected(header.pid, client_pid)) return send_result(fd, PC_ERR_PROTECTED);
    return send_result(fd, platform::set_priority(header.pid, static_cast<int32_t>(header.flags)));
}

bool serve_details(int fd, const helper::RequestHeader &header) {
    platform::Details details;
    std::vector<char> bytes;
    if (header.pid >= 0) {
        platform::process_details(header.pid, details);
        bytes = helper::encode_details(details);
    }
    if (bytes.size() > helper::kMaxDetailsBytes) bytes.clear();
    const auto size = static_cast<uint32_t>(bytes.size());
    return send_all(fd, &size, sizeof(size)) && send_all(fd, bytes.data(), bytes.size());
}

// Read-only, like Sample: paths and endpoints, never contents.
bool serve_open_files(int fd, const helper::RequestHeader &header) {
    std::vector<platform::OpenFile> files;
    const uint32_t complete = platform::open_files(header.pid < 0 ? -1 : header.pid, files) ? 1 : 0;
    auto bytes = helper::encode_files(files);
    if (bytes.size() > helper::kMaxFilesBytes) bytes = helper::encode_files({});
    const auto size = static_cast<uint32_t>(bytes.size());
    return send_all(fd, &complete, sizeof(complete)) && send_all(fd, &size, sizeof(size)) &&
           send_all(fd, bytes.data(), bytes.size());
}

bool serve_connections(int fd, const helper::RequestHeader &header) {
    std::vector<pc_connection> list;
    const uint32_t complete = platform::connections(header.pid < 0 ? -1 : header.pid, list) ? 1 : 0;
    if (list.size() > helper::kMaxConnections) list.resize(helper::kMaxConnections);
    const auto count = static_cast<uint32_t>(list.size());
    return send_all(fd, &complete, sizeof(complete)) && send_all(fd, &count, sizeof(count)) &&
           send_all(fd, list.data(), list.size() * sizeof(pc_connection));
}

// The OS-managed startup list takes seconds to read (sfltool): keep the last one and refresh it on
// a background thread, so the request answers at once and sampling never waits on it.
struct StartupCache {
    std::mutex mutex;
    std::vector<pc_startup_item> items;
    bool ready = false;
    bool refreshing = false;
    std::chrono::steady_clock::time_point fetched;
};

// One list per user: the daemon can serve several, and each sees their own login items.
StartupCache &startup_cache(uid_t user) {
    static std::mutex mutex;
    static std::unordered_map<uid_t, std::unique_ptr<StartupCache>> caches;
    std::lock_guard lock(mutex);
    auto &cache = caches[user];
    if (!cache) cache = std::make_unique<StartupCache>();
    return *cache;
}

// `client_uid`: the app's user. We run as root, so "the current user" would be root.
bool serve_startup(int fd, uid_t client_uid) {
    constexpr auto kMaxAge = std::chrono::seconds(20);
    auto &cache = startup_cache(client_uid);
    std::vector<pc_startup_item> items;
    uint32_t ready = 0;
    {
        std::lock_guard lock(cache.mutex);
        if (!cache.refreshing && (!cache.ready || std::chrono::steady_clock::now() - cache.fetched > kMaxAge)) {
            cache.refreshing = true;
            std::thread([client_uid] {
                auto list = platform::managed_startup_items(client_uid);
                auto &cache = startup_cache(client_uid);
                std::lock_guard lock(cache.mutex);
                cache.items = std::move(list);
                cache.ready = true;
                cache.refreshing = false;
                cache.fetched = std::chrono::steady_clock::now();
            }).detach();
        }
        items = cache.items;
        ready = cache.ready ? 1 : 0;
    }
    if (items.size() > helper::kMaxStartupItems) items.resize(helper::kMaxStartupItems);
    const auto count = static_cast<uint32_t>(items.size());
    return send_all(fd, &ready, sizeof(ready)) && send_all(fd, &count, sizeof(count)) &&
           send_all(fd, items.data(), items.size() * sizeof(pc_startup_item));
}

// Only fixed launchctl verbs on validated labels in the system domain.
bool serve_launchd(int fd, const helper::RequestHeader &header) {
    char payload[helper::kLabelSize];
    if (!receive_all(fd, payload, sizeof(payload))) return false;
    payload[sizeof(payload) - 1] = '\0';
    const std::string label(payload);
    if (!platform::valid_service_label(label)) return send_result(fd, PC_ERR_INVALID);
    return send_result(fd, platform::service_control(PC_DOMAIN_SYSTEM, label, static_cast<int32_t>(header.flags)));
}

// parent_watch < 0: no parent to watch (daemon mode); poll ignores the entry.
void serve(int client, pid_t client_pid, uid_t client_uid, int parent_watch) {
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
            case helper::Request::Signal: ok = serve_signal(client, header, client_pid); break;
            case helper::Request::Priority: ok = serve_priority(client, header, client_pid); break;
            case helper::Request::Details: ok = serve_details(client, header); break;
            case helper::Request::Launchd: ok = serve_launchd(client, header); break;
            case helper::Request::Startup: ok = serve_startup(client, client_uid); break;
            case helper::Request::OpenFiles: ok = serve_open_files(client, header); break;
            case helper::Request::Connections: ok = serve_connections(client, header); break;
        }
        if (!ok) return;
    }
}

#if defined(__APPLE__)

// "anchor apple generic and identifier \"dev.procyon.app\" and certificate leaf[subject.OU] = \"<our team>\"".
// Empty when this binary isn't Developer ID signed: then the daemon refuses to run.
std::string client_requirement() {
    std::string requirement;
    SecCodeRef self = nullptr;
    CFDictionaryRef info = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &self) == errSecSuccess &&
        SecCodeCopySigningInformation(reinterpret_cast<SecStaticCodeRef>(self), kSecCSSigningInformation, &info) ==
            errSecSuccess) {
        auto team = static_cast<CFStringRef>(CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier));
        char buffer[64];
        if (team && CFStringGetCString(team, buffer, sizeof(buffer), kCFStringEncodingUTF8))
            requirement = std::string("anchor apple generic and identifier \"") + PC_HELPER_CLIENT_ID +
                          "\" and certificate leaf[subject.OU] = \"" + buffer + "\"";
    }
    if (info) CFRelease(info);
    if (self) CFRelease(self);
    return requirement;
}

// The connecting process must be Procyon signed by our team (checked through its audit token, so a
// recycled pid can't impersonate it) and run by an administrator, matching the legacy password prompt.
bool daemon_peer_is_trusted(int fd, SecRequirementRef requirement, pid_t &peer_pid, uid_t &peer_uid) {
    audit_token_t token;
    socklen_t length = sizeof(token);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &length) != 0) return false;
    peer_pid = audit_token_to_pid(token);
    peer_uid = audit_token_to_euid(token);

    uuid_t user;
    uuid_t admins;
    int is_admin = 0;
    if (mbr_uid_to_uuid(audit_token_to_euid(token), user) != 0 || mbr_gid_to_uuid(80, admins) != 0 ||
        mbr_check_membership(user, admins, &is_admin) != 0 || !is_admin)
        return false;

    CFDataRef token_data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(&token), sizeof(token));
    const void *keys[] = {kSecGuestAttributeAudit};
    const void *values[] = {token_data};
    CFDictionaryRef attributes =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecCodeRef code = nullptr;
    const bool trusted =
        SecCodeCopyGuestWithAttributes(nullptr, attributes, kSecCSDefaultFlags, &code) == errSecSuccess &&
        SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement) == errSecSuccess;
    if (code) CFRelease(code);
    CFRelease(attributes);
    CFRelease(token_data);
    return trusted;
}

// Launched on demand by launchd (SMAppService). Serves any number of trusted clients, one thread
// each, and exits after kDaemonIdleSeconds without clients.
int daemon_main() {
    const std::string requirement_text = client_requirement();
    SecRequirementRef requirement = nullptr;
    CFStringRef text = CFStringCreateWithCString(nullptr, requirement_text.c_str(), kCFStringEncodingUTF8);
    const bool have_requirement =
        !requirement_text.empty() &&
        SecRequirementCreateWithString(text, kSecCSDefaultFlags, &requirement) == errSecSuccess;
    CFRelease(text);
    if (!have_requirement) {
        log("daemon mode needs a Developer ID signature");
        return 1;
    }

    int *sockets = nullptr;
    size_t socket_count = 0;
    if (launch_activate_socket("Listener", &sockets, &socket_count) != 0 || socket_count == 0) {
        log("no launchd socket (run through SMAppService)");
        CFRelease(requirement);
        return 1;
    }
    const int server = sockets[0];
    for (size_t i = 1; i < socket_count; ++i) ::close(sockets[i]);
    std::free(sockets);

    int idle_seconds = 0;
    while (idle_seconds < kDaemonIdleSeconds) {
        pollfd listener = {server, POLLIN, 0};
        if (poll(&listener, 1, 1000) < 0 && errno != EINTR) break;
        if (!(listener.revents & POLLIN)) {
            idle_seconds = active_clients.load() > 0 ? 0 : idle_seconds + 1;
            continue;
        }
        idle_seconds = 0;
        int client = ::accept(server, nullptr, nullptr);
        if (client < 0) continue;
        pid_t peer_pid = 0;
        uid_t peer_uid = 0;
        if (!daemon_peer_is_trusted(client, requirement, peer_pid, peer_uid)) {
            log("rejected untrusted client");
            ::close(client);
            continue;
        }
        int no_sigpipe = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
        ++active_clients;
        std::thread([client, peer_pid, peer_uid] {
            serve(client, peer_pid, peer_uid, -1);
            ::close(client);
            --active_clients;
        }).detach();
    }
    CFRelease(requirement);
    return 0;
}

#endif

}  // namespace

int pc_helper_main(int argc, char **argv) {
#if defined(__APPLE__)
    if (argc == 2 && !std::strcmp(argv[1], "--daemon")) return daemon_main();
#endif
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
        serve(client, parent, uid, kq);
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
