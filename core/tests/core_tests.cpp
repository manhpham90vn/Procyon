// Core checks that need no UI and change nothing on the machine: the helper wire format, input
// validation, and the refusals that keep protected processes and system services safe.
#include <unistd.h>

#include <cstdio>
#include <exception>
#include <string>

#include "helper_protocol.hpp"
#include "platform.hpp"
#include "procyon/procyon.h"

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
    const auto mine = platform::parse_managed_startup_items(dump, getuid());
    const auto *opener = find(mine, "com.example.opener");
    check(opener && opener->scope == PC_STARTUP_OPEN_AT_LOGIN && opener->enabled, "user's open-at-login app listed");
    check(opener && std::string(opener->app_path).rfind("/Users/" + uid + "/", 0) != 0, "redacted home path restored");
    const auto *daemon = find(mine, "com.example.daemon");
    check(daemon && daemon->scope == PC_STARTUP_APP_BACKGROUND && daemon->managed_by_os, "machine-wide item listed");
    check(daemon && std::string(daemon->app_path) == "/Applications/Example.app", "embedded item uses its app");
    check(!find(mine, "com.example.legacy"), "legacy agents left to the launchd scan");
    check(!find(mine, "com.example.app"), "apps that don't open at login skipped");

    const auto other = platform::parse_managed_startup_items(dump, getuid() + 1000);
    check(!find(other, "com.example.opener") && find(other, "com.example.daemon"), "another user's records skipped");
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
        validation();
        managed_startup_parsing();
        refusals();
    } catch (const std::exception &error) {
        (void)std::fprintf(stderr, "FAIL: exception %s\n", error.what());
        return 1;
    }
    if (failures == 0) (void)std::printf("core tests passed\n");
    return failures == 0 ? 0 : 1;
}
