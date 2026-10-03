// Core checks that need no UI and change nothing on the machine: the helper wire format, input
// validation, the view builder, the launchctl parsers, and the refusals that keep protected
// processes and system services safe.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "helper_protocol.hpp"
#include "monitor.hpp"
#include "platform.hpp"
#include "procyon/procyon.h"
#include "view.hpp"

using namespace procyon;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        (void)std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void details_round_trip() {
    platform::Details in;
    in.cwd = "/tmp";
    in.arguments_known = true;
    in.arguments = {"/bin/zsh", "-c", "echo hi", ""};
    in.environment = {"PATH=/usr/bin", "EMPTY="};
    in.threads_known = true;
    platform::ThreadInfo thread;
    thread.id = 42;
    thread.name = "main";
    thread.cpu_percent = 12.5;
    thread.user_time_ns = 1000;
    thread.system_time_ns = 2000;
    thread.priority = 31;
    thread.state = PC_STATE_RUNNING;
    in.threads = {thread};

    const auto bytes = helper::encode_details(in);
    platform::Details out;
    check(helper::decode_details(bytes, out), "details decode");
    check(out.cwd == in.cwd && out.arguments == in.arguments && out.environment == in.environment, "details strings");
    check(out.arguments_known && out.threads_known && out.threads.size() == 1, "details flags");
    check(out.threads[0].id == 42 && out.threads[0].name == "main" && out.threads[0].cpu_percent == 12.5 &&
              out.threads[0].priority == 31 && out.threads[0].state == PC_STATE_RUNNING,
          "details thread");

    // A truncated reply must be rejected, never read past its end.
    for (size_t cut = 0; cut < bytes.size(); cut += 7) {
        std::vector<char> truncated(bytes.begin(), bytes.begin() + static_cast<long>(cut));
        check(!helper::decode_details(truncated, out), "truncated details rejected");
    }
}

void files_round_trip() {
    std::vector<platform::OpenFile> in = {
        {12, 3, PC_FILE_REGULAR, "/tmp/a b.txt"}, {12, -1, PC_FILE_CWD, "/"}, {99, 7, PC_FILE_DIRECTORY, ""}};
    const auto bytes = helper::encode_files(in);
    std::vector<platform::OpenFile> out;
    check(helper::decode_files(bytes, out) && out.size() == 3, "files decode");
    check(out[0].pid == 12 && out[0].fd == 3 && out[0].path == "/tmp/a b.txt" && out[1].kind == PC_FILE_CWD &&
              out[2].kind == PC_FILE_DIRECTORY,
          "files fields");
    for (size_t cut = 0; cut < bytes.size(); cut += 5) {
        std::vector<char> truncated(bytes.begin(), bytes.begin() + static_cast<long>(cut));
        check(!helper::decode_files(truncated, out), "truncated files rejected");
    }
}

// This process's own descriptors are always readable: a file it opened and a socket it listens on.
void own_handles() {
    const std::string path = "/tmp/procyon-core-test-" + std::to_string(getpid());
    FILE *file = std::fopen(path.c_str(), "w");
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool listening = listener >= 0 && ::bind(listener, reinterpret_cast<sockaddr *>(&address), length) == 0 &&
                           ::listen(listener, 1) == 0 &&
                           ::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0;

    pc_monitor *monitor = pc_monitor_create();
    const pc_open_file *files = nullptr;
    bool complete = false;
    const int32_t file_count = pc_monitor_open_files(monitor, getpid(), &files, &complete);
    bool found = false;
    for (int32_t i = 0; i < file_count; ++i)
        found |=
            files[i].kind == PC_FILE_REGULAR && std::string(files[i].path).find(path.substr(5)) != std::string::npos;
    check(complete && found, "own open file listed");

    const pc_connection *connections = nullptr;
    const int32_t connection_count = pc_monitor_connections(monitor, getpid(), &connections, &complete);
    bool listed = false;
    for (int32_t i = 0; i < connection_count; ++i)
        listed |= connections[i].protocol == PC_PROTOCOL_TCP && connections[i].state == PC_TCP_LISTEN &&
                  connections[i].local_port == ntohs(address.sin_port) &&
                  std::string(connections[i].local_address) == "127.0.0.1";
    check(listening && complete && listed, "own listening socket listed");
    pc_monitor_destroy(monitor);

    if (listener >= 0) ::close(listener);
    if (file) (void)std::fclose(file);
    (void)std::remove(path.c_str());
}

