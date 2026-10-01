/*
 * Procyon core: stable C ABI shared by every native UI (SwiftUI, Win32, GTK).
 *
 * Ownership rules
 *  - pc_monitor is not thread-safe; drive one instance from a single thread/queue.
 *  - Pointers returned by pc_monitor_* stay valid until the next call to
 *    pc_monitor_refresh() (snapshot data) or pc_monitor_build_view() (view rows).
 *  - Unknown numeric values are reported as -1 (signed) or flagged via PC_PROC_RESTRICTED.
 *    UIs must render them as "unavailable", never as zero.
 */
#ifndef PROCYON_H
#define PROCYON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_API_VERSION 2

typedef struct pc_monitor pc_monitor;

/* ---------- capabilities ---------- */

typedef enum {
    PC_CAP_PROCESS_DISK_IO = 1u << 0, /* per-process disk read/write rates */
    PC_CAP_PROCESS_NETWORK = 1u << 1, /* per-process network rates */
    PC_CAP_MEMORY_COMPRESSED = 1u << 2,
    PC_CAP_MEMORY_PRESSURE = 1u << 3,
    PC_CAP_SWAP = 1u << 4,
    PC_CAP_HYBRID_CORES = 1u << 5, /* performance/efficiency core split is known */
} pc_capability;

uint32_t pc_capabilities(void);

/* ---------- static system info ---------- */

typedef struct {
    char os_name[64];    /* "macOS" */
    char os_version[64]; /* "27.0.1" */
    char os_build[32];   /* "26A434" */
    char kernel[256];
    char hostname[256];
    char model_id[64];    /* "Mac15,6" */
    char model_name[128]; /* "MacBook Pro" (empty when unknown) */
    char cpu_brand[128];  /* "Apple M3 Pro" */
    char arch[16];        /* "arm64" */
    int32_t physical_cores;
    int32_t logical_cores;
    int32_t performance_cores; /* 0 when not hybrid / unknown */
    int32_t efficiency_cores;
    uint64_t cpu_frequency_hz; /* 0 when unknown */
    uint64_t memory_total;     /* bytes */
    int64_t boot_time;         /* unix seconds */
} pc_system_info;

bool pc_system_info_get(pc_system_info *out);

typedef struct {
    char name[256];
    char mount_point[1024];
    char file_system[32];
    uint64_t total_bytes;
    uint64_t available_bytes;
    bool is_root;
    bool is_removable;
} pc_volume;

/* ---------- live snapshot ---------- */

typedef enum {
    PC_PROC_SYSTEM = 1u << 0,     /* owned by the OS / root; ending it needs confirmation */
    PC_PROC_RESTRICTED = 1u << 1, /* metrics unavailable without elevated privileges */
    PC_PROC_PROTECTED = 1u << 2,  /* must never be ended (kernel, init/launchd, self) */
    PC_PROC_APP_BUNDLE = 1u << 3, /* belongs to an application bundle */
} pc_process_flags;

typedef struct {
    int32_t pid;
    int32_t ppid;
    uint32_t uid;
    uint32_t flags;        /* pc_process_flags */
    double cpu_percent;    /* percent of one logical core (may exceed 100); -1 unknown */
    int64_t memory_bytes;  /* physical footprint; -1 unknown */
    double disk_read_bps;  /* -1 unknown */
    double disk_write_bps; /* -1 unknown */
    double net_rx_bps;     /* -1 unknown */
    double net_tx_bps;     /* -1 unknown */
    int32_t threads;       /* -1 unknown */
    int64_t start_time;    /* unix seconds, 0 unknown */
    char name[256];
    char user[64];
    char path[1024];
    char app_id[1024]; /* grouping key: bundle path for apps, "exe:<name>" otherwise */
    char app_name[256];
} pc_process;

typedef enum {
    PC_PRESSURE_UNKNOWN = -1,
    PC_PRESSURE_NORMAL = 0,
    PC_PRESSURE_WARNING = 1,
    PC_PRESSURE_CRITICAL = 2,
} pc_memory_pressure;

