#include "helper_client.hpp"

#if defined(_WIN32)
// No helper on Windows: the UI relaunches itself elevated for full access, so the core acts with
// the rights it has. The client never connects; every call reports "no helper".
namespace procyon {

bool HelperClient::connect(const std::string &) { return false; }
void HelperClient::disconnect() { fd_ = -1; }
bool HelperClient::sample(const std::vector<int32_t> &, std::vector<helper::Counters> &out) {
    out.clear();
    return false;
}
pc_result HelperClient::signal(int32_t, int32_t) { return PC_ERR_PERMISSION; }
pc_result HelperClient::set_priority(int32_t, int32_t) { return PC_ERR_PERMISSION; }
bool HelperClient::details(int32_t, platform::Details &) { return false; }
pc_result HelperClient::launchd(const std::string &, int32_t) { return PC_ERR_PERMISSION; }
bool HelperClient::startup_items(std::vector<pc_startup_item> &out, bool &ready) {
    out.clear();
    ready = false;
    return false;
}
bool HelperClient::open_files(int32_t, std::vector<platform::OpenFile> &out, bool &complete) {
    out.clear();
    complete = false;
    return false;
}
bool HelperClient::connections(int32_t, std::vector<pc_connection> &out, bool &complete) {
    out.clear();
    complete = false;
    return false;
}
bool HelperClient::send_all(const void *, size_t) { return false; }
bool HelperClient::receive_all(void *, size_t) { return false; }
pc_result HelperClient::simple(helper::Request, int32_t, uint32_t, const void *, size_t) { return PC_ERR_PERMISSION; }

}  // namespace procyon

#else

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <optional>