void validation() {
    check(platform::valid_service_label("com.example.agent_1-beta"), "plain label accepted");
    check(!platform::valid_service_label(""), "empty label rejected");
    check(!platform::valid_service_label("-k"), "option-like label rejected");
    check(!platform::valid_service_label("a b"), "label with space rejected");
    check(!platform::valid_service_label("system/com.apple.x"), "label with slash rejected");
    check(!platform::valid_service_label(std::string(300, 'a')), "long label rejected");
    check(platform::valid_signal(15) && !platform::valid_signal(0) && !platform::valid_signal(1000), "signal range");
}

// The helper parses as root on behalf of the app's user: the user's records must be chosen by the
// uid it is given, not by the process's own.
void managed_startup_parsing() {
    const std::string uid = std::to_string(getuid());
    const std::string dump =
        "========================\n Records for UID -2 : X\n========================\n Items:\n\n"
        " #1:\n                 Name: Machine Helper\n                 Type: daemon (0x10)\n"
        "          Disposition: [enabled, allowed, notified] (0xb)\n           Identifier: 16.com.example.daemon\n"
        "                  URL: Contents/Library/LaunchDaemons/com.example.daemon.plist\n"
        "    Parent Identifier: 2.com.example.app\n\n"
        " #2:\n                 Name: Example\n                 Type: app (0x2)\n"
        "          Disposition: [disabled, allowed, notified] (0xa)\n           Identifier: 2.com.example.app\n"
        "                  URL: /Applications/Example.app\n\n"
        "========================\n Records for UID " +
        uid +
        " : Y\n========================\n Items:\n\n"
        " #1:\n                 Name: Opener\n                 Type: app (0x2)\n"
        "          Disposition: [enabled, allowed, notified] (0xb)\n           Identifier: 2.com.example.opener\n"
        "                  URL: /Users/" +
        uid +
        "/Applications/Opener.app\n\n"
        " #2:\n                 Name: Legacy\n                 Type: legacy agent (0x10008)\n"
        "          Disposition: [enabled, allowed, notified] (0xb)\n           Identifier: 8.com.example.legacy\n"
        "                  URL: /Library/LaunchAgents/com.example.legacy.plist\n";

    auto find = [](const std::vector<pc_startup_item> &items, const char *label) -> const pc_startup_item * {
        for (const auto &item : items)
            if (std::string(item.label) == label) return &item;
        return nullptr;
    };
    // Parsing spawns nothing: the loaded services come in as parameters.
    platform::LoadedServices user_jobs, system_jobs;
    user_jobs["com.example.opener"] = {4242, 0};
    system_jobs["com.example.daemon"] = {77, 0};
    const auto mine = platform::parse_managed_startup_items(dump, getuid(), user_jobs, system_jobs);
    const auto *opener = find(mine, "com.example.opener");
    check(opener && opener->scope == PC_STARTUP_OPEN_AT_LOGIN && opener->enabled, "user's open-at-login app listed");
    check(opener && std::string(opener->app_path).rfind("/Users/" + uid + "/", 0) != 0, "redacted home path restored");
    const auto *daemon = find(mine, "com.example.daemon");
    check(daemon && daemon->scope == PC_STARTUP_APP_BACKGROUND && daemon->managed_by_os, "machine-wide item listed");
    check(daemon && std::string(daemon->app_path) == "/Applications/Example.app", "embedded item uses its app");
    check(daemon && daemon->pid == 77 && opener && opener->pid == 4242, "running pids from the given services");
    check(!find(mine, "com.example.legacy"), "legacy agents left to the launchd scan");
    check(!find(mine, "com.example.app"), "apps that don't open at login skipped");

    const auto other = platform::parse_managed_startup_items(dump, getuid() + 1000, {}, {});
    check(!find(other, "com.example.opener") && find(other, "com.example.daemon"), "another user's records skipped");
}