typedef struct {
    double timestamp; /* unix seconds */
    double interval;  /* seconds since previous refresh */

    /* CPU, all values 0..1 */
    double cpu_usage;
    double cpu_user;
    double cpu_system;
    int32_t core_count;
    const double *core_usage; /* core_count entries, 0..1 */
    double load_average[3];

    /* memory, bytes */
    uint64_t memory_total;
    uint64_t memory_used;
    uint64_t memory_app;
    uint64_t memory_wired;
    uint64_t memory_compressed;
    uint64_t memory_cached;
    uint64_t memory_free;
    uint64_t swap_total;
    uint64_t swap_used;
    int32_t memory_pressure; /* pc_memory_pressure */

    /* disk + network, whole machine */
    double disk_read_bps;
    double disk_write_bps;
    uint64_t disk_read_total;
    uint64_t disk_write_total;
    double net_rx_bps;
    double net_tx_bps;
    uint64_t net_rx_total;
    uint64_t net_tx_total;

    /* processes */
    int32_t process_count;
    int32_t thread_count;     /* sum over processes with known thread counts */
    int32_t restricted_count; /* processes whose metrics are unavailable (PC_PROC_RESTRICTED) */
    int32_t helper_state;     /* pc_helper_state */
    const pc_process *processes;
} pc_snapshot;

pc_monitor *pc_monitor_create(void);
void pc_monitor_destroy(pc_monitor *monitor);

/* Collects a new sample. Rates are computed against the previous refresh. */
const pc_snapshot *pc_monitor_refresh(pc_monitor *monitor);
const pc_snapshot *pc_monitor_snapshot(const pc_monitor *monitor);

/* Mounted local volumes; refreshed on every call. */
int32_t pc_monitor_volumes(pc_monitor *monitor, const pc_volume **out_volumes);

/* ---------- privileged helper ---------- */

typedef enum {
    PC_HELPER_DETACHED = 0,  /* never attached, or detached on purpose */
    PC_HELPER_CONNECTED = 1, /* restricted processes are read through the helper */
    PC_HELPER_LOST = 2,      /* was connected, the helper went away */
} pc_helper_state;

/* Connects to a running procyon-helper (see procyon/helper.h). Launching it with elevated
   privileges is the UI's job, because each OS asks for consent differently. */
bool pc_monitor_attach_helper(pc_monitor *monitor, const char *socket_path);
void pc_monitor_detach_helper(pc_monitor *monitor);
int32_t pc_monitor_helper_state(const pc_monitor *monitor);

/* ---------- process views: filter, sort, group, tree ---------- */

typedef enum {
    PC_VIEW_FLAT = 0,
    PC_VIEW_GROUPED = 1, /* by application */
    PC_VIEW_TREE = 2,    /* by parent process */
} pc_view_mode;

typedef enum {
    PC_COLUMN_NAME = 0,
    PC_COLUMN_PID,
    PC_COLUMN_USER,
    PC_COLUMN_CPU,
    PC_COLUMN_MEMORY,
    PC_COLUMN_DISK_READ,
    PC_COLUMN_DISK_WRITE,
    PC_COLUMN_NET_RX,
    PC_COLUMN_NET_TX,
    PC_COLUMN_THREADS,
} pc_column;

typedef struct {
    int32_t mode;        /* pc_view_mode */
    int32_t sort_column; /* pc_column */
    bool descending;
    const char *filter; /* UTF-8, case-insensitive substring; NULL or "" matches all */
    int32_t limit;      /* max top-level rows, 0 = unlimited */
} pc_view_query;

typedef struct {
    int32_t process_index; /* index into snapshot->processes, -1 for a group row */
    int32_t parent_row;    /* -1 for top level */
    int32_t depth;
    int32_t child_count;   /* direct children rows */
    int32_t process_count; /* processes represented (1 for a process row, n for a group) */
    /* group rows: sums of members; process rows: the process's own values */
    double cpu_percent;
    int64_t memory_bytes;
    double disk_read_bps;
    double disk_write_bps;
    double net_rx_bps;
    double net_tx_bps;
    int32_t threads;
    const char *group_id;   /* group rows only, else NULL */
    const char *group_name; /* group rows only, else NULL */
    int32_t group_pid;      /* group rows: representative pid (main executable) */
} pc_row;

/* Rows come back in display order (pre-order for trees). */
int32_t pc_monitor_build_view(pc_monitor *monitor, const pc_view_query *query, const pc_row **out_rows);

/* ---------- actions ---------- */

typedef enum {
    PC_OK = 0,
    PC_ERR_NOT_FOUND = 1,
    PC_ERR_PERMISSION = 2,
    PC_ERR_PROTECTED = 3,
    PC_ERR_FAILED = 4,
} pc_result;

/* force = false asks the process to quit (SIGTERM); true kills it (SIGKILL). */
pc_result pc_process_end(pc_monitor *monitor, int32_t pid, bool force);

/* Force-kills pid and all its descendants (children first). */
pc_result pc_process_end_tree(pc_monitor *monitor, int32_t pid);

const char *pc_result_message(pc_result result);

#ifdef __cplusplus
}
#endif

#endif /* PROCYON_H */