namespace procyon {
namespace {

// Replies to Sample and Details come straight from counters; a stuck helper must never freeze
// sampling, so they get a short budget.
constexpr time_t kReplyTimeoutSeconds = 2;
// Launchd verbs run launchctl synchronously (up to 15 s) and listing every process's files or
// sockets walks every descriptor on the machine: those replies may legitimately take longer.
constexpr time_t kSlowReplyTimeoutSeconds = 20;

void set_receive_timeout(int fd, time_t seconds) {
    const timeval timeout{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

// Lengthens the receive timeout for one request and puts the default back afterwards. Watches the
// client's descriptor itself: a failed request disconnects (fd -1) before this goes out of scope.
class SlowReply {
public:
    explicit SlowReply(const int &fd) : fd_(fd) {
        if (fd_ >= 0) set_receive_timeout(fd_, kSlowReplyTimeoutSeconds);
    }
    ~SlowReply() {
        if (fd_ >= 0) set_receive_timeout(fd_, kReplyTimeoutSeconds);
    }
    SlowReply(const SlowReply &) = delete;
    SlowReply &operator=(const SlowReply &) = delete;

private:
    const int &fd_;
};

// The uid of the process at the other end of a connected Unix socket, -1 when unknown.
uid_t peer_uid(int fd) {
#if defined(__linux__)
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 ? credentials.uid
                                                                               : static_cast<uid_t>(-1);
#else
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = 0;
    return getpeereid(fd, &uid, &gid) == 0 ? uid : static_cast<uid_t>(-1);
#endif
}

// SO_NOSIGPIPE (BSD) is set on the socket; Linux asks per send.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

bool HelperClient::connect(const std::string &socket_path) {
    disconnect();
    sockaddr_un address{};
    if (socket_path.size() >= sizeof(address.sun_path)) return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);

    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    // A stuck helper must never freeze sampling.
    set_receive_timeout(fd_, kReplyTimeoutSeconds);
    const timeval send_timeout{kReplyTimeoutSeconds, 0};
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif

    // Only a root helper is worth talking to; anything else squatting on the path is ignored.
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || peer_uid(fd_) != 0) {
        disconnect();
        return false;
    }

    helper::RequestHeader hello;
    hello.type = static_cast<uint32_t>(helper::Request::Hello);
    helper::HelloReply reply;
    if (!send_all(&hello, sizeof(hello)) || !receive_all(&reply, sizeof(reply)) || reply.magic != helper::kMagic ||
        reply.version != helper::kVersion) {
        disconnect();
        return false;
    }
    return true;
}

void HelperClient::disconnect() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool HelperClient::sample(const std::vector<int32_t> &pids, std::vector<helper::Counters> &out) {
    out.clear();
    if (!connected()) return false;
    helper::RequestHeader header;
    header.type = static_cast<uint32_t>(helper::Request::Sample);
    const auto count = static_cast<uint32_t>(std::min<size_t>(pids.size(), helper::kMaxPids));
    if (!send_all(&header, sizeof(header)) || !send_all(&count, sizeof(count)) ||
        !send_all(pids.data(), count * sizeof(int32_t))) {
        disconnect();
        return false;
    }
    uint32_t reply_count = 0;
    if (!receive_all(&reply_count, sizeof(reply_count)) || reply_count > count) {
        disconnect();
        return false;
    }
    out.resize(reply_count);
    if (!receive_all(out.data(), reply_count * sizeof(helper::Counters))) {
        disconnect();
        return false;
    }
    return true;
}

pc_result HelperClient::simple(helper::Request type, int32_t pid, uint32_t flags, const void *payload,
                               size_t payload_size) {
    if (!connected()) return PC_ERR_PERMISSION;
    helper::RequestHeader header;
    header.type = static_cast<uint32_t>(type);
    header.pid = pid;
    header.flags = flags;
    int32_t result = PC_ERR_FAILED;
    if (!send_all(&header, sizeof(header)) || (payload_size && !send_all(payload, payload_size)) ||
        !receive_all(&result, sizeof(result))) {
        disconnect();
        return PC_ERR_FAILED;
    }
    return static_cast<pc_result>(result);
}

pc_result HelperClient::signal(int32_t pid, int32_t signal) {
    return simple(helper::Request::Signal, pid, static_cast<uint32_t>(signal));
}

pc_result HelperClient::set_priority(int32_t pid, int32_t nice) {
    return simple(helper::Request::Priority, pid, static_cast<uint32_t>(nice));
}

pc_result HelperClient::launchd(const std::string &label, int32_t action) {
    char payload[helper::kLabelSize] = {};
    if (label.size() >= sizeof(payload)) return PC_ERR_INVALID;
    label.copy(payload, sizeof(payload) - 1);  // payload stays NUL-terminated
    const SlowReply slow(fd_);
    return simple(helper::Request::Launchd, 0, static_cast<uint32_t>(action), payload, sizeof(payload));
}

bool HelperClient::startup_items(std::vector<pc_startup_item> &out, bool &ready) {
    out.clear();
    if (!connected()) return false;
    helper::RequestHeader header;
    header.type = static_cast<uint32_t>(helper::Request::Startup);
    uint32_t ready_flag = 0, count = 0;
    if (!send_all(&header, sizeof(header)) || !receive_all(&ready_flag, sizeof(ready_flag)) ||
        !receive_all(&count, sizeof(count)) || count > helper::kMaxStartupItems) {
        disconnect();
        return false;
    }
    out.resize(count);
    if (!receive_all(out.data(), count * sizeof(pc_startup_item))) {
        disconnect();
        return false;
    }
    for (auto &item : out) {  // never trust a peer's strings to be terminated
        item.label[sizeof(item.label) - 1] = '\0';
        item.name[sizeof(item.name) - 1] = '\0';
        item.program[sizeof(item.program) - 1] = '\0';
        item.config_path[sizeof(item.config_path) - 1] = '\0';
        item.app_path[sizeof(item.app_path) - 1] = '\0';
        item.parent_name[sizeof(item.parent_name) - 1] = '\0';
    }
    ready = ready_flag != 0;
    return true;
}

bool HelperClient::open_files(int32_t pid, std::vector<platform::OpenFile> &out, bool &complete) {
    out.clear();
    if (!connected()) return false;
    const std::optional<SlowReply> slow = pid < 0 ? std::make_optional<SlowReply>(fd_) : std::nullopt;
    helper::RequestHeader header;
    header.type = static_cast<uint32_t>(helper::Request::OpenFiles);
    header.pid = pid;
    uint32_t complete_flag = 0, size = 0;
    if (!send_all(&header, sizeof(header)) || !receive_all(&complete_flag, sizeof(complete_flag)) ||
        !receive_all(&size, sizeof(size)) || size > helper::kMaxFilesBytes) {
        disconnect();
        return false;
    }
    std::vector<char> bytes(size);
    if (!receive_all(bytes.data(), size)) {
        disconnect();
        return false;
    }
    complete = complete_flag != 0;
    return helper::decode_files(bytes, out);
}

bool HelperClient::connections(int32_t pid, std::vector<pc_connection> &out, bool &complete) {
    out.clear();
    if (!connected()) return false;
    const std::optional<SlowReply> slow = pid < 0 ? std::make_optional<SlowReply>(fd_) : std::nullopt;
    helper::RequestHeader header;
    header.type = static_cast<uint32_t>(helper::Request::Connections);
    header.pid = pid;
    uint32_t complete_flag = 0, count = 0;
    if (!send_all(&header, sizeof(header)) || !receive_all(&complete_flag, sizeof(complete_flag)) ||
        !receive_all(&count, sizeof(count)) || count > helper::kMaxConnections) {
        disconnect();
        return false;
    }
    out.resize(count);
    if (!receive_all(out.data(), count * sizeof(pc_connection))) {
        disconnect();
        return false;
    }
    for (auto &c : out) {  // never trust a peer's strings to be terminated
        c.local_address[sizeof(c.local_address) - 1] = '\0';
        c.remote_address[sizeof(c.remote_address) - 1] = '\0';
    }
    complete = complete_flag != 0;
    return true;
}

bool HelperClient::details(int32_t pid, platform::Details &out) {
    if (!connected()) return false;
    helper::RequestHeader header;
    header.type = static_cast<uint32_t>(helper::Request::Details);
    header.pid = pid;
    uint32_t size = 0;
    if (!send_all(&header, sizeof(header)) || !receive_all(&size, sizeof(size)) || size > helper::kMaxDetailsBytes) {
        disconnect();
        return false;
    }
    std::vector<char> bytes(size);
    if (!receive_all(bytes.data(), size)) {
        disconnect();
        return false;
    }
    return size > 0 && helper::decode_details(bytes, out);
}

bool HelperClient::send_all(const void *data, size_t size) {
    auto bytes = static_cast<const char *>(data);
    while (size > 0) {
        ssize_t sent = ::send(fd_, bytes, size, kSendFlags);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool HelperClient::receive_all(void *data, size_t size) {
    auto bytes = static_cast<char *>(data);
    while (size > 0) {
        ssize_t received = ::recv(fd_, bytes, size, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        bytes += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

}  // namespace procyon

#endif  // !_WIN32
