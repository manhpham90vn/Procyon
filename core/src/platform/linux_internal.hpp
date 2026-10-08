// Shared helpers of the Linux adapter files: procfs/sysfs reads, string splitting, running a tool.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../platform.hpp"

namespace procyon::platform::linux_internal {

// Whole file (procfs files report size 0, so read until EOF). False when it can't be opened.
bool read_file(const std::string &path, std::string &out);
// First line without the newline, "" when unreadable.
std::string read_line(const std::string &path);
// The file as an integer, `fallback` when unreadable or not a number.
int64_t read_int(const std::string &path, int64_t fallback = -1);
// Target of a symlink, "" when unreadable.
std::string read_link(const std::string &path);
bool exists(const std::string &path);
// Names in a directory, without "." and "..". Empty when unreadable.
std::vector<std::string> list_dir(const std::string &path);
// Numeric entries of /proc: every pid.
std::vector<int32_t> list_pids();

std::string trim(std::string_view text);
std::vector<std::string> split(std::string_view text, char separator);
// Splits on runs of spaces and tabs.
std::vector<std::string> split_whitespace(std::string_view text);
bool starts_with(std::string_view text, std::string_view prefix);
std::string basename_of(const std::string &path);

void copy_string(char *dst, size_t capacity, const std::string &src);

// Runs `argv` (no shell, LC_ALL=C) and returns its exit status, -1 when it couldn't start or was
// killed after `timeout_ms`. Standard output goes to `out`, standard error to `err` when given.
int run(const std::vector<std::string> &argv, std::string &out, std::string *err = nullptr, int timeout_ms = 10000);
// Path of an executable found in PATH (or the standard system directories), "" when missing.
std::string find_program(const std::string &name);

// Clock ticks per second (USER_HZ) and the boot time in unix seconds, read once.
uint64_t clock_ticks();
int64_t boot_time();

// The parsed part of /proc/<pid>/stat that every file needs.
struct ProcStat {
    int32_t pid = 0;
    std::string comm;
    char state = '?';
    int32_t ppid = 0;
    uint32_t flags = 0;
    uint64_t utime = 0, stime = 0;  // clock ticks
    int32_t priority = 0;
    int32_t nice = 0;
    int32_t threads = 0;
    uint64_t start_ticks = 0;                                          // since boot
    bool kernel_thread() const { return (flags & 0x00200000u) != 0; }  // PF_KTHREAD
};
// Parses the text of /proc/<pid>/stat or /proc/<pid>/task/<tid>/stat. Exposed for tests.
bool parse_stat(const std::string &text, ProcStat &out);
bool read_stat(int32_t pid, ProcStat &out);
int32_t state_of(char state);

}  // namespace procyon::platform::linux_internal