void launchctl_parsing() {
    const std::string print =
        "system = {\n\ttype = system\n\thandle = 0\n\tactive count = 700\n"
        "\tservices = {\n"
        "\t\t  412    0  com.example.running\n"
        "\t\t    -    1  com.example.failed\n"
        "\t\t    -  (pe)  com.example.pending\n"
        "\t\tgarbage line\n"
        "\t}\n"
        "\tdisabled services = {\n\t\t  9  9  com.example.after\n\t}\n}\n";
    const auto loaded = platform::parse_launchctl_print(print);
    check(loaded.size() == 3, "services block parsed");
    check(loaded.count("com.example.running") && loaded.at("com.example.running").pid == 412 &&
              loaded.at("com.example.running").last_exit == 0,
          "running service pid");
    check(loaded.count("com.example.failed") && loaded.at("com.example.failed").pid == 0 &&
              loaded.at("com.example.failed").last_exit == 1,
          "stopped service exit status");
    check(loaded.count("com.example.pending") && loaded.at("com.example.pending").last_exit == 0, "(pe) reads 0");
    check(!loaded.count("com.example.after"), "parsing stops at the block's end");
    check(platform::parse_launchctl_print("nothing here").empty(), "no services block, no services");

    const std::string disabled =
        "disabled services = {\n"
        "\t\"com.example.off\" => disabled\n"
        "\t\"com.example.on\" => enabled\n"
        "\t\"com.example.legacy_off\" => true\n"
        "\t\"com.example.legacy_on\" => false\n"
        "\tno quotes => disabled\n"
        "}\n";
    const auto overrides = platform::parse_launchctl_disabled(disabled);
    check(overrides.size() == 4, "print-disabled entries parsed");
    check(overrides.count("com.example.off") && overrides.at("com.example.off"), "disabled read as disabled");
    check(overrides.count("com.example.on") && !overrides.at("com.example.on"), "enabled read as enabled");
    check(overrides.count("com.example.legacy_off") && overrides.at("com.example.legacy_off") &&
              overrides.count("com.example.legacy_on") && !overrides.at("com.example.legacy_on"),
          "older true/false form");
}

// The kernel's per-core tick counters are 32-bit: a wrap must read as the small delta it is.
void tick_wrap() {
    platform::CpuTicks before{0xFFFFFFF0u, 0xFFFFFFFFu, 100, 5};
    platform::CpuTicks now{0x00000010u, 0x00000002u, 160, 5};
    const TickDelta d = tick_delta(now, before);
    check(d.user == 0x20 && d.system == 3 && d.idle == 60 && d.nice == 0, "wrapped tick delta");
    const TickDelta first = tick_delta(now, platform::CpuTicks{});
    check(first.user == 0x10 && first.idle == 160, "first sample counts from zero");
    // Widened: sums of several wrapped deltas never overflow 64 bits.
    check(d.user + d.system + d.idle + d.nice == 0x20 + 3 + 60, "delta sum");
}

// A listing is incomplete only when descriptors exist that we may not see.
void handle_denial() {
    check(platform::handles_denied(0, EPERM) && platform::handles_denied(-1, EACCES), "refused reads are denials");
    check(!platform::handles_denied(0, ESRCH), "a process that is gone or a zombie has nothing to list");
    check(!platform::handles_denied(0, 0), "an empty descriptor table has nothing to list");
    check(!platform::handles_denied(48, EPERM), "a successful read is never a denial");
    check(!platform::handles_denied(0, EINVAL), "other failures hide nothing");
}

// ---- view builder ----

pc_process make_process(int32_t pid, int32_t ppid, const char *name, const char *app_id, const char *app_name,
                        double cpu, const char *user = "me") {
    pc_process p{};
    p.pid = pid;
    p.ppid = ppid;
    p.cpu_percent = cpu;
    p.memory_bytes = 1000 * pid;
    p.disk_read_bps = p.disk_write_bps = p.net_rx_bps = p.net_tx_bps = -1;
    p.gpu_percent = p.power_watts = -1;
    p.threads = 1;
    (void)std::snprintf(p.name, sizeof(p.name), "%s", name);
    (void)std::snprintf(p.app_id, sizeof(p.app_id), "%s", app_id);
    (void)std::snprintf(p.app_name, sizeof(p.app_name), "%s", app_name);
    (void)std::snprintf(p.user, sizeof(p.user), "%s", user);
    return p;
}

std::vector<pc_process> sample_processes() {
    return {
        make_process(1, 0, "launchd", "exe:launchd", "launchd", 0.5, "root"),
        // Safari: the helper has the lowest pid but its parent is inside the group; the app is the
        // representative because its parent (launchd) is outside.
        make_process(300, 200, "Safari Helper", "/Applications/Safari.app", "Safari", 3.0),
        make_process(200, 1, "Safari", "/Applications/Safari.app", "Safari", 2.0),
        make_process(310, 200, "Safari Networking", "/Applications/Safari.app", "Safari", -1),
        make_process(400, 1, "zsh", "exe:zsh", "zsh", 0.0),
        make_process(410, 400, "node", "exe:node", "node", 12.0),
        make_process(411, 410, "Électron", "exe:Électron", "Électron", 1.0),
        make_process(500, 1, "Mail", "/Applications/Mail.app", "Mail", -1),
    };
}

