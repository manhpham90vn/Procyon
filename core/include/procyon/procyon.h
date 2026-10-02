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

#define PC_API_VERSION 4

typedef struct pc_monitor pc_monitor;

/* ---------- capabilities ---------- */

typedef enum {
    PC_CAP_PROCESS_DISK_IO = 1u << 0, /* per-process disk read/write rates */
    PC_CAP_PROCESS_NETWORK = 1u << 1, /* per-process network rates */
    PC_CAP_MEMORY_COMPRESSED = 1u << 2,
    PC_CAP_MEMORY_PRESSURE = 1u << 3,
    PC_CAP_SWAP = 1u << 4,
    PC_CAP_HYBRID_CORES = 1u << 5, /* performance/efficiency core split is known */
    PC_CAP_GPU = 1u << 6,          /* pc_snapshot.gpus */
    PC_CAP_PROCESS_GPU = 1u << 7,  /* per-process GPU usage */
    PC_CAP_PRIORITY = 1u << 8,     /* pc_process_set_priority */
    PC_CAP_SUSPEND = 1u << 9,      /* pc_process_suspend / pc_process_resume */
    PC_CAP_SIGNALS = 1u << 10,     /* pc_process_signal with POSIX signal numbers */
    PC_CAP_CPU_AFFINITY = 1u << 11,
    PC_CAP_TEMPERATURE = 1u << 12,    /* pc_snapshot.cpu_temperature, disk_temperature where a sensor exists */
    PC_CAP_BATTERY = 1u << 13,        /* pc_battery_get */
    PC_CAP_SERVICES = 1u << 14,       /* pc_monitor_services, pc_service_control */
    PC_CAP_STARTUP = 1u << 15,        /* pc_monitor_startup_items, pc_startup_set_enabled */
    PC_CAP_PROCESS_ENERGY = 1u << 16, /* pc_process.power_watts */
    PC_CAP_OPEN_FILES = 1u << 17,     /* pc_monitor_open_files */
    PC_CAP_CONNECTIONS = 1u << 18,    /* pc_monitor_connections */
} pc_capability;

uint32_t pc_capabilities(void);

/* ---------- static system info ---------- */

typedef enum {
    PC_CORE_UNKNOWN = 0,
    PC_CORE_PERFORMANCE = 1,
    PC_CORE_EFFICIENCY = 2,
} pc_core_kind;

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
    /* pc_core_kind of each logical CPU, in the order of pc_snapshot.core_usage */
    uint8_t core_kinds[256];
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

typedef enum {
    PC_STATE_UNKNOWN = 0,
    PC_STATE_RUNNING = 1,
    PC_STATE_SLEEPING = 2,
    PC_STATE_STOPPED = 3, /* suspended */
    PC_STATE_ZOMBIE = 4,
} pc_process_state;

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
    double gpu_percent;    /* percent of the GPU's time (all processes sum to <= 100); -1 unknown */
    double power_watts;    /* energy the OS attributes to the process per second (CPU, GPU, ...); -1 unknown */
    int32_t nice;          /* -20 (highest priority) .. 20 */
    int32_t state;         /* pc_process_state */
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
    char name[128];      /* "Apple M3" */
    char vendor[32];     /* "Apple", "AMD", "Intel", "NVIDIA" */
    int32_t cores;       /* GPU cores, 0 unknown */
    bool unified_memory; /* shares system memory (Apple Silicon, most integrated GPUs) */
    /* 0..1, -1 unknown */
    double utilization;
    double renderer_utilization;
    double tiler_utilization;
    double encoder_utilization;
    double decoder_utilization;
    int64_t memory_used;  /* bytes, -1 unknown */
    int64_t memory_total; /* bytes of dedicated VRAM, -1 unknown or unified */
    double temperature;   /* Celsius, -1 unknown */
} pc_gpu;

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
    double cpu_temperature; /* Celsius (hottest die sensor), -1 unknown */

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
    double disk_temperature; /* Celsius (internal SSD), -1 unknown */
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

    /* GPUs; per-process GPU time is in pc_process.gpu_percent */
    int32_t gpu_count;
    const pc_gpu *gpus;
} pc_snapshot;

pc_monitor *pc_monitor_create(void);
void pc_monitor_destroy(pc_monitor *monitor);

/* Per-process sampling is on by default. Off, a refresh reads only machine-wide metrics (a tray
   widget with no window open): processes is empty and process_count 0. */
void pc_monitor_set_process_sampling(pc_monitor *monitor, bool enabled);

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
    PC_COLUMN_GPU,
    PC_COLUMN_POWER,
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
    double gpu_percent;
    double power_watts;
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
    PC_ERR_UNSUPPORTED = 5, /* not available on this OS (see pc_capabilities) */
    PC_ERR_INVALID = 6,     /* bad argument */
} pc_result;

