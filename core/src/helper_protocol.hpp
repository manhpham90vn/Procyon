// Wire protocol between the core (client) and procyon-helper (privileged server).
// Same-machine Unix socket, native byte order, fixed-size records.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "platform.hpp"

namespace procyon::helper {

constexpr uint32_t kMagic = 0x50524359;  // "PRCY"
constexpr uint32_t kVersion = 3;

enum class Request : uint32_t {
    Hello = 1,     // -> HelloReply
    Sample = 2,    // payload: uint32 count + count * int32 pid -> uint32 count + count * Counters
    Signal = 3,    // pid + flags(signal number) -> int32 pc_result
    Priority = 4,  // pid + flags(nice as int32) -> int32 pc_result
    Details = 5,   // pid -> uint32 size + size bytes (encode_details); size 0 when unreadable
    Launchd = 6,   // flags(pc_service_action) + payload char[256] label, system domain only -> int32 pc_result
    Startup = 7,   // -> uint32 ready + uint32 count + count * pc_startup_item (the helper's cached list)
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
// Hard limit on a Details reply (arguments + environment + threads).
constexpr uint32_t kMaxDetailsBytes = 8 << 20;
constexpr size_t kLabelSize = 256;
constexpr uint32_t kMaxStartupItems = 4096;

// ---- Details encoding: length-prefixed strings and fixed-size numbers ----

class Writer {
public:
    template <typename T>
    void number(T value) {
        append(&value, sizeof(value));
    }
    void string(const std::string &value) {
        number(static_cast<uint32_t>(value.size()));
        append(value.data(), value.size());
    }
    const std::vector<char> &bytes() const { return bytes_; }

private:
    void append(const void *data, size_t size) {
        auto p = static_cast<const char *>(data);
        bytes_.insert(bytes_.end(), p, p + size);
    }
    std::vector<char> bytes_;
};

class Reader {
public:
    Reader(const char *data, size_t size) : data_(data), size_(size) {}
    template <typename T>
    bool number(T &value) {
        if (size_ - offset_ < sizeof(value)) return false;
        std::memcpy(&value, data_ + offset_, sizeof(value));
        offset_ += sizeof(value);
        return true;
    }
    bool string(std::string &value) {
        uint32_t length = 0;
        if (!number(length) || size_ - offset_ < length) return false;
        value.assign(data_ + offset_, length);
        offset_ += length;
        return true;
    }

private:
    const char *data_;
    size_t size_;
    size_t offset_ = 0;
};

inline std::vector<char> encode_details(const platform::Details &d) {
    Writer w;
    w.string(d.cwd);
    w.number<uint8_t>(d.arguments_known);
    w.number(static_cast<uint32_t>(d.arguments.size()));
    for (const auto &a : d.arguments) w.string(a);
    w.number(static_cast<uint32_t>(d.environment.size()));
    for (const auto &e : d.environment) w.string(e);
    w.number<uint8_t>(d.threads_known);
    w.number(static_cast<uint32_t>(d.threads.size()));
    for (const auto &t : d.threads) {
        w.number(t.id);
        w.string(t.name);
        w.number(t.cpu_percent);
        w.number(t.user_time_ns);
        w.number(t.system_time_ns);
        w.number(t.priority);
        w.number(t.state);
    }
    return w.bytes();
}

inline bool decode_details(const std::vector<char> &bytes, platform::Details &d) {
    Reader r(bytes.data(), bytes.size());
    d = {};
    uint8_t flag = 0;
    uint32_t count = 0;
    if (!r.string(d.cwd) || !r.number(flag)) return false;
    d.arguments_known = flag != 0;
    if (!r.number(count)) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!r.string(d.arguments.emplace_back())) return false;
    if (!r.number(count)) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!r.string(d.environment.emplace_back())) return false;
    if (!r.number(flag)) return false;
    d.threads_known = flag != 0;
    if (!r.number(count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        auto &t = d.threads.emplace_back();
        if (!r.number(t.id) || !r.string(t.name) || !r.number(t.cpu_percent) || !r.number(t.user_time_ns) ||
            !r.number(t.system_time_ns) || !r.number(t.priority) || !r.number(t.state))
            return false;
    }
    return true;
}

}  // namespace procyon::helper
