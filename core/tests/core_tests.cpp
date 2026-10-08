// Core checks that need no UI and change nothing on the machine: the helper wire format, input
// validation, the view builder, the launchctl parsers, and the refusals that keep protected
// processes and system services safe.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "helper_protocol.hpp"
#if defined(__linux__)
#include <sys/wait.h>

#include <csignal>

#include "platform/linux_internal.hpp"
#endif
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

#if defined(_WIN32)
int32_t own_pid() { return static_cast<int32_t>(GetCurrentProcessId()); }
// A pid that exists and is not ours: the System process.
int32_t other_pid() { return 4; }
int32_t init_pid() { return 4; }  // the protected init equivalent
#else
int32_t own_pid() { return getpid(); }
int32_t other_pid() { return getppid(); }
int32_t init_pid() { return 1; }
#endif

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
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    char temp_dir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, temp_dir);
    const std::string path = std::string(temp_dir) + "procyon-core-test-" + std::to_string(own_pid());
    const std::string needle = "procyon-core-test-" + std::to_string(own_pid());
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, 0);
    const bool have_listener = listener != INVALID_SOCKET;
#else
    const std::string path = "/tmp/procyon-core-test-" + std::to_string(own_pid());
    const std::string needle = path.substr(5);
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    const bool have_listener = listener >= 0;
#endif
    FILE *file = std::fopen(path.c_str(), "w");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool listening = have_listener && ::bind(listener, reinterpret_cast<sockaddr *>(&address), length) == 0 &&
                           ::listen(listener, 1) == 0 &&
                           ::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0;

    pc_monitor *monitor = pc_monitor_create();
    const pc_open_file *files = nullptr;
    bool complete = false;
    const int32_t file_count = pc_monitor_open_files(monitor, own_pid(), &files, &complete);
    bool found = false;
    for (int32_t i = 0; i < file_count; ++i)
        found |= files[i].kind == PC_FILE_REGULAR && std::string(files[i].path).find(needle) != std::string::npos;
    check(complete && found, "own open file listed");

    const pc_connection *connections = nullptr;
    const int32_t connection_count = pc_monitor_connections(monitor, own_pid(), &connections, &complete);
    bool listed = false;
    for (int32_t i = 0; i < connection_count; ++i)
        listed |= connections[i].protocol == PC_PROTOCOL_TCP && connections[i].state == PC_TCP_LISTEN &&
                  connections[i].local_port == ntohs(address.sin_port) &&
                  std::string(connections[i].local_address) == "127.0.0.1";
    check(listening && complete && listed, "own listening socket listed");
    pc_monitor_destroy(monitor);

#if defined(_WIN32)
    if (have_listener) closesocket(listener);
#else
    if (have_listener) ::close(listener);
#endif
    if (file) (void)std::fclose(file);
    (void)std::remove(path.c_str());
}

void validation() {
    check(platform::valid_service_label("com.example.agent_1-beta"), "plain label accepted");
    check(!platform::valid_service_label(""), "empty label rejected");
    check(!platform::valid_service_label("-k"), "option-like label rejected");
    check(!platform::valid_service_label("system/com.apple.x"), "label with slash rejected");
    check(!platform::valid_service_label(std::string(300, 'a')), "long label rejected");
    check(platform::valid_signal(15) && !platform::valid_signal(0) && !platform::valid_signal(1000), "signal range");
#if defined(_WIN32)
    // Windows service names may contain spaces ("Bonjour Service"); nothing is passed to a shell.
    check(platform::valid_service_label("Bonjour Service"), "service name with space accepted");
    check(!platform::valid_service_label("a\\b"), "label with backslash rejected");
    check(platform::valid_service_label("run:hkcu:OneDrive"), "startup label accepted");
#else
    check(!platform::valid_service_label("a b"), "label with space rejected");
#endif
}

