#include "helper_client.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace procyon {

bool HelperClient::connect(const std::string &socket_path) {
    disconnect();
    sockaddr_un address{};
    if (socket_path.size() >= sizeof(address.sun_path)) return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);

    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    // A stuck helper must never freeze sampling.
    timeval timeout{2, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int no_sigpipe = 1;
    setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));

    // Only a root helper is worth talking to; anything else squatting on the path is ignored.
    uid_t peer_uid = 1;
    gid_t peer_gid = 0;
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        getpeereid(fd_, &peer_uid, &peer_gid) != 0 || peer_uid != 0) {
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
    ready = ready_flag != 0;
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
        ssize_t sent = ::send(fd_, bytes, size, 0);
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
