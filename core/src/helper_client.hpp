#pragma once

#include <string>
#include <vector>

#include "helper_protocol.hpp"
#include "procyon/procyon.h"

namespace procyon {

// Connection from the monitor to a running procyon-helper.
class HelperClient {
public:
    ~HelperClient() { disconnect(); }

    bool connect(const std::string &socket_path);
    void disconnect();
    bool connected() const { return fd_ >= 0; }

    // Counters for the given pids; pids the helper cannot read are omitted.
    bool sample(const std::vector<int32_t> &pids, std::vector<helper::Counters> &out);
    pc_result end(int32_t pid, bool force);

private:
    bool send_all(const void *data, size_t size);
    bool receive_all(void *data, size_t size);

    int fd_ = -1;
};

}  // namespace procyon