#if defined(_WIN32)
// powercfg's report is parsed by section order (the headers may be localized): one holder with a
// display and a system request shows once with both kinds, a driver is named after its device, and
// performance boosts keep nothing awake.
void power_request_parsing() {
    const std::string report =
        "DISPLAY:\r\n"
        "[PROCESS] \\Device\\HarddiskVolume3\\Apps\\Player.exe\r\n"
        "Playing a video\r\n"
        "\r\n"
        "SYSTEM:\r\n"
        "[DRIVER] Realtek High Definition Audio (HDAUDIO\\FUNC_01&VEN_10EC&DEV_0897)\r\n"
        "An audio stream is currently in use.\r\n"
        "[PROCESS] \\Device\\HarddiskVolume3\\Apps\\Player.exe\r\n"
        "Playing a video\r\n"
        "\r\n"
        "AWAYMODE:\r\n"
        "None.\r\n"
        "\r\n"
        "EXECUTION:\r\n"
        "None.\r\n"
        "\r\n"
        "PERFBOOST:\r\n"
        "[PROCESS] \\Device\\HarddiskVolume3\\Apps\\Game.exe\r\n"
        "Boost\r\n"
        "\r\n"
        "ACTIVELOCKSCREEN:\r\n"
        "None.\r\n";
    const auto requests = platform::parse_power_requests(report);
    check(requests.size() == 2, "two holders listed, the boost left out");
    bool player = false, driver = false;
    for (const auto &r : requests) {
        if (r.reason == "Playing a video")
            player = r.kind == (PC_ASSERT_DISPLAY_SLEEP | PC_ASSERT_SYSTEM_SLEEP) &&
                     r.type == "DisplayRequired+SystemRequired";
        if (r.pid == 0 && r.type == "SystemRequired")
            driver = r.kind == PC_ASSERT_SYSTEM_SLEEP &&
                     r.reason == "Realtek High Definition Audio · An audio stream is currently in use.";
    }
    check(player, "display and system requests of one process merged");
    check(driver, "driver request named after its device, instance id dropped");
    check(platform::parse_power_requests("").empty() && platform::parse_power_requests("DISPLAY:\nNone.\n").empty(),
          "empty report, empty list");
}
#endif

#if defined(__APPLE__)
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
#endif  // __APPLE__

#if defined(__linux__)
// /proc/<pid>/stat: the command name may hold spaces and parentheses.
void proc_stat_parsing() {
    platform::linux_internal::ProcStat stat;
    const std::string text =
        "1234 (Web Content (x)) S 1000 1234 1234 0 -1 4194560 100 0 0 0 250 50 0 0 20 -5 31 0 98765 1000 200 "
        "18446744073709551615";
    check(platform::linux_internal::parse_stat(text, stat), "stat parsed");
    check(stat.pid == 1234 && stat.comm == "Web Content (x)" && stat.state == 'S' && stat.ppid == 1000,
          "stat identity");
    check(stat.utime == 250 && stat.stime == 50 && stat.nice == -5 && stat.threads == 31 && stat.start_ticks == 98765,
          "stat counters");
    check(!stat.kernel_thread(), "a user process is no kernel thread");
    check(platform::linux_internal::parse_stat("2 (kthreadd) S 0 0 0 0 -1 2129984 0 0 0 0 0 0 0 0 20 0 1 0 1 0 0",
                                               stat) &&
              stat.kernel_thread(),
          "PF_KTHREAD flags a kernel thread");
    check(!platform::linux_internal::parse_stat("garbage", stat), "garbage rejected");
}

