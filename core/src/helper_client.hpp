#pragma once

#include <string>
#include <vector>

#include "helper_protocol.hpp"
#include "procyon/procyon.h"

namespace procyon {

// Connection from the monitor to a running procyon-helper.
class HelperClient {
public:
    HelperClient() = default;
    ~HelperClient() { disconnect(); }
    // Owns the socket descriptor: a copy would close it twice.
    HelperClient(const HelperClient &) = delete;
    HelperClient &operator=(const HelperClient &) = delete;

    bool connect(const std::string &socket_path);
    void disconnect();
    bool connected() const { return fd_ >= 0; }

    // Counters for the given pids; pids the helper cannot read are omitted.
    bool sample(const std::vector<int32_t> &pids, std::vector<helper::Counters> &out);
    pc_result signal(int32_t pid, int32_t signal);
    pc_result set_priority(int32_t pid, int32_t nice);
    bool details(int32_t pid, platform::Details &out);
    // System-domain launchd action (pc_service_action).
    pc_result launchd(const std::string &label, int32_t action);
    // The helper's cached list of OS-managed startup items; `ready` false while it is still reading.
    bool startup_items(std::vector<pc_startup_item> &out, bool &ready);
    // Open files and sockets of `pid` (-1 every process) as root sees them.
    bool open_files(int32_t pid, std::vector<platform::OpenFile> &out, bool &complete);
    bool connections(int32_t pid, std::vector<pc_connection> &out, bool &complete);

private:
    bool send_all(const void *data, size_t size);
    bool receive_all(void *data, size_t size);
    // Sends a header-only request and reads an int32 pc_result.
    pc_result simple(helper::Request type, int32_t pid, uint32_t flags, const void *payload = nullptr,
                     size_t payload_size = 0);

    int fd_ = -1;
};

}  // namespace procyon
