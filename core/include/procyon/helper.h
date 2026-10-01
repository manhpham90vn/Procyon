/*
 * Privileged helper entry point (procyon-helper).
 *
 *   procyon-helper --socket <path> --parent <pid> --uid <uid>
 *
 * Runs as root, serves exactly one client: the process `parent`, owned by `uid`, connecting
 * over the Unix socket at `path` (created 0600 and owned by `uid`). Exits when the client
 * disconnects, when `parent` exits, or when nobody connects within 30 seconds.
 */
#ifndef PROCYON_HELPER_H
#define PROCYON_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

int pc_helper_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* PROCYON_HELPER_H */