void proc_net_parsing() {
    const std::string tcp =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 4242 1\n"
        "   1: 0F02000A:D2F0 22D8B85D:01BB 01 00000000:00000000 00:00000000 00000000  1000        0 4343 1\n";
    const auto entries = platform::parse_proc_net(tcp, PC_PROTOCOL_TCP, 4);
    check(entries.size() == 2, "two tcp rows");
    check(entries.size() == 2 && std::string(entries[0].connection.local_address) == "127.0.0.1" &&
              entries[0].connection.local_port == 8080 && entries[0].connection.state == PC_TCP_LISTEN &&
              entries[0].connection.remote_address[0] == '\0' && entries[0].inode == 4242,
          "listening socket on loopback");
    check(entries.size() == 2 && std::string(entries[1].connection.local_address) == "10.0.2.15" &&
              std::string(entries[1].connection.remote_address) == "93.184.216.34" &&
              entries[1].connection.remote_port == 443 && entries[1].connection.state == PC_TCP_ESTABLISHED,
          "established connection");
    const std::string tcp6 =
        "  sl  local_address                         remote_address                        st\n"
        "   0: 00000000000000000000000000000000:0016 00000000000000000000000000000000:0000 0A 00000000:00000000 "
        "00:00000000 00000000     0        0 77 1\n"
        "   1: 00000000000000000000000001000000:0277 00000000000000000000000000000000:0000 0A 00000000:00000000 "
        "00:00000000 00000000     0        0 78 1\n";
    const auto v6 = platform::parse_proc_net(tcp6, PC_PROTOCOL_TCP, 6);
    check(v6.size() == 2 && std::string(v6[0].connection.local_address) == "*" && v6[0].connection.local_port == 22,
          "any address reads as *");
    check(v6.size() == 2 && std::string(v6[1].connection.local_address) == "::1", "ipv6 loopback");
    const auto udp = platform::parse_proc_net(
        "header\n 5: 00000000:14E9 00000000:0000 07 00000000:00000000 00:00000000 00000000 0 0 99 2\n", PC_PROTOCOL_UDP,
        4);
    check(udp.size() == 1 && udp[0].connection.state == PC_TCP_NONE && udp[0].connection.local_port == 5353,
          "udp has no tcp state");
}

void drm_fdinfo_parsing() {
    uint64_t client = 0, ns = 0;
    const std::string amd =
        "pos:\t0\nflags:\t02100002\ndrm-driver:\tamdgpu\ndrm-client-id:\t17\ndrm-engine-gfx:\t5000 ns\n"
        "drm-engine-dec:\t9000 ns\ndrm-engine-capacity-gfx:\t2\ndrm-memory-vram:\t1024 KiB\n";
    check(platform::parse_drm_fdinfo(amd, client, ns) && client == 17 && ns == 9000, "busiest engine kind counted");
    check(!platform::parse_drm_fdinfo("pos:\t0\nflags:\t02\n", client, ns), "a plain file is no DRM client");
}

void inhibitor_parsing() {
    const std::string json =
        "{\"type\":\"a(ssssuu)\",\"data\":[[[\"sleep\",\"Firefox\",\"Playing video\",\"block\",1000,4242],"
        "[\"shutdown:sleep\",\"NetworkManager\",\"NetworkManager needs to turn off networks\",\"delay\",0,812],"
        "[\"idle\",\"GNOME Videos\",\"Playing \\\"Movie\\\"\",\"block\",1000,5000],"
        "[\"handle-lid-switch\",\"gsd\",\"External monitor\",\"block\",1000,900]]]}";
    const auto locks = platform::parse_inhibitors(json);
    check(locks.size() == 2, "delay locks and lid handling left out");
    check(locks.size() == 2 && locks[0].pid == 4242 && locks[0].kind == PC_ASSERT_SYSTEM_SLEEP &&
              locks[0].reason == "Firefox · Playing video",
          "sleep lock");
    check(locks.size() == 2 && locks[1].kind == (PC_ASSERT_DISPLAY_SLEEP | PC_ASSERT_SYSTEM_SLEEP) &&
              locks[1].reason == "GNOME Videos · Playing \"Movie\"",
          "idle lock keeps the display on, escapes read");
    check(platform::parse_inhibitors("").empty() && platform::parse_inhibitors("{\"data\":[[]]}").empty(), "no locks");
    const auto paths = platform::busctl_values(
        "{\"type\":\"ao\",\"data\":[[\"/org/gnome/SessionManager/Inhibitor4\",\"/org/gnome/SessionManager/"
        "Inhibitor7\"]]}");
    check(paths.size() == 2 && paths[1] == "/org/gnome/SessionManager/Inhibitor7", "object paths read");
    const auto flags = platform::busctl_values("{\"type\":\"u\",\"data\":[12]}");
    check(flags.size() == 1 && flags[0] == "12", "a number read");
    check(platform::busctl_values("{\"type\":\"s\",\"data\":[\"\"]}") == std::vector<std::string>{""},
          "an empty string is still a value");
}