/* force = false asks the process to quit (SIGTERM); true kills it (SIGKILL). */
pc_result pc_process_end(pc_monitor *monitor, int32_t pid, bool force);

/* Force-kills pid and all its descendants (children first). */
pc_result pc_process_end_tree(pc_monitor *monitor, int32_t pid);

/* Sends a POSIX signal number (PC_CAP_SIGNALS). Protected processes refuse every signal. */
pc_result pc_process_signal(pc_monitor *monitor, int32_t pid, int32_t signal);

/* Stops / continues every thread of the process (PC_CAP_SUSPEND). */
pc_result pc_process_suspend(pc_monitor *monitor, int32_t pid);
pc_result pc_process_resume(pc_monitor *monitor, int32_t pid);

/* nice value -20 (highest) .. 20 (lowest). Raising priority needs the helper (PC_CAP_PRIORITY). */
pc_result pc_process_set_priority(pc_monitor *monitor, int32_t pid, int32_t nice);

/* Bit i = logical CPU i (PC_CAP_CPU_AFFINITY); PC_ERR_UNSUPPORTED where the OS has no affinity. */
pc_result pc_process_set_affinity(pc_monitor *monitor, int32_t pid, uint64_t mask);

const char *pc_result_message(pc_result result);

/* ---------- process details ---------- */

typedef struct {
    uint64_t id;
    char name[64];      /* thread name, may be empty */
    double cpu_percent; /* recent usage of one core, -1 unknown */
    uint64_t user_time_ns;
    uint64_t system_time_ns;
    int32_t priority; /* current scheduling priority */
    int32_t state;    /* pc_process_state */
} pc_thread;

typedef struct {
    int32_t pid;
    int32_t ppid;
    uint32_t uid;
    int32_t nice;
    int32_t state; /* pc_process_state */
    int64_t start_time;
    char name[256];
    char user[64];
    char path[1024];
    char cwd[1024];       /* empty when unreadable */
    bool arguments_known; /* false: needs elevated privileges */
    int32_t argument_count;
    const char *const *arguments; /* argv, argument_count entries */
    int32_t environment_count;
    const char *const *environment; /* "KEY=value" */
    bool threads_known;
    int32_t thread_count;
    const pc_thread *threads;
} pc_process_details;

/* Reads path, command line, environment and threads of one process, through the helper when
   needed. The result stays valid until the next call. */
pc_result pc_process_details_get(pc_monitor *monitor, int32_t pid, const pc_process_details **out);

/* ---------- open files and network connections ---------- */

typedef enum {
    PC_FILE_REGULAR = 0,
    PC_FILE_DIRECTORY = 1,
    PC_FILE_CWD = 2,   /* the process's working directory (fd -1); keeps a volume busy like an open file */
    PC_FILE_OTHER = 3, /* devices, FIFOs */
} pc_file_kind;

typedef struct {
    int32_t pid;
    int32_t fd;   /* -1 for the working directory */
    int32_t kind; /* pc_file_kind */
    const char *path;
} pc_open_file;

typedef enum {
    PC_PROTOCOL_TCP = 0,
    PC_PROTOCOL_UDP = 1,
} pc_protocol;

typedef enum {
    PC_TCP_NONE = 0, /* UDP */
    PC_TCP_LISTEN,
    PC_TCP_SYN_SENT,
    PC_TCP_SYN_RECEIVED,
    PC_TCP_ESTABLISHED,
    PC_TCP_CLOSE_WAIT,
    PC_TCP_CLOSING, /* FIN_WAIT_1/2, CLOSING, LAST_ACK */
    PC_TCP_TIME_WAIT,
    PC_TCP_CLOSED,
} pc_tcp_state;

typedef struct {
    int32_t pid;
    int32_t protocol; /* pc_protocol */
    int32_t family;   /* 4 or 6 */
    int32_t state;    /* pc_tcp_state */
    int32_t local_port;
    int32_t remote_port;     /* 0 when not connected */
    char local_address[64];  /* "*" for any address */
    char remote_address[64]; /* empty when not connected */
} pc_connection;

/* Files (pid -1: of every process) open right now, plus working directories. Other users'
   processes are read through the helper; without it they are skipped and `*complete` is false.
   Spawns no tools but walks every descriptor: call on demand, not every tick. Valid until the
   next call. */
int32_t pc_monitor_open_files(pc_monitor *monitor, int32_t pid, const pc_open_file **out, bool *complete);
/* TCP and UDP sockets (pid -1: of every process), one entry per distinct endpoint pair. Same rules
   as pc_monitor_open_files. */
int32_t pc_monitor_connections(pc_monitor *monitor, int32_t pid, const pc_connection **out, bool *complete);

/* ---------- power ---------- */