int32_t pid_of(const std::vector<pc_process> &processes, const pc_row &row) {
    return row.process_index >= 0 ? processes[static_cast<size_t>(row.process_index)].pid : row.group_pid;
}

void view_grouping() {
    const auto processes = sample_processes();
    View view;

    pc_view_query query{PC_VIEW_GROUPED, PC_COLUMN_CPU, true, nullptr, 0};
    build_view(processes, query, view);
    const pc_row *safari = nullptr;
    for (const auto &row : view.rows)
        if (row.process_index < 0 && row.group_name && std::string(row.group_name) == "Safari") safari = &row;
    check(safari && safari->group_pid == 200, "group representative: parent outside the group wins over lowest pid");
    check(safari && safari->process_count == 3 && safari->child_count == 3, "group counts every member");
    check(safari && safari->cpu_percent == 5.0 && safari->memory_bytes == 810000, "group sums known values");
    check(safari && safari->gpu_percent == -1, "group stays unknown when no member reports");
    size_t singles = 0;
    for (const auto &row : view.rows)
        if (row.depth == 0 && row.process_index >= 0) ++singles;
    check(singles == 5, "single-process apps are plain rows");
    check(!view.rows.empty() && view.rows.front().depth == 0 && pid_of(processes, view.rows.front()) == 410,
          "grouped view sorted by cpu descending");

    // A filter on a member's name keeps the group with its totals but lists only that member.
    query.filter = "networking";
    build_view(processes, query, view);
    check(view.rows.size() == 2 && view.rows[0].process_index < 0 && view.rows[0].process_count == 3 &&
              view.rows[0].cpu_percent == 5.0 && view.rows[0].child_count == 1 &&
              pid_of(processes, view.rows[1]) == 310,
          "filtered group keeps totals, lists matching members");
    // A filter on the app name includes every member.
    query.filter = "SAFARI";
    build_view(processes, query, view);
    check(view.rows.size() == 4 && view.rows[0].child_count == 3, "filter on the app name includes all members");
    // Case folding beyond ASCII, both ways.
    query.filter = "ÉLECTRON";
    build_view(processes, query, view);
    check(view.rows.size() == 1 && pid_of(processes, view.rows[0]) == 411, "UTF-8 upper-case filter matches");
    query.filter = "électron";
    build_view(processes, query, view);
    check(view.rows.size() == 1 && pid_of(processes, view.rows[0]) == 411, "UTF-8 lower-case filter matches");
    query.filter = "41";
    build_view(processes, query, view);
    check(view.rows.size() == 2, "pid digits match");
    query.filter = "nothing-like-this";
    build_view(processes, query, view);
    check(view.rows.empty(), "no match, no rows");
}

void view_tree() {
    const auto processes = sample_processes();
    View view;
    pc_view_query query{PC_VIEW_TREE, PC_COLUMN_PID, false, nullptr, 0};
    build_view(processes, query, view);
    check(view.rows.size() == processes.size(), "tree lists every process");
    check(view.rows[0].process_index >= 0 && pid_of(processes, view.rows[0]) == 1 && view.rows[0].depth == 0,
          "launchd is the root");
    bool node_under_zsh = false;
    for (size_t i = 0; i < view.rows.size(); ++i)
        if (pid_of(processes, view.rows[i]) == 410)
            node_under_zsh = view.rows[i].depth == 2 && view.rows[i].parent_row >= 0 &&
                             pid_of(processes, view.rows[static_cast<size_t>(view.rows[i].parent_row)]) == 400;
    check(node_under_zsh, "children nest under their parent in pre-order");

    // A match deep in the tree brings its ancestors along, nothing else.
    query.filter = "électron";
    build_view(processes, query, view);
    check(view.rows.size() == 4, "ancestors of a match are included");
    check(view.rows.size() == 4 && pid_of(processes, view.rows[0]) == 1 && pid_of(processes, view.rows[1]) == 400 &&
              pid_of(processes, view.rows[2]) == 410 && pid_of(processes, view.rows[3]) == 411,
          "ancestor chain in order");
    check(view.rows.size() == 4 && view.rows[3].depth == 3, "depth follows the chain");
}