// Suspend and resume through the native signal numbers (SIGSTOP 19 and SIGCONT 18 on Linux, not
// macOS's 17 and 19): a child stops and continues.
// The machine's memory parts partition its total (the composition bar's proportions rely on it).
void memory_partition() {
    platform::Memory m;
    check(platform::memory(m), "memory read");
    const uint64_t parts = m.app + m.wired + m.compressed + m.cached + m.free;
    check(parts <= m.total && m.total - parts <= m.total / 100, "memory parts add up to the total");
}

void native_signals() {
    const pid_t child = ::fork();
    if (child == 0) {
        ::pause();
        ::_exit(0);
    }
    check(child > 0, "child started");
    if (child <= 0) return;
    const auto state = [child] {
        platform::linux_internal::ProcStat stat;
        for (int i = 0; i < 100; ++i) {
            if (platform::linux_internal::read_stat(child, stat) && stat.state != 'S' && stat.state != 'R')
                return stat.state;
            ::usleep(10000);
        }
        return platform::linux_internal::read_stat(child, stat) ? stat.state : '?';
    };
    check(platform::send_signal(child, SIGSTOP) == PC_OK && state() == 'T', "SIGSTOP stops");
    check(platform::send_signal(child, SIGCONT) == PC_OK, "SIGCONT continues");
    (void)::kill(child, SIGKILL);
    (void)::waitpid(child, nullptr, 0);
}

