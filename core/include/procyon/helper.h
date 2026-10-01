/*
 * Privileged helper entry point (procyon-helper). Two modes, same socket protocol:
 *
 *   procyon-helper --daemon
 *     macOS LaunchDaemon registered once by the app with SMAppService (Developer ID builds).
 *     launchd owns the socket at PC_HELPER_DAEMON_SOCKET and starts the helper on the first
 *     connection. Accepts only PC_HELPER_CLIENT_ID signed by the helper's own team and run by an
 *     administrator. Exits after a minute without clients.
 *
 *   procyon-helper --socket <path> --parent <pid> --uid <uid>
 *     Started by the app through a password prompt (development builds without a team signature).
 *     Serves exactly one client: the process `parent`, owned by `uid`, connecting over the Unix
 *     socket at `path` (created 0600 and owned by `uid`). Exits when the client disconnects, when
 *     `parent` exits, or when nobody connects within 30 seconds.
 */
#ifndef PROCYON_HELPER_H
#define PROCYON_HELPER_H

#define PC_HELPER_CLIENT_ID "dev.procyon.app"
#define PC_HELPER_DAEMON_LABEL "dev.procyon.helper"
#define PC_HELPER_DAEMON_SOCKET "/var/run/dev.procyon.helper.sock"

#ifdef __cplusplus
extern "C" {
#endif

int pc_helper_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* PROCYON_HELPER_H */