void view_sorting_and_limit() {
    const auto processes = sample_processes();
    View view;
    // Unknown (-1) sinks to the bottom whatever the direction.
    pc_view_query query{PC_VIEW_FLAT, PC_COLUMN_CPU, true, nullptr, 0};
    build_view(processes, query, view);
    check(view.rows.size() == processes.size(), "flat view lists every process");
    check(pid_of(processes, view.rows.front()) == 410, "descending: highest cpu first");
    const auto last_two = [&] {
        const size_t n = view.rows.size();
        const int32_t a = pid_of(processes, view.rows[n - 2]), b = pid_of(processes, view.rows[n - 1]);
        return (a == 310 && b == 500) || (a == 500 && b == 310);
    };
    check(last_two(), "descending: unknown values last");
    query.descending = false;
    build_view(processes, query, view);
    check(pid_of(processes, view.rows.front()) == 400, "ascending: lowest known cpu first");
    check(last_two(), "ascending: unknown values still last");
    check(view.rows.size() >= 2 && pid_of(processes, view.rows[view.rows.size() - 2]) == 310,
          "ties among unknowns break by pid");

    query.descending = true;
    query.limit = 3;
    build_view(processes, query, view);
    check(view.rows.size() == 3 && pid_of(processes, view.rows[0]) == 410 && pid_of(processes, view.rows[1]) == 300,
          "limit keeps the top rows");
    query.mode = PC_VIEW_GROUPED;
    query.limit = 2;
    build_view(processes, query, view);
    size_t top_level = 0;
    for (const auto &row : view.rows) top_level += row.depth == 0;
    check(top_level == 2 && view.rows.size() == 5, "limit counts top-level rows, members come along");
}

void refusals() {
    pc_monitor *monitor = pc_monitor_create();
    check(pc_process_signal(monitor, 1, 15) == PC_ERR_PROTECTED, "launchd refuses signals");
    check(pc_process_signal(monitor, getpid(), 15) == PC_ERR_PROTECTED, "self refuses signals");
    check(pc_process_signal(monitor, getppid(), 0) == PC_ERR_INVALID, "signal 0 rejected");
    check(pc_process_suspend(monitor, 0) == PC_ERR_PROTECTED, "kernel can't be suspended");
    check(pc_process_set_priority(monitor, 1, 5) == PC_ERR_PROTECTED, "launchd priority unchanged");
    check(pc_process_set_affinity(monitor, getppid(), 0) == PC_ERR_INVALID ||
              pc_process_set_affinity(monitor, getppid(), 0) == PC_ERR_UNSUPPORTED,
          "empty affinity rejected");
    // System-domain changes need the helper; without it nothing runs.
    check(pc_service_control(monitor, PC_DOMAIN_SYSTEM, "com.example.none", PC_SERVICE_STOP) == PC_ERR_PERMISSION,
          "system service needs helper");
    check(pc_service_control(monitor, PC_DOMAIN_USER, "bad label", PC_SERVICE_STOP) == PC_ERR_INVALID,
          "bad service label rejected");
    check(pc_service_control(monitor, PC_DOMAIN_USER, "com.example.none", 99) == PC_ERR_INVALID, "bad action rejected");
    check(pc_startup_set_enabled(monitor, PC_STARTUP_DAEMON, "com.example.none", false) == PC_ERR_PERMISSION,
          "daemon needs helper");
    check(pc_startup_set_enabled(monitor, 7, "com.example.none", false) == PC_ERR_INVALID, "bad scope rejected");

    const pc_process_details *details = nullptr;
    check(pc_process_details_get(monitor, getpid(), &details) == PC_OK && details, "own details readable");
    check(details && details->arguments_known && details->argument_count >= 1, "own arguments readable");
    check(details && details->threads_known && details->thread_count >= 1, "own threads readable");
    check(pc_process_details_get(monitor, -5, &details) == PC_ERR_NOT_FOUND, "unknown pid not found");
    pc_monitor_destroy(monitor);
}

}  // namespace

int main() {
    try {
        details_round_trip();
        files_round_trip();
        own_handles();
        validation();
        managed_startup_parsing();
        launchctl_parsing();
        tick_wrap();
        handle_denial();
        view_grouping();
        view_tree();
        view_sorting_and_limit();
        refusals();
    } catch (const std::exception &error) {
        (void)std::fprintf(stderr, "FAIL: exception %s\n", error.what());
        return 1;
    }
    if (failures == 0) (void)std::printf("core tests passed\n");
    return failures == 0 ? 0 : 1;
}