void systemd_parsing() {
    const auto units = platform::parse_systemctl_show(
        "Id=ssh.service\nDescription=OpenBSD Secure Shell server\nMainPID=812\nExecStart={ path=/usr/sbin/sshd ; "
        "argv[]=/usr/sbin/sshd -D ; ignore_errors=no }\n\nId=cups.service\nMainPID=0\nUnitFileState=disabled\n");
    check(units.size() == 2, "one map per unit");
    check(units.size() == 2 && units[0].at("Description") == "OpenBSD Secure Shell server" &&
              units[0].at("MainPID") == "812" && units[1].at("UnitFileState") == "disabled",
          "unit properties");

    const auto entry = platform::parse_desktop_entry(
        "[Desktop Entry]\nName=Backup\nExec=env FOO=1 backup --quiet\nOnlyShowIn=GNOME;Unity;\nHidden=true\n"
        "X-GNOME-Autostart-enabled=false\n[Desktop Action New]\nName=Other\n");
    check(entry.name == "Backup" && entry.exec == "env FOO=1 backup --quiet" && entry.only_show_in == "GNOME;Unity;",
          "desktop entry keys");
    check(entry.hidden && !entry.autostart_enabled, "desktop entry switches");
    check(platform::valid_service_label("getty@tty1.service") &&
              platform::valid_service_label("xdg:org.gnome.Foo.desktop") &&
              platform::valid_service_label("dev-disk-by\\x2duuid.swap"),
          "unit and autostart labels accepted");
    check(platform::valid_service_label("xdg:My App.desktop") && platform::valid_service_label("xdg:\xc3\x9c"
                                                                                               "bersicht.desktop"),
          "autostart labels with spaces and UTF-8 accepted");
    check(!platform::valid_service_label("xdg:../x.desktop") && !platform::valid_service_label("xdg:a/b.desktop") &&
              !platform::valid_service_label("xdg:") && !platform::valid_service_label("xdg:a\nb.desktop") &&
              !platform::valid_service_label("my unit.service"),
          "autostart labels with a path, control byte or nothing rejected; units keep their rule");

    using platform::HwmonInput;
    const auto cpu = [](std::vector<HwmonInput> inputs) { return platform::cpu_sensor_inputs(inputs); };
    check(cpu({{"coretemp", "Core 0", "c0"}, {"coretemp", "Package id 0", "p"}, {"acpitz", "", "a"}}) ==
              std::vector<std::string>{"p"},
          "coretemp package sensor preferred");
    check(cpu({{"coretemp", "Core 0", "c0"}, {"coretemp", "Core 1", "c1"}, {"acpitz", "", "a"}}) ==
              std::vector<std::string>{"c0", "c1"},
          "coretemp cores without a package sensor, not ACPI");
    check(cpu({{"k10temp", "Tctl", "tctl"}, {"k10temp", "Tdie", "tdie"}}) == std::vector<std::string>{"tdie"},
          "k10temp Tdie preferred over the offset Tctl");
    check(cpu({{"k10temp", "Tctl", "tctl"}}) == std::vector<std::string>{"tctl"}, "k10temp Tctl alone");
    check(cpu({{"acpitz", "", "a"}, {"nvme", "Composite", "n"}}) == std::vector<std::string>{"a"},
          "ACPI zone as the last resort");
    check(platform::battery_condition("Good", 0.5) == "Normal" &&
              platform::battery_condition("Dead", 1) == "Service Recommended" &&
              platform::battery_condition("Overheat", 1) == "Overheat" &&
              platform::battery_condition("Unknown", 0.79) == "Service Recommended" &&
              platform::battery_condition("", 0.9) == "Normal" && platform::battery_condition("", -1).empty(),
          "battery condition in macOS's words");
    check(platform::linux_internal::arch_name("aarch64") == "arm64" &&
              platform::linux_internal::arch_name("i686") == "x86" &&
              platform::linux_internal::arch_name("x86_64") == "x86_64",
          "architecture names as procyon.h spells them");
    check(platform::systemd_os_unit("systemd-journald.service", "/usr/lib/systemd/system/systemd-journald.service",
                                    "/usr/lib/systemd/systemd-journald") &&
              platform::systemd_os_unit("accounts-daemon.service", "/usr/lib/systemd/system/accounts-daemon.service",
                                        "/usr/libexec/accounts-daemon"),
          "systemd's and the session's own units are part of the OS");
    check(!platform::systemd_os_unit("docker.service", "/usr/lib/systemd/system/docker.service", "/usr/bin/dockerd") &&
              !platform::systemd_os_unit("nginx.service", "/lib/systemd/system/nginx.service", "/usr/sbin/nginx") &&
              !platform::systemd_os_unit("systemd-custom.service", "/etc/systemd/system/systemd-custom.service",
                                         "/usr/lib/systemd/x"),
          "packaged daemons and administrators' units are third-party");
}
#endif  // __linux__

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
#if defined(_WIN32)
    check(platform::handles_denied(0, ERROR_ACCESS_DENIED) && platform::handles_denied(-1, ERROR_ACCESS_DENIED),
          "refused reads are denials");
    check(!platform::handles_denied(0, ERROR_INVALID_PARAMETER), "a process that is gone has nothing to list");
    check(!platform::handles_denied(0, 0), "an empty handle table has nothing to list");
    check(!platform::handles_denied(48, ERROR_ACCESS_DENIED), "a successful read is never a denial");
#else
    check(platform::handles_denied(0, EPERM) && platform::handles_denied(-1, EACCES), "refused reads are denials");
    check(!platform::handles_denied(0, ESRCH), "a process that is gone or a zombie has nothing to list");
    check(!platform::handles_denied(0, 0), "an empty descriptor table has nothing to list");
    check(!platform::handles_denied(48, EPERM), "a successful read is never a denial");
    check(!platform::handles_denied(0, EINVAL), "other failures hide nothing");
#endif
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
    const bool signals = (pc_capabilities() & PC_CAP_SIGNALS) != 0;
    check(pc_process_signal(monitor, init_pid(), 15) == (signals ? PC_ERR_PROTECTED : PC_ERR_UNSUPPORTED),
          "init refuses signals");
    check(pc_process_signal(monitor, own_pid(), 15) == (signals ? PC_ERR_PROTECTED : PC_ERR_UNSUPPORTED),
          "self refuses signals");
    if (signals) check(pc_process_signal(monitor, other_pid(), 0) == PC_ERR_INVALID, "signal 0 rejected");
    check(pc_process_suspend(monitor, 0) == PC_ERR_PROTECTED, "kernel can't be suspended");
    check(pc_process_set_priority(monitor, init_pid(), 5) == PC_ERR_PROTECTED, "init priority unchanged");
    // An unprotected process of ours or another user: protection is checked before the mask.
    int32_t victim = other_pid();
