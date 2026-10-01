// Wire protocol between the core (client) and procyon-helper (privileged server).
// Same-machine Unix socket, native byte order, fixed-size records.
#pragma once

#include <cstdint>

namespace procyon::helper {

constexpr uint32_t kMagic = 0x50524359;  // "PRCY"
constexpr uint32_t kVersion = 1;

enum class Request : uint32_t {
    Hello = 1,   // -> HelloReply
    Sample = 2,  // payload: uint32 count + count * int32 pid -> uint32 count + count * Counters
    End = 3,     // pid + flags(bit0 = force) -> int32 pc_result
};

struct RequestHeader {
    uint32_t magic = kMagic;
    uint32_t type = 0;
    int32_t pid = 0;
    uint32_t flags = 0;
};

struct HelloReply {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    int32_t helper_pid = 0;
};

struct Counters {
    int32_t pid = 0;
    int32_t threads = -1;
    int64_t start_time = 0;
    uint64_t cpu_time_ns = 0;
    int64_t memory_bytes = -1;
    uint64_t disk_read = 0;
    uint64_t disk_write = 0;
    uint32_t has_disk_io = 0;
    uint32_t reserved = 0;
};

// Hard limit on pids per Sample request, guards the server against bogus sizes.
constexpr uint32_t kMaxPids = 1 << 16;

}  // namespace procyon::helper