typedef struct {
    bool present;
    double level; /* 0..1 */
    bool on_ac_power;
    bool charging;
    bool fully_charged;
    int32_t minutes_to_empty; /* -1 unknown / calculating */
    int32_t minutes_to_full;
    int32_t cycle_count;         /* -1 unknown */
    double health;               /* maximum / design capacity, 0..1, -1 unknown */
    char condition[64];          /* "Normal", "Service Recommended", … ; empty when unknown */
    int32_t design_capacity_mah; /* -1 unknown */
    int32_t max_capacity_mah;
    double temperature; /* Celsius, -1 unknown */
    double power_watts; /* negative while discharging; 0 unknown */
    char adapter[128];  /* power adapter description, empty on battery */
} pc_battery;

bool pc_battery_get(pc_battery *out);

typedef enum {
    PC_ASSERT_SYSTEM_SLEEP = 1u << 0,  /* keeps the machine awake */
    PC_ASSERT_DISPLAY_SLEEP = 1u << 1, /* keeps the display on */
} pc_assertion_kind;

typedef struct {
    int32_t pid;          /* process holding the assertion */
    int32_t on_behalf_of; /* pid it was taken for, -1 none */
    uint32_t kind;        /* pc_assertion_kind */
    char process_name[256];
    char type[64]; /* OS assertion type */
    char reason[256];
    int64_t created; /* unix seconds, 0 unknown */
} pc_power_assertion;

/* Apps currently preventing sleep. Valid until the next call. */
int32_t pc_monitor_power_assertions(pc_monitor *monitor, const pc_power_assertion **out);

/* ---------- services and startup items ---------- */

typedef enum {
    PC_DOMAIN_SYSTEM = 0, /* machine-wide (launchd system, systemd system, Windows services) */
    PC_DOMAIN_USER = 1,   /* the current user's session */
} pc_domain;

typedef struct {
    char label[256];        /* unique id within its domain */
    char name[256];         /* display name */
    char program[1024];     /* executable, empty when unknown */
    char config_path[1024]; /* definition file, empty when unknown */
    int32_t domain;         /* pc_domain */
    int32_t pid;            /* 0 when not running */
    int32_t last_exit;      /* last exit status, 0 when none/unknown */
    bool enabled;
    bool apple; /* part of the OS */
} pc_service;

typedef enum {
    PC_SERVICE_START = 0,
    PC_SERVICE_STOP = 1,
    PC_SERVICE_RESTART = 2,
    PC_SERVICE_ENABLE = 3,
    PC_SERVICE_DISABLE = 4,
} pc_service_action;

/* Every service the OS knows about. Spawns OS tools: call on demand, not every tick. */
int32_t pc_monitor_services(pc_monitor *monitor, const pc_service **out);
/* System-domain services go through the helper. */
pc_result pc_service_control(pc_monitor *monitor, int32_t domain, const char *label, int32_t action);

typedef enum {
    PC_STARTUP_USER_AGENT = 0,     /* ~/Library/LaunchAgents, XDG autostart, HKCU Run */
    PC_STARTUP_GLOBAL_AGENT = 1,   /* /Library/LaunchAgents: every user's login */
    PC_STARTUP_DAEMON = 2,         /* /Library/LaunchDaemons: at boot, as root */
    PC_STARTUP_OPEN_AT_LOGIN = 3,  /* an app the OS opens at login (macOS Login Items) */
    PC_STARTUP_APP_BACKGROUND = 4, /* an app's registered background item (helper, agent, daemon, task) */
} pc_startup_scope;

typedef struct {
    char label[256];
    char name[256]; /* app or program name */
    char program[1024];
    char config_path[1024];
    char app_path[1024];   /* owning .app bundle, empty when none */
    int32_t scope;         /* pc_startup_scope */
    int32_t pid;           /* running instance, 0 none */
    char parent_name[256]; /* the app or developer it belongs to, may be empty */
    bool enabled;
    bool run_at_load;
    bool keep_alive;
    bool managed_by_os; /* only the OS's own settings can change it: pc_startup_set_enabled refuses */
} pc_startup_item;

/* Apps and programs that start with the system or at login (third-party only). */
int32_t pc_monitor_startup_items(pc_monitor *monitor, const pc_startup_item **out);
/* What the OS registers for apps beyond launchd definitions (macOS: Open at Login and "Allow in
   the Background" items). The OS only shares this list with administrators, so it is read by the
   helper: returns -1 without it. The helper refreshes the list in the background; `*ready` is false
   (and the result possibly empty) until its first read finishes. Never blocks. Valid until the
   next call. */
int32_t pc_monitor_startup_managed_items(pc_monitor *monitor, const pc_startup_item **out, bool *ready);

/* Daemons go through the helper. */
pc_result pc_startup_set_enabled(pc_monitor *monitor, int32_t scope, const char *label, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* PROCYON_H */