#if defined(_WIN32)
    for (const pc_process *p = pc_monitor_snapshot(monitor)->processes,
                          *end = p + pc_monitor_snapshot(monitor)->process_count;
         p < end; ++p)
        if (!(p->flags & PC_PROC_PROTECTED) && p->pid != own_pid()) victim = p->pid;
#endif
    check(pc_process_set_affinity(monitor, victim, 0) == PC_ERR_INVALID ||
              pc_process_set_affinity(monitor, victim, 0) == PC_ERR_UNSUPPORTED,
          "empty affinity rejected");
#if defined(__linux__)
    // No helper: systemctl answers (asking polkit only for units that exist). Without systemd the
    // capability is off.
    const bool services = (pc_capabilities() & PC_CAP_SERVICES) != 0;
    check(pc_service_control(monitor, PC_DOMAIN_SYSTEM, "procyon-no-such.service", PC_SERVICE_STOP) ==
              (services ? PC_ERR_NOT_FOUND : PC_ERR_UNSUPPORTED),
          "unknown unit not found");
    check(pc_service_control(monitor, PC_DOMAIN_USER, "bad label", PC_SERVICE_STOP) ==
              (services ? PC_ERR_INVALID : PC_ERR_UNSUPPORTED),
          "bad service label rejected");
    check(pc_startup_set_enabled(monitor, PC_STARTUP_USER_AGENT, "xdg:procyon-no-such.desktop", false) ==
              PC_ERR_NOT_FOUND,
          "unknown autostart entry not found");
    check(pc_startup_set_enabled(monitor, PC_STARTUP_USER_AGENT, "xdg:../escape.desktop", false) == PC_ERR_INVALID,
          "autostart label with a path rejected");
#elif !defined(_WIN32)
    // System-domain changes need the helper; without it nothing runs.
    check(pc_service_control(monitor, PC_DOMAIN_SYSTEM, "com.example.none", PC_SERVICE_STOP) == PC_ERR_PERMISSION,
          "system service needs helper");
    check(pc_service_control(monitor, PC_DOMAIN_USER, "bad label", PC_SERVICE_STOP) == PC_ERR_INVALID,
          "bad service label rejected");
    check(pc_startup_set_enabled(monitor, PC_STARTUP_DAEMON, "com.example.none", false) == PC_ERR_PERMISSION,
          "daemon needs helper");
#else
    // No helper: the OS answers, and a service that doesn't exist is reported as such.
    check(pc_service_control(monitor, PC_DOMAIN_SYSTEM, "ProcyonNoSuchService", PC_SERVICE_STOP) == PC_ERR_NOT_FOUND,
          "unknown service not found");
    check(pc_service_control(monitor, PC_DOMAIN_USER, "bad/label", PC_SERVICE_STOP) == PC_ERR_INVALID,
          "bad service label rejected");
    check(pc_startup_set_enabled(monitor, PC_STARTUP_USER_AGENT, "run:nowhere:Thing", false) == PC_ERR_INVALID,
          "startup label without a known source rejected");
#endif
    check(pc_service_control(monitor, PC_DOMAIN_USER, "com.example.none", 99) == PC_ERR_INVALID, "bad action rejected");
    check(pc_startup_set_enabled(monitor, 7, "com.example.none", false) == PC_ERR_INVALID, "bad scope rejected");

    const pc_process_details *details = nullptr;
    check(pc_process_details_get(monitor, own_pid(), &details) == PC_OK && details, "own details readable");
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
#if defined(_WIN32)
        power_request_parsing();
#elif defined(__APPLE__)
        managed_startup_parsing();
        launchctl_parsing();
#else
        proc_stat_parsing();
        proc_net_parsing();
        drm_fdinfo_parsing();
        inhibitor_parsing();
        systemd_parsing();
        native_signals();
        memory_partition();
#endif
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
