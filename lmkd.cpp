/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lowmemorykiller"
#define PERFD_LIB  "libqti-perfd-client_system.so"
#define IOPD_LIB  "libqti-iopd-client_system.so"

#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <liblmkd_utils.h>
#include <lmkd.h>
#include <log/log.h>
#include <log/log_event_list.h>
#include <log/log_time.h>
#include <private/android_filesystem_config.h>
#include <processgroup/processgroup.h>
#include <psi/psi.h>
#include <system/thread_defs.h>
#include <dlfcn.h>

#include "statslog.h"

#define BPF_FD_JUST_USE_INT
#include "BpfSyscallWrappers.h"

#include <meminfo/pageacct.h>
#include <meminfo/procmeminfo.h>

/*
 * Define LMKD_TRACE_KILLS to record lmkd kills in kernel traces
 * to profile and correlate with OOM kills
 */
#ifdef LMKD_TRACE_KILLS

#define ATRACE_TAG ATRACE_TAG_ALWAYS
#include <cutils/trace.h>

#define TRACE_KILL_START(pid) ATRACE_INT(__FUNCTION__, pid);
#define TRACE_KILL_END()      ATRACE_INT(__FUNCTION__, 0);

#else /* LMKD_TRACE_KILLS */

#define TRACE_KILL_START(pid) ((void)(pid))
#define TRACE_KILL_END() ((void)0)

#endif /* LMKD_TRACE_KILLS */

#ifndef __unused
#define __unused __attribute__((__unused__))
#endif

#define MEMCG_SYSFS_PATH "/dev/memcg/"
#define MEMCG_MEMORY_USAGE "/dev/memcg/memory.usage_in_bytes"
#define MEMCG_MEMORYSW_USAGE "/dev/memcg/memory.memsw.usage_in_bytes"
#define ZONEINFO_PATH "/proc/zoneinfo"
#define MEMINFO_PATH "/proc/meminfo"
#define VMSTAT_PATH "/proc/vmstat"
#define PROC_STATUS_TGID_FIELD "Tgid:"
#define TRACE_MARKER_PATH "/sys/kernel/tracing/trace_marker"
#define PROC_STATUS_RSS_FIELD "VmRSS:"
#define PROC_STATUS_SWAP_FIELD "VmSwap:"
#define LINE_MAX 128
#define MAX_NR_ZONES 6

#define PERCEPTIBLE_APP_ADJ 200
#define VISIBLE_APP_ADJ 100
#define PERCEPTIBLE_RECENT_FOREGROUND_APP_ADJ 50

#define INVALID_ADJ -10000
#define UNKNOWN_ADJ 1001
#define CACHED_APP_LMK_FIRST_ADJ 950
#define CACHED_APP_MIN_ADJ 900
#define SERVICE_B_ADJ 800
#define PREVIOUS_APP_ADJ 700
#define HOME_APP_ADJ 600
#define SERVICE_ADJ 500
#define HEAVY_WEIGHT_APP_ADJ 400
#define BACKUP_APP_ADJ 300
#define PERCEPTIBLE_LOW_APP_ADJ 250
#define PERCEPTIBLE_MEDIUM_APP_ADJ 225
#define FOREGROUND_APP_ADJ 0
#define PERSISTENT_SERVICE_ADJ -700
#define PERSISTENT_PROC_ADJ -800
#define NATIVE_ADJ -1000
#define NUM_OOM_LEVEL 17

static int kill_cnt_hist[NUM_OOM_LEVEL] = { 0, };

/* Android Logger event logtags (see event.logtags) */
#define KILLINFO_LOG_TAG 10195355

/* gid containing AID_SYSTEM required */
#define INKERNEL_MINFREE_PATH "/sys/module/lowmemorykiller/parameters/minfree"
#define INKERNEL_ADJ_PATH "/sys/module/lowmemorykiller/parameters/adj"

#define ARRAY_SIZE(x)   (sizeof(x) / sizeof(*(x)))
#define EIGHT_MEGA (1 << 23)

#define TARGET_UPDATE_MIN_INTERVAL_MS 1000
#define THRASHING_RESET_INTERVAL_MS 1000

#define NS_PER_MS (NS_PER_SEC / MS_PER_SEC)
#define US_PER_MS (US_PER_SEC / MS_PER_SEC)

/* Defined as ProcessList.SYSTEM_ADJ in ProcessList.java */
#define SYSTEM_ADJ (-900)

#define STRINGIFY(x) STRINGIFY_INTERNAL(x)
#define STRINGIFY_INTERNAL(x) #x

/*
 * Read lmk property with persist.device_config.lmkd_native.<name> overriding ro.lmk.<name>
 * persist.device_config.lmkd_native.* properties are being set by experiments. If a new property
 * can be controlled by an experiment then use GET_LMK_PROPERTY instead of property_get_xxx and
 * add "on property" triggers in lmkd.rc to react to the experiment flag changes.
 */
#define GET_LMK_PROPERTY(type, name, def) \
    property_get_##type("persist.device_config.lmkd_native." name, \
        property_get_##type("ro.lmk." name, def))

/*
 * PSI monitor tracking window size.
 * PSI monitor generates events at most once per window,
 * therefore we poll memory state for the duration of
 * PSI_WINDOW_SIZE_MS after the event happens.
 */
#define PSI_WINDOW_SIZE_MS 1000
/* Polling period after PSI signal when pressure is high */
#define PSI_POLL_PERIOD_SHORT_MS 10
/* Polling period after PSI signal when pressure is low */
#define PSI_POLL_PERIOD_LONG_MS 100
/* PSI complete stall for super critical events */
#define PSI_SCRIT_COMPLETE_STALL_MS (75)

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

#define FAIL_REPORT_RLIMIT_MS 1000

#define SZ_4G (0x100000000ULL)

#define PSI_PROC_TRAVERSE_DELAY_MS 200
/*
 * System property defaults
 */
/* ro.lmk.swap_free_low_percentage property defaults */
#define DEF_LOW_SWAP 10
/* ro.lmk.thrashing_limit property defaults */
#define DEF_THRASHING_LOWRAM 30
#define DEF_THRASHING 30
/* ro.lmk.thrashing_limit_decay property defaults */
#define DEF_THRASHING_DECAY_LOWRAM 50
#define DEF_THRASHING_DECAY 10
/* ro.lmk.psi_partial_stall_ms property defaults */
#define DEF_PARTIAL_STALL_LOWRAM 200
#define DEF_PARTIAL_STALL 70
/* ro.lmk.psi_complete_stall_ms property defaults */
#define DEF_COMPLETE_STALL 70

#define PSI_CONT_EVENT_THRESH (4)
#define LMKD_REINIT_PROP "lmkd.reinit"

#define PSI_OLD_LOW_THRESH_MS 70
#define PSI_OLD_MED_THRESH_MS 100
#define PSI_OLD_CRIT_THRESH_MS 70

#define NATIVE_PID_FD (-128)

/* default to old in-kernel interface if no memory pressure events */
static bool use_inkernel_interface = true;
static bool has_inkernel_module;

/* memory pressure levels */
enum vmpressure_level {
    VMPRESS_LEVEL_LOW = 0,
    VMPRESS_LEVEL_MEDIUM,
    VMPRESS_LEVEL_CRITICAL,
    VMPRESS_LEVEL_SUPER_CRITICAL,
    VMPRESS_LEVEL_COUNT
};

static const char *level_name[] = {
    "low",
    "medium",
    "critical",
    "super critical",
};

//#define __NR_hvalve 443
static int hvalve_kill_cnt[VMPRESS_LEVEL_COUNT] = { 0,};

struct {
    int64_t min_nr_free_pages; /* recorded but not used yet */
    int64_t max_nr_free_pages;
} low_pressure_mem = { -1, -1 };

struct psi_threshold {
    enum psi_stall_type stall_type;
    int threshold_ms;
};

static int level_oomadj[VMPRESS_LEVEL_COUNT];
static int mpevfd[VMPRESS_LEVEL_COUNT] = { -1, -1, -1, -1 };
static bool pidfd_supported;
static int last_kill_pid_or_fd = -1;
static struct timespec last_kill_tm;

/* lmkd configurable parameters */
static bool is_userdebug_or_eng_build;
static bool debug_process_killing;
static float cache_percent;
static bool enable_pressure_upgrade;
static int64_t upgrade_pressure;
static int64_t downgrade_pressure;
static bool low_ram_device;
static bool kill_heaviest_task;
static unsigned long kill_timeout_ms;
static int direct_reclaim_pressure = 45;
static int reclaim_scan_threshold = 1024;
static bool use_minfree_levels;
static bool force_use_old_strategy;
static bool per_app_memcg;
static bool enhance_batch_kill;
static bool enable_adaptive_lmk;
static bool enable_userspace_lmk;
static bool enable_watermark_check;
static int swap_free_low_percentage;
static int psi_partial_stall_ms;
static int psi_complete_stall_ms;
static int thrashing_limit_pct;
static int thrashing_limit_decay_pct;
static int thrashing_critical_pct;
static int swap_util_max;
static int64_t filecache_min_kb;
static int64_t stall_limit_critical;
static bool use_psi_monitors = false;
static bool enable_preferred_apps =  false;
static bool last_event_upgraded = false;
static int count_upgraded_event;
static long pa_update_timeout_ms = 60000; /* 1 min */
static int kpoll_fd;
static int psi_cont_event_thresh = PSI_CONT_EVENT_THRESH;
/* PSI window related variables */
static int psi_window_size_ms = PSI_WINDOW_SIZE_MS;
static int psi_poll_period_scrit_ms = PSI_POLL_PERIOD_SHORT_MS;
static struct psi_threshold psi_thresholds[VMPRESS_LEVEL_COUNT] = {
    { PSI_SOME, PSI_OLD_LOW_THRESH_MS },    /* Default 70ms out of 1sec for partial stall */
    { PSI_SOME, PSI_OLD_MED_THRESH_MS },   /* Default 100ms out of 1sec for partial stall */
    { PSI_FULL, PSI_OLD_CRIT_THRESH_MS },    /* Default 70ms out of 1sec for complete stall */
    { PSI_FULL, PSI_SCRIT_COMPLETE_STALL_MS }, /* Default 80ms out of 1sec for complete stall */
};
static int wmark_boost_factor = 1;
static int wbf_step = 1, wbf_effective = 1;

static android_log_context ctx;

enum polling_update {
    POLLING_DO_NOT_CHANGE,
    POLLING_START,
    POLLING_CRIT_UPGRADE,
    POLLING_PAUSE,
    POLLING_RESUME,
};

enum hvalve_state {
    IDLE,
    TRIGGER,
    KILL_PENDING,
};

static enum hvalve_state hvalve_kill_pending = IDLE;

/*
 * Data used for periodic polling for the memory state of the device.
 * Note that when system is not polling poll_handler is set to NULL,
 * when polling starts poll_handler gets set and is reset back to
 * NULL when polling stops.
 */
struct polling_params {
    struct event_handler_info* poll_handler;
    struct event_handler_info* paused_handler;
    struct timespec poll_start_tm;
    struct timespec last_poll_tm;
    int polling_interval_ms;
    enum polling_update update;
};

/* data required to handle events */
struct event_handler_info {
    int data;
    void (*handler)(int data, uint32_t events, struct polling_params *poll_params);
};

/* data required to handle socket events */
struct sock_event_handler_info {
    int sock;
    pid_t pid;
    uint32_t async_event_mask;
    struct event_handler_info handler_info;
};

/* max supported number of data connections (AMS, init, tests) */
#define MAX_DATA_CONN 3

/* socket event handler data */
static struct sock_event_handler_info ctrl_sock;
static struct sock_event_handler_info data_sock[MAX_DATA_CONN];

/* vmpressure event handler data */
static struct event_handler_info vmpressure_hinfo[VMPRESS_LEVEL_COUNT];

/*
 * 1 ctrl listen socket, 3 ctrl data socket, 3 memory pressure levels,
 * 1 lmk events + 1 fd to wait for process death
 */
#define MAX_EPOLL_EVENTS (1 + MAX_DATA_CONN + VMPRESS_LEVEL_COUNT + 1 + 1)
static int epollfd;
static int maxevents;

/* OOM score values used by both kernel and framework */
#define OOM_SCORE_ADJ_MIN       (-1000)
#define OOM_SCORE_ADJ_MAX       1000

static int lowmem_adj[MAX_TARGETS];
static int lowmem_minfree[MAX_TARGETS];
static int lowmem_targets_size;

/* Fields to parse in /proc/zoneinfo */
/* zoneinfo per-zone fields */
enum zoneinfo_zone_field {
    ZI_ZONE_NR_FREE_PAGES = 0,
    ZI_ZONE_MIN,
    ZI_ZONE_LOW,
    ZI_ZONE_HIGH,
    ZI_ZONE_PRESENT,
    ZI_ZONE_NR_FREE_CMA,
    ZI_ZONE_FIELD_COUNT
};

static const char* const zoneinfo_zone_field_names[ZI_ZONE_FIELD_COUNT] = {
    "nr_free_pages",
    "min",
    "low",
    "high",
    "present",
    "nr_free_cma",
};

/* zoneinfo per-zone special fields */
enum zoneinfo_zone_spec_field {
    ZI_ZONE_SPEC_PROTECTION = 0,
    ZI_ZONE_SPEC_PAGESETS,
    ZI_ZONE_SPEC_FIELD_COUNT,
};

static const char* const zoneinfo_zone_spec_field_names[ZI_ZONE_SPEC_FIELD_COUNT] = {
    "protection:",
    "pagesets",
};

/* see __MAX_NR_ZONES definition in kernel mmzone.h */
#define MAX_NR_ZONES 6

union zoneinfo_zone_fields {
    struct {
        int64_t nr_free_pages;
        int64_t min;
        int64_t low;
        int64_t high;
        int64_t present;
        int64_t nr_free_cma;
    } field;
    int64_t arr[ZI_ZONE_FIELD_COUNT];
};

struct zoneinfo_zone {
    union zoneinfo_zone_fields fields;
    int64_t protection[MAX_NR_ZONES];
    int64_t max_protection;
};

/* zoneinfo per-node fields */
enum zoneinfo_node_field {
    ZI_NODE_NR_INACTIVE_FILE = 0,
    ZI_NODE_NR_ACTIVE_FILE,
    ZI_NODE_FIELD_COUNT
};

static const char* const zoneinfo_node_field_names[ZI_NODE_FIELD_COUNT] = {
    "nr_inactive_file",
    "nr_active_file",
};

union zoneinfo_node_fields {
    struct {
        int64_t nr_inactive_file;
        int64_t nr_active_file;
    } field;
    int64_t arr[ZI_NODE_FIELD_COUNT];
};

struct zoneinfo_node {
    int id;
    int zone_count;
    struct zoneinfo_zone zones[MAX_NR_ZONES];
    union zoneinfo_node_fields fields;
};

/* for now two memory nodes is more than enough */
#define MAX_NR_NODES 2

struct zoneinfo {
    int node_count;
    struct zoneinfo_node nodes[MAX_NR_NODES];
    int64_t totalreserve_pages;
    int64_t total_inactive_file;
    int64_t total_active_file;
};

/* Fields to parse in /proc/meminfo */
enum meminfo_field {
    MI_NR_TOTAL_PAGES = 0,
    MI_NR_FREE_PAGES,
    MI_CACHED,
    MI_SWAP_CACHED,
    MI_BUFFERS,
    MI_SHMEM,
    MI_UNEVICTABLE,
    MI_TOTAL_SWAP,
    MI_FREE_SWAP,
    MI_ACTIVE_ANON,
    MI_INACTIVE_ANON,
    MI_ACTIVE_FILE,
    MI_INACTIVE_FILE,
    MI_SRECLAIMABLE,
    MI_SUNRECLAIM,
    MI_KERNEL_STACK,
    MI_PAGE_TABLES,
    MI_ION_HELP,
    MI_ION_HELP_POOL,
    MI_CMA_FREE,
    MI_FIELD_COUNT
};

static const char* const meminfo_field_names[MI_FIELD_COUNT] = {
    "MemTotal:",
    "MemFree:",
    "Cached:",
    "SwapCached:",
    "Buffers:",
    "Shmem:",
    "Unevictable:",
    "SwapTotal:",
    "SwapFree:",
    "Active(anon):",
    "Inactive(anon):",
    "Active(file):",
    "Inactive(file):",
    "SReclaimable:",
    "SUnreclaim:",
    "KernelStack:",
    "PageTables:",
    "ION_heap:",
    "ION_heap_pool:",
    "CmaFree:",
};

union meminfo {
    struct {
        int64_t nr_total_pages;
        int64_t nr_free_pages;
        int64_t cached;
        int64_t swap_cached;
        int64_t buffers;
        int64_t shmem;
        int64_t unevictable;
        int64_t total_swap;
        int64_t free_swap;
        int64_t active_anon;
        int64_t inactive_anon;
        int64_t active_file;
        int64_t inactive_file;
        int64_t sreclaimable;
        int64_t sunreclaimable;
        int64_t kernel_stack;
        int64_t page_tables;
        int64_t ion_heap;
        int64_t ion_heap_pool;
        int64_t cma_free;
        /* fields below are calculated rather than read from the file */
        int64_t nr_file_pages;
        int64_t total_gpu_kb;
    } field;
    int64_t arr[MI_FIELD_COUNT];
};

/* Fields to parse in /proc/vmstat */
enum vmstat_field {
    VS_FREE_PAGES,
    VS_INACTIVE_FILE,
    VS_ACTIVE_FILE,
    VS_WORKINGSET_REFAULT,
    VS_WORKINGSET_REFAULT_FILE,
    VS_PGSCAN_KSWAPD,
    VS_PGSCAN_DIRECT,
    VS_PGSCAN_DIRECT_THROTTLE,
    VS_PGSKIP_FIRST_ZONE,
    VS_PGSKIP_DMA = VS_PGSKIP_FIRST_ZONE,
    VS_PGSKIP_DMA32,
    VS_PGSKIP_NORMAL,
    VS_PGSKIP_HIGH,
    VS_PGSKIP_MOVABLE,
    VS_PGSKIP_LAST_ZONE = VS_PGSKIP_MOVABLE,
    VS_COMPACT_STALL,
    VS_FIELD_COUNT
};

#define PGSKIP_IDX(x) (x - VS_PGSKIP_FIRST_ZONE)

static const char* const vmstat_field_names[VS_FIELD_COUNT] = {
    "nr_free_pages",
    "nr_inactive_file",
    "nr_active_file",
    "workingset_refault",
    "workingset_refault_file",
    "pgscan_kswapd",
    "pgscan_direct",
    "pgscan_direct_throttle",
    "pgskip_dma",
    "pgskip_dma32",
    "pgskip_normal",
    "pgskip_high",
    "pgskip_movable",
    "compact_stall",
};

union vmstat {
    struct {
        int64_t nr_free_pages;
        int64_t nr_inactive_file;
        int64_t nr_active_file;
        int64_t workingset_refault;
        int64_t workingset_refault_file;
        int64_t pgscan_kswapd;
        int64_t pgscan_direct;
        int64_t pgscan_direct_throttle;
        int64_t pgskip_dma;
        int64_t pgskip_dma32;
        int64_t pgskip_normal;
        int64_t pgskip_high;
        int64_t pgskip_movable;
        int64_t compact_stall;
    } field;
    int64_t arr[VS_FIELD_COUNT];
};

enum field_match_result {
    NO_MATCH,
    PARSE_FAIL,
    PARSE_SUCCESS
};

struct watermark_info {
    char name[LINE_MAX];
    int free;
    int high;
    int cma;
    int present;
    int lowmem_reserve[MAX_NR_ZONES];
    int inactive_anon;
    int active_anon;
    int inactive_file;
    int active_file;
};

struct adjslot_list {
    struct adjslot_list *next;
    struct adjslot_list *prev;
};

struct proc {
    struct adjslot_list asl;
    int pid;
    int pidfd;
    uid_t uid;
    int oomadj;
    pid_t reg_pid; /* PID of the process that registered this record */
    struct proc *pidhash_next;
};

struct reread_data {
    const char* const filename;
    int fd;
};

typedef struct {
     char value[PROPERTY_VALUE_MAX];
} PropVal;


#define PREFERRED_OUT_LENGTH 12288
#define PAPP_OPCODE 10

char *preferred_apps;
void (*perf_ux_engine_trigger)(int, char *) = NULL;

#define PIDHASH_SZ 1024
static struct proc *pidhash[PIDHASH_SZ];
#define pid_hashfn(x) ((((x) >> 8) ^ (x)) & (PIDHASH_SZ - 1))

#define ADJTOSLOT(adj) ((adj) + -OOM_SCORE_ADJ_MIN)
#define ADJTOSLOT_COUNT (ADJTOSLOT(OOM_SCORE_ADJ_MAX) + 1)
static struct adjslot_list procadjslot_list[ADJTOSLOT_COUNT];

#define MAX_DISTINCT_OOM_ADJ 32
#define KILLCNT_INVALID_IDX 0xFF
/*
 * Because killcnt array is sparse a two-level indirection is used
 * to keep the size small. killcnt_idx stores index of the element in
 * killcnt array. Index KILLCNT_INVALID_IDX indicates an unused slot.
 */
static uint8_t killcnt_idx[ADJTOSLOT_COUNT];
static uint16_t killcnt[MAX_DISTINCT_OOM_ADJ];
static int killcnt_free_idx = 0;
static uint32_t killcnt_total = 0;

/* Super critical event related variables. */
static union vmstat s_crit_base;
static bool s_crit_event = false;
static bool s_crit_event_upgraded = false;

/*
 * Initialize this as we decide the window size based on ram size for
 * lowram targets on old strategy.
 */
static long page_k = PAGE_SIZE / 1024;

static void init_PreferredApps();
static void update_perf_props();

static void update_props();
static bool init_monitors();
static void destroy_monitors();

static int clamp(int low, int high, int value) {
    return max(min(value, high), low);
}

static bool parse_int64(const char* str, int64_t* ret) {
    char* endptr;
    long long val = strtoll(str, &endptr, 10);
    if (str == endptr || val > INT64_MAX) {
        return false;
    }
    *ret = (int64_t)val;
    return true;
}

static int find_field(const char* name, const char* const field_names[], int field_count) {
    for (int i = 0; i < field_count; i++) {
        if (!strcmp(name, field_names[i])) {
            return i;
        }
    }
    return -1;
}

static enum field_match_result match_field(const char* cp, const char* ap,
                                   const char* const field_names[],
                                   int field_count, int64_t* field,
                                   int *field_idx) {
    int i = find_field(cp, field_names, field_count);
    if (i < 0) {
        return NO_MATCH;
    }
    *field_idx = i;
    return parse_int64(ap, field) ? PARSE_SUCCESS : PARSE_FAIL;
}

/*
 * Read file content from the beginning up to max_len bytes or EOF
 * whichever happens first.
 */
static ssize_t read_all(int fd, char *buf, size_t max_len)
{
    ssize_t ret = 0;
    off_t offset = 0;

    while (max_len > 0) {
        ssize_t r = TEMP_FAILURE_RETRY(pread(fd, buf, max_len, offset));
        if (r == 0) {
            break;
        }
        if (r == -1) {
            return -1;
        }
        ret += r;
        buf += r;
        offset += r;
        max_len -= r;
    }

    return ret;
}

/*
 * Read a new or already opened file from the beginning.
 * If the file has not been opened yet data->fd should be set to -1.
 * To be used with files which are read often and possibly during high
 * memory pressure to minimize file opening which by itself requires kernel
 * memory allocation and might result in a stall on memory stressed system.
 */
static char *reread_file(struct reread_data *data) {
    /* start with page-size buffer and increase if needed */
    static ssize_t buf_size = PAGE_SIZE;
    static char *new_buf, *buf = NULL;
    ssize_t size;

    if (data->fd == -1) {
        /* First-time buffer initialization */
        if (!buf && (buf = static_cast<char*>(malloc(buf_size))) == nullptr) {
            return NULL;
        }

        data->fd = TEMP_FAILURE_RETRY(open(data->filename, O_RDONLY | O_CLOEXEC));
        if (data->fd < 0) {
            ALOGE("%s open: %s", data->filename, strerror(errno));
            return NULL;
        }
    }

    while (true) {
        size = read_all(data->fd, buf, buf_size - 1);
        if (size < 0) {
            ALOGE("%s read: %s", data->filename, strerror(errno));
            close(data->fd);
            data->fd = -1;
            return NULL;
        }
        if (size < buf_size - 1) {
            break;
        }
        /*
         * Since we are reading /proc files we can't use fstat to find out
         * the real size of the file. Double the buffer size and keep retrying.
         */
        if ((new_buf = static_cast<char*>(realloc(buf, buf_size * 2))) == nullptr) {
            errno = ENOMEM;
            return NULL;
        }
        buf = new_buf;
        buf_size *= 2;
    }
    buf[size] = 0;

    return buf;
}

static bool claim_record(struct proc* procp, pid_t pid) {
    if (procp->reg_pid == pid) {
        /* Record already belongs to the registrant */
        return true;
    }
    if (procp->reg_pid == 0) {
        /* Old registrant is gone, claim the record */
        procp->reg_pid = pid;
        return true;
    }
    /* The record is owned by another registrant */
    return false;
}

static void remove_claims(pid_t pid) {
    int i;

    for (i = 0; i < PIDHASH_SZ; i++) {
        struct proc* procp = pidhash[i];
        while (procp) {
            if (procp->reg_pid == pid) {
                procp->reg_pid = 0;
            }
            procp = procp->pidhash_next;
        }
    }
}

static void ctrl_data_close(int dsock_idx) {
    struct epoll_event epev;

    ALOGI("closing lmkd data connection");
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, data_sock[dsock_idx].sock, &epev) == -1) {
        // Log a warning and keep going
        ALOGW("epoll_ctl for data connection socket failed; errno=%d", errno);
    }
    maxevents--;

    close(data_sock[dsock_idx].sock);
    data_sock[dsock_idx].sock = -1;

    /* Mark all records of the old registrant as unclaimed */
    remove_claims(data_sock[dsock_idx].pid);
}

static ssize_t ctrl_data_read(int dsock_idx, char* buf, size_t bufsz, struct ucred* sender_cred) {
    struct iovec iov = {buf, bufsz};
    char control[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr hdr = {
            NULL, 0, &iov, 1, control, sizeof(control), 0,
    };
    ssize_t ret;
    ret = TEMP_FAILURE_RETRY(recvmsg(data_sock[dsock_idx].sock, &hdr, 0));
    if (ret == -1) {
        ALOGE("control data socket read failed; %s", strerror(errno));
        return -1;
    }
    if (ret == 0) {
        ALOGE("Got EOF on control data socket");
        return -1;
    }

    struct ucred* cred = NULL;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
    while (cmsg != NULL) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
            cred = (struct ucred*)CMSG_DATA(cmsg);
            break;
        }
        cmsg = CMSG_NXTHDR(&hdr, cmsg);
    }

    if (cred == NULL) {
        ALOGE("Failed to retrieve sender credentials");
        /* Close the connection */
        ctrl_data_close(dsock_idx);
        return -1;
    }

    memcpy(sender_cred, cred, sizeof(struct ucred));

    /* Store PID of the peer */
    data_sock[dsock_idx].pid = cred->pid;

    return ret;
}

static int ctrl_data_write(int dsock_idx, char* buf, size_t bufsz) {
    int ret = 0;

    ret = TEMP_FAILURE_RETRY(write(data_sock[dsock_idx].sock, buf, bufsz));

    if (ret == -1) {
        ALOGE("control data socket write failed; errno=%d", errno);
    } else if (ret == 0) {
        ALOGE("Got EOF on control data socket");
        ret = -1;
    }

    return ret;
}

/*
 * Write the pid/uid pair over the data socket, note: all active clients
 * will receive this unsolicited notification.
 */
static void ctrl_data_write_lmk_kill_occurred(pid_t pid, uid_t uid) {
    LMKD_CTRL_PACKET packet;
    size_t len = lmkd_pack_set_prockills(packet, pid, uid);

    for (int i = 0; i < MAX_DATA_CONN; i++) {
        if (data_sock[i].sock >= 0 && data_sock[i].async_event_mask & 1 << LMK_ASYNC_EVENT_KILL) {
            ctrl_data_write(i, (char*)packet, len);
        }
    }
}

/*
 * Write the kill_stat/memory_stat over the data socket to be propagated via AMS to statsd
 */
static void stats_write_lmk_kill_occurred(struct kill_stat *kill_st,
                                          struct memory_stat *mem_st) {
    LMK_KILL_OCCURRED_PACKET packet;
    const size_t len = lmkd_pack_set_kill_occurred(packet, kill_st, mem_st);
    if (len == 0) {
        return;
    }

    for (int i = 0; i < MAX_DATA_CONN; i++) {
        if (data_sock[i].sock >= 0 && data_sock[i].async_event_mask & 1 << LMK_ASYNC_EVENT_STAT) {
            ctrl_data_write(i, packet, len);
        }
    }

}

static void stats_write_lmk_kill_occurred_pid(int pid, struct kill_stat *kill_st,
                                              struct memory_stat *mem_st) {
    kill_st->taskname = stats_get_task_name(pid);
    if (kill_st->taskname != NULL) {
        stats_write_lmk_kill_occurred(kill_st, mem_st);
    }
}

/*
 * Write the state_changed over the data socket to be propagated via AMS to statsd
 */
static void stats_write_lmk_state_changed(enum lmk_state state) {
    LMKD_CTRL_PACKET packet_state_changed;
    const size_t len = lmkd_pack_set_state_changed(packet_state_changed, state);
    if (len == 0) {
        return;
    }
    for (int i = 0; i < MAX_DATA_CONN; i++) {
        if (data_sock[i].sock >= 0 && data_sock[i].async_event_mask & 1 << LMK_ASYNC_EVENT_STAT) {
            ctrl_data_write(i, (char*)packet_state_changed, len);
        }
    }
}

static void poll_kernel(int poll_fd) {
    if (poll_fd == -1) {
        // not waiting
        return;
    }

    while (1) {
        char rd_buf[256];
        int bytes_read = TEMP_FAILURE_RETRY(pread(poll_fd, (void*)rd_buf, sizeof(rd_buf), 0));
        if (bytes_read <= 0) break;
        rd_buf[bytes_read] = '\0';

        int64_t pid;
        int64_t uid;
        int64_t group_leader_pid;
        int64_t rss_in_pages;
        struct memory_stat mem_st = {};
        int16_t oom_score_adj;
        int16_t min_score_adj;
        int64_t starttime;
        char* taskname = 0;

        int fields_read =
                sscanf(rd_buf,
                       "%" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64
                       " %" SCNd16 " %" SCNd16 " %" SCNd64 "\n%m[^\n]",
                       &pid, &uid, &group_leader_pid, &mem_st.pgfault, &mem_st.pgmajfault,
                       &rss_in_pages, &oom_score_adj, &min_score_adj, &starttime, &taskname);

        /* only the death of the group leader process is logged */
        if (fields_read == 10 && group_leader_pid == pid) {
            ctrl_data_write_lmk_kill_occurred((pid_t)pid, (uid_t)uid);
            mem_st.process_start_time_ns = starttime * (NS_PER_SEC / sysconf(_SC_CLK_TCK));
            mem_st.rss_in_bytes = rss_in_pages * PAGE_SIZE;

            struct kill_stat kill_st = {
                .uid = static_cast<int32_t>(uid),
                .kill_reason = NONE,
                .oom_score = oom_score_adj,
                .min_oom_score = min_score_adj,
                .free_mem_kb = 0,
                .free_swap_kb = 0,
            };
            stats_write_lmk_kill_occurred_pid(pid, &kill_st, &mem_st);
        }

        free(taskname);
    }
}

static bool init_poll_kernel() {
    kpoll_fd = TEMP_FAILURE_RETRY(open("/proc/lowmemorykiller", O_RDONLY | O_NONBLOCK | O_CLOEXEC));

    if (kpoll_fd < 0) {
        ALOGE("kernel lmk event file could not be opened; errno=%d", errno);
        return false;
    }

    return true;
}

static struct proc *pid_lookup(int pid) {
    struct proc *procp;

    for (procp = pidhash[pid_hashfn(pid)]; procp && procp->pid != pid;
         procp = procp->pidhash_next)
            ;

    return procp;
}

static void adjslot_insert(struct adjslot_list *head, struct adjslot_list *new_element)
{
    struct adjslot_list *next = head->next;
    new_element->prev = head;
    new_element->next = next;
    next->prev = new_element;
    head->next = new_element;
}

static void adjslot_remove(struct adjslot_list *old)
{
    struct adjslot_list *prev = old->prev;
    struct adjslot_list *next = old->next;
    next->prev = prev;
    prev->next = next;
}

static struct adjslot_list *adjslot_tail(struct adjslot_list *head) {
    struct adjslot_list *asl = head->prev;

    return asl == head ? NULL : asl;
}

static void proc_slot(struct proc *procp) {
    int adjslot = ADJTOSLOT(procp->oomadj);

    adjslot_insert(&procadjslot_list[adjslot], &procp->asl);
}

static void proc_unslot(struct proc *procp) {
    adjslot_remove(&procp->asl);
}

static void proc_insert(struct proc *procp) {
    int hval = pid_hashfn(procp->pid);

    procp->pidhash_next = pidhash[hval];
    pidhash[hval] = procp;
    proc_slot(procp);
}

static int pid_remove(int pid) {
    int hval = pid_hashfn(pid);
    struct proc *procp;
    struct proc *prevp;

    for (procp = pidhash[hval], prevp = NULL; procp && procp->pid != pid;
         procp = procp->pidhash_next)
            prevp = procp;

    if (!procp)
        return -1;

    if (!prevp)
        pidhash[hval] = procp->pidhash_next;
    else
        prevp->pidhash_next = procp->pidhash_next;

    proc_unslot(procp);
    /*
     * Close pidfd here if we are not waiting for corresponding process to die,
     * in which case stop_wait_for_proc_kill() will close the pidfd later
     */
    if (procp->pidfd >= 0 && procp->pidfd != last_kill_pid_or_fd) {
        close(procp->pidfd);
    }

    if (procp->pidfd != NATIVE_PID_FD) {
        free(procp);
    } else {
        memset(procp, 0, sizeof(struct proc));
    }

    return 0;
}

/*
 * Write a string to a file.
 * Returns false if the file does not exist.
 */
static bool writefilestring(const char *path, const char *s,
                            bool err_if_missing) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t len = strlen(s);
    ssize_t ret;

    if (fd < 0) {
        if (err_if_missing) {
            ALOGE("Error opening %s; errno=%d", path, errno);
        }
        return false;
    }

    ret = TEMP_FAILURE_RETRY(write(fd, s, len));
    if (ret < 0) {
        ALOGE("Error writing %s; errno=%d", path, errno);
    } else if (ret < len) {
        ALOGE("Short write on %s; length=%zd", path, ret);
    }

    close(fd);
    return true;
}

static inline long get_time_diff_ms(struct timespec *from,
                                    struct timespec *to) {
    return (to->tv_sec - from->tv_sec) * (long)MS_PER_SEC +
           (to->tv_nsec - from->tv_nsec) / (long)NS_PER_MS;
}

/* Reads /proc/pid/status into buf. */
static bool read_proc_status(int pid, char *buf, size_t buf_sz) {
    static char path[PATH_MAX];
    int fd;
    ssize_t size;

    snprintf(path, PATH_MAX, "/proc/%d/status", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    size = read_all(fd, buf, buf_sz - 1);
    close(fd);
    if (size < 0) {
        return false;
    }
    buf[size] = 0;
    return true;
}

/* Looks for tag in buf and parses the first integer */
static bool parse_status_tag(char *buf, const char *tag, int64_t *out) {
    char *pos = buf;
    while (true) {
        pos = strstr(pos, tag);
        /* Stop if tag not found or found at the line beginning */
        if (pos == NULL || pos == buf || pos[-1] == '\n') {
            break;
        }
        pos++;
    }

    if (pos == NULL) {
        return false;
    }

    pos += strlen(tag);
    while (*pos == ' ') ++pos;
    return parse_int64(pos, out);
}

static long proc_get_rss(int pid) {
    static char path[PATH_MAX];
    static char line[LINE_MAX];
    int fd;
    long rss = 0;
    long total;
    ssize_t ret;

    /* gid containing AID_READPROC required */
    snprintf(path, PATH_MAX, "/proc/%d/statm", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        return -1;
    }

    ret = read_all(fd, line, sizeof(line) - 1);
    if (ret < 0) {
        close(fd);
        return -1;
    }

    sscanf(line, "%ld %ld ", &total, &rss);
    close(fd);
    return rss;
}

static bool parse_vmswap(char *buf, long *data) {

    if (sscanf(buf, "VmSwap: %ld", data) == 1) {
        return 1;
    }

    return 0;
}

static long proc_get_swap(int pid) {
    static char buf[PAGE_SIZE] = {0, };
    static char path[PATH_MAX] = {0, };
    ssize_t ret;
    char *c, *save_ptr;
    int fd;
    long data;

    snprintf(path, PATH_MAX, "/proc/%d/status", pid);
    fd = open(path,  O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    ret = read_all(fd, buf, sizeof(buf) - 1);
    if (ret < 0) {
        ALOGE("unable to read Vm status");
        data = 0;
        goto out;
    }

    for(c = strtok_r(buf, "\n", &save_ptr); c;
        c = strtok_r(NULL, "\n", &save_ptr)) {
        if (parse_vmswap(c, &data)){
            goto out;
        }
    }

    ALOGE("Couldn't get Swap info. Is it kthread?");
    data = 0;
out:
    close(fd);
    /* Vmswap is in Kb. Convert to page size. */
    return (data >> 2);
}

static long proc_get_size(int pid)
{
    long size;

    return (size = proc_get_rss(pid)) ? size : proc_get_swap(pid);
}

static long proc_get_vm(int pid) {
    static char path[PATH_MAX];
    static char line[LINE_MAX];
    int fd;
    long total;
    ssize_t ret;

    /* gid containing AID_READPROC required */
    snprintf(path, PATH_MAX, "/proc/%d/statm", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        return -1;
    }

    ret = read_all(fd, line, sizeof(line) - 1);
    if (ret < 0) {
        close(fd);
        return -1;
    }

    sscanf(line, "%ld", &total);
    close(fd);
    return total;
}

static char *proc_get_name(int pid, char *buf, size_t buf_size) {
    static char path[PATH_MAX];
    int fd;
    char *cp;
    ssize_t ret;

    /* gid containing AID_READPROC required */
    snprintf(path, PATH_MAX, "/proc/%d/cmdline", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        return NULL;
    }
    ret = read_all(fd, buf, buf_size - 1);
    close(fd);
    if (ret < 0) {
        return NULL;
    }
    buf[ret] = '\0';

    cp = strchr(buf, ' ');
    if (cp) {
        *cp = '\0';
    }

    return buf;
}

static void cmd_procprio(LMKD_CTRL_PACKET packet, int field_count, struct ucred *cred) {
    struct proc *procp;
    char path[LINE_MAX];
    char val[20];
    int soft_limit_mult;
    struct lmk_procprio params;
    bool is_system_server;
    struct passwd *pwdrec;
    int64_t tgid;
    static char buf[PAGE_SIZE];

    lmkd_pack_get_procprio(packet, field_count, &params);

    if (params.oomadj < OOM_SCORE_ADJ_MIN ||
        params.oomadj > OOM_SCORE_ADJ_MAX) {
        ALOGE("Invalid PROCPRIO oomadj argument %d", params.oomadj);
        return;
    }

    if (params.ptype < PROC_TYPE_FIRST || params.ptype >= PROC_TYPE_COUNT) {
        ALOGE("Invalid PROCPRIO process type argument %d", params.ptype);
        return;
    }

    /* Check if registered process is a thread group leader */
    if (read_proc_status(params.pid, buf, sizeof(buf))) {
        if (parse_status_tag(buf, PROC_STATUS_TGID_FIELD, &tgid) && tgid != params.pid) {
            ALOGE("Attempt to register a task that is not a thread group leader "
                  "(tid %d, tgid %" PRId64 ")", params.pid, tgid);
            return;
        }
    }

    /* gid containing AID_READPROC required */
    /* CAP_SYS_RESOURCE required */
    /* CAP_DAC_OVERRIDE required */
    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", params.pid);
    snprintf(val, sizeof(val), "%d", params.oomadj);
    if (!writefilestring(path, val, false)) {
        ALOGW("Failed to open %s; errno=%d: process %d might have been killed",
              path, errno, params.pid);
        /* If this file does not exist the process is dead. */
        return;
    }

    if (use_inkernel_interface) {
        stats_store_taskname(params.pid, proc_get_name(params.pid, path, sizeof(path)));
        return;
    }

    /* lmkd should not change soft limits for services */
    if (params.ptype == PROC_TYPE_APP && per_app_memcg) {
        if (params.oomadj >= 900) {
            soft_limit_mult = 0;
        } else if (params.oomadj >= 800) {
            soft_limit_mult = 0;
        } else if (params.oomadj >= 700) {
            soft_limit_mult = 0;
        } else if (params.oomadj >= 600) {
            // Launcher should be perceptible, don't kill it.
            params.oomadj = 200;
            soft_limit_mult = 1;
        } else if (params.oomadj >= 500) {
            soft_limit_mult = 0;
        } else if (params.oomadj >= 400) {
            soft_limit_mult = 0;
        } else if (params.oomadj >= 300) {
            soft_limit_mult = 1;
        } else if (params.oomadj >= 200) {
            soft_limit_mult = 8;
        } else if (params.oomadj >= 100) {
            soft_limit_mult = 10;
        } else if (params.oomadj >=   0) {
            soft_limit_mult = 20;
        } else {
            // Persistent processes will have a large
            // soft limit 512MB.
            soft_limit_mult = 64;
        }

        snprintf(path, sizeof(path), MEMCG_SYSFS_PATH
                 "apps/uid_%d/pid_%d/memory.soft_limit_in_bytes",
                 params.uid, params.pid);
        snprintf(val, sizeof(val), "%d", soft_limit_mult * EIGHT_MEGA);

        /*
         * system_server process has no memcg under /dev/memcg/apps but should be
         * registered with lmkd. This is the best way so far to identify it.
         */
        is_system_server = (params.oomadj == SYSTEM_ADJ &&
                            (pwdrec = getpwnam("system")) != NULL &&
                            params.uid == pwdrec->pw_uid);
        writefilestring(path, val, !is_system_server);
    }

    procp = pid_lookup(params.pid);
    if (!procp) {
        int pidfd = -1;

        if (pidfd_supported) {
            pidfd = TEMP_FAILURE_RETRY(pidfd_open(params.pid, 0));
            if (pidfd < 0) {
                ALOGE("pidfd_open for pid %d failed; errno=%d", params.pid, errno);
                return;
            }
        }

        procp = static_cast<struct proc*>(calloc(1, sizeof(struct proc)));
        if (!procp) {
            // Oh, the irony.  May need to rebuild our state.
            return;
        }

        procp->pid = params.pid;
        procp->pidfd = pidfd;
        procp->uid = params.uid;
        procp->reg_pid = cred->pid;
        procp->oomadj = params.oomadj;
        proc_insert(procp);
    } else {
        if (!claim_record(procp, cred->pid)) {
            char buf[LINE_MAX];
            char *taskname = proc_get_name(cred->pid, buf, sizeof(buf));
            /* Only registrant of the record can remove it */
            ALOGE("%s (%d, %d) attempts to modify a process registered by another client",
                taskname ? taskname : "A process ", cred->uid, cred->pid);
            return;
        }
        proc_unslot(procp);
        procp->oomadj = params.oomadj;
        proc_slot(procp);
    }
}

static void cmd_procremove(LMKD_CTRL_PACKET packet, struct ucred *cred) {
    struct lmk_procremove params;
    struct proc *procp;

    lmkd_pack_get_procremove(packet, &params);
    //ALOGI("CMD_PROCREMOVE");
    if (use_inkernel_interface) {
        /*
         * Perform an extra check before the pid is removed, after which it
         * will be impossible for poll_kernel to get the taskname. poll_kernel()
         * is potentially a long-running blocking function; however this method
         * handles AMS requests but does not block AMS.
         */
        poll_kernel(kpoll_fd);

        stats_remove_taskname(params.pid);
        return;
    }

    procp = pid_lookup(params.pid);
    if (!procp) {
        return;
    }

    if (!claim_record(procp, cred->pid)) {
        char buf[LINE_MAX];
        char *taskname = proc_get_name(cred->pid, buf, sizeof(buf));
        /* Only registrant of the record can remove it */
        ALOGE("%s (%d, %d) attempts to unregister a process registered by another client",
            taskname ? taskname : "A process ", cred->uid, cred->pid);
        return;
    }

    /*
     * WARNING: After pid_remove() procp is freed and can't be used!
     * Therefore placed at the end of the function.
     */
    pid_remove(params.pid);
}

static void cmd_procpurge(struct ucred *cred) {
    int i;
    struct proc *procp;
    struct proc *next;

    ALOGI("CMD_PROCPURGE");

    if (use_inkernel_interface) {
        stats_purge_tasknames();
        return;
    }

    for (i = 0; i < PIDHASH_SZ; i++) {
        procp = pidhash[i];
        while (procp) {
            next = procp->pidhash_next;
            /* Purge only records created by the requestor */
            if (claim_record(procp, cred->pid)) {
                pid_remove(procp->pid);
            }
            procp = next;
        }
    }
}

static void cmd_subscribe(int dsock_idx, LMKD_CTRL_PACKET packet) {
    struct lmk_subscribe params;

    lmkd_pack_get_subscribe(packet, &params);

    ALOGI("CMD_SUBSCRIBE");
    data_sock[dsock_idx].async_event_mask |= 1 << params.evt_type;
}

static void inc_killcnt(int oomadj) {
    int slot = ADJTOSLOT(oomadj);
    uint8_t idx = killcnt_idx[slot];

    if (idx == KILLCNT_INVALID_IDX) {
        /* index is not assigned for this oomadj */
        if (killcnt_free_idx < MAX_DISTINCT_OOM_ADJ) {
            killcnt_idx[slot] = killcnt_free_idx;
            killcnt[killcnt_free_idx] = 1;
            killcnt_free_idx++;
        } else {
            ALOGW("Number of distinct oomadj levels exceeds %d",
                MAX_DISTINCT_OOM_ADJ);
        }
    } else {
        /*
         * wraparound is highly unlikely and is detectable using total
         * counter because it has to be equal to the sum of all counters
         */
        killcnt[idx]++;
    }
    /* increment total kill counter */
    killcnt_total++;
}

static int get_killcnt(int min_oomadj, int max_oomadj) {
    int slot;
    int count = 0;

    if (min_oomadj > max_oomadj)
        return 0;

    /* special case to get total kill count */
    if (min_oomadj > OOM_SCORE_ADJ_MAX)
        return killcnt_total;

    while (min_oomadj <= max_oomadj &&
           (slot = ADJTOSLOT(min_oomadj)) < ADJTOSLOT_COUNT) {
        uint8_t idx = killcnt_idx[slot];
        if (idx != KILLCNT_INVALID_IDX) {
            count += killcnt[idx];
        }
        min_oomadj++;
    }

    return count;
}

static int cmd_getkillcnt(LMKD_CTRL_PACKET packet) {
    struct lmk_getkillcnt params;

    if (use_inkernel_interface) {
        /* kernel driver does not expose this information */
        return 0;
    }

    lmkd_pack_get_getkillcnt(packet, &params);

    ALOGI("CMD_GETKILLCNT");
    return get_killcnt(params.min_oomadj, params.max_oomadj);
}

static void cmd_target(int ntargets, LMKD_CTRL_PACKET packet) {
    int i;
    struct lmk_target target;
    char minfree_str[PROPERTY_VALUE_MAX];
    char *pstr = minfree_str;
    char *pend = minfree_str + sizeof(minfree_str);
    static struct timespec last_req_tm;
    struct timespec curr_tm;

    if (ntargets < 1 || ntargets > (int)ARRAY_SIZE(lowmem_adj))
        return;

    /*
     * Ratelimit minfree updates to once per TARGET_UPDATE_MIN_INTERVAL_MS
     * to prevent DoS attacks
     */
    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm) != 0) {
        ALOGE("Failed to get current time");
        return;
    }

    if (get_time_diff_ms(&last_req_tm, &curr_tm) <
        TARGET_UPDATE_MIN_INTERVAL_MS) {
        ALOGE("Ignoring frequent updated to lmkd limits");
        return;
    }

    last_req_tm = curr_tm;

    for (i = 0; i < ntargets; i++) {
        lmkd_pack_get_target(packet, i, &target);
        lowmem_minfree[i] = target.minfree;
        lowmem_adj[i] = target.oom_adj_score;

        pstr += snprintf(pstr, pend - pstr, "%d:%d,", target.minfree,
            target.oom_adj_score);
        if (pstr >= pend) {
            /* if no more space in the buffer then terminate the loop */
            pstr = pend;
            break;
        }
    }

    ALOGI("CMD_TARGET");
    lowmem_targets_size = ntargets;

    /* Override the last extra comma */
    pstr[-1] = '\0';
    property_set("sys.lmk.minfree_levels", minfree_str);

    if (has_inkernel_module) {
        char minfreestr[128];
        char killpriostr[128];

        minfreestr[0] = '\0';
        killpriostr[0] = '\0';

        for (i = 0; i < lowmem_targets_size; i++) {
            char val[40];

            if (i) {
                strlcat(minfreestr, ",", sizeof(minfreestr));
                strlcat(killpriostr, ",", sizeof(killpriostr));
            }

            snprintf(val, sizeof(val), "%d", use_inkernel_interface ? lowmem_minfree[i] : 0);
            strlcat(minfreestr, val, sizeof(minfreestr));
            snprintf(val, sizeof(val), "%d", use_inkernel_interface ? lowmem_adj[i] : 0);
            strlcat(killpriostr, val, sizeof(killpriostr));
        }

        writefilestring(INKERNEL_MINFREE_PATH, minfreestr, true);
        writefilestring(INKERNEL_ADJ_PATH, killpriostr, true);
    }
}

static void ctrl_command_handler(int dsock_idx) {
    LMKD_CTRL_PACKET packet;
    struct ucred cred;
    int len;
    enum lmk_cmd cmd;
    int nargs;
    int targets;
    int kill_cnt;
    int result;

    len = ctrl_data_read(dsock_idx, (char *)packet, CTRL_PACKET_MAX_SIZE, &cred);
    if (len <= 0)
        return;

    if (len < (int)sizeof(int)) {
        ALOGE("Wrong control socket read length len=%d", len);
        return;
    }

    cmd = lmkd_pack_get_cmd(packet);
    nargs = len / sizeof(int) - 1;
    if (nargs < 0)
        goto wronglen;

    switch(cmd) {
    case LMK_TARGET:
        targets = nargs / 2;
        if (nargs & 0x1 || targets > (int)ARRAY_SIZE(lowmem_adj))
            goto wronglen;
        cmd_target(targets, packet);
        break;
    case LMK_PROCPRIO:
        /* process type field is optional for backward compatibility */
        if (nargs < 3 || nargs > 4)
            goto wronglen;
        cmd_procprio(packet, nargs, &cred);
        break;
    case LMK_PROCREMOVE:
        if (nargs != 1)
            goto wronglen;
        cmd_procremove(packet, &cred);
        break;
    case LMK_PROCPURGE:
        if (nargs != 0)
            goto wronglen;
        cmd_procpurge(&cred);
        break;
    case LMK_GETKILLCNT:
        if (nargs != 2)
            goto wronglen;
        kill_cnt = cmd_getkillcnt(packet);
        len = lmkd_pack_set_getkillcnt_repl(packet, kill_cnt);
        if (ctrl_data_write(dsock_idx, (char *)packet, len) != len)
            return;
        break;
    case LMK_SUBSCRIBE:
        if (nargs != 1)
            goto wronglen;
        cmd_subscribe(dsock_idx, packet);
        break;
    case LMK_PROCKILL:
        /* This command code is NOT expected at all */
        ALOGE("Received unexpected command code %d", cmd);
        break;
    case LMK_UPDATE_PROPS:
        if (nargs != 0)
            goto wronglen;
        update_props();
        if (!use_inkernel_interface) {
            /* Reinitialize monitors to apply new settings */
            destroy_monitors();
            result = init_monitors() ? 0 : -1;
        } else {
            result = 0;
        }
        len = lmkd_pack_set_update_props_repl(packet, result);
        if (ctrl_data_write(dsock_idx, (char *)packet, len) != len) {
            ALOGE("Failed to report operation results");
        }
        if (!result) {
            ALOGI("Properties reinitilized");
        } else {
            /* New settings can't be supported, crash to be restarted */
            ALOGE("New configuration is not supported. Exiting...");
            exit(1);
        }
        break;
    default:
        ALOGE("Received unknown command code %d", cmd);
        return;
    }

    return;

wronglen:
    ALOGE("Wrong control socket read length cmd=%d len=%d", cmd, len);
}

static void ctrl_data_handler(int data, uint32_t events,
                              struct polling_params *poll_params __unused) {
    if (events & EPOLLIN) {
        ctrl_command_handler(data);
    }
}

static int get_free_dsock() {
    for (int i = 0; i < MAX_DATA_CONN; i++) {
        if (data_sock[i].sock < 0) {
            return i;
        }
    }
    return -1;
}

static void ctrl_connect_handler(int data __unused, uint32_t events __unused,
                                 struct polling_params *poll_params __unused) {
    struct epoll_event epev;
    int free_dscock_idx = get_free_dsock();

    if (free_dscock_idx < 0) {
        /*
         * Number of data connections exceeded max supported. This should not
         * happen but if it does we drop all existing connections and accept
         * the new one. This prevents inactive connections from monopolizing
         * data socket and if we drop ActivityManager connection it will
         * immediately reconnect.
         */
        for (int i = 0; i < MAX_DATA_CONN; i++) {
            ctrl_data_close(i);
        }
        free_dscock_idx = 0;
    }

    data_sock[free_dscock_idx].sock = accept(ctrl_sock.sock, NULL, NULL);
    if (data_sock[free_dscock_idx].sock < 0) {
        ALOGE("lmkd control socket accept failed; errno=%d", errno);
        return;
    }

    ALOGI("lmkd data connection established");
    /* use data to store data connection idx */
    data_sock[free_dscock_idx].handler_info.data = free_dscock_idx;
    data_sock[free_dscock_idx].handler_info.handler = ctrl_data_handler;
    data_sock[free_dscock_idx].async_event_mask = 0;
    epev.events = EPOLLIN;
    epev.data.ptr = (void *)&(data_sock[free_dscock_idx].handler_info);
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, data_sock[free_dscock_idx].sock, &epev) == -1) {
        ALOGE("epoll_ctl for data connection socket failed; errno=%d", errno);
        ctrl_data_close(free_dscock_idx);
        return;
    }
    maxevents++;
}

/*
 * /proc/zoneinfo parsing routines
 * Expected file format is:
 *
 *   Node <node_id>, zone   <zone_name>
 *   (
 *    per-node stats
 *       (<per-node field name> <value>)+
 *   )?
 *   (pages free     <value>
 *       (<per-zone field name> <value>)+
 *    pagesets
 *       (<unused fields>)*
 *   )+
 *   ...
 */
static void zoneinfo_parse_protection(char *buf, struct zoneinfo_zone *zone) {
    int zone_idx;
    int64_t max = 0;
    char *save_ptr;

    for (buf = strtok_r(buf, "(), ", &save_ptr), zone_idx = 0;
         buf && zone_idx < MAX_NR_ZONES;
         buf = strtok_r(NULL, "), ", &save_ptr), zone_idx++) {
        long long zoneval = strtoll(buf, &buf, 0);
        if (zoneval > max) {
            max = (zoneval > INT64_MAX) ? INT64_MAX : zoneval;
        }
        zone->protection[zone_idx] = zoneval;
    }
    zone->max_protection = max;
}

static int zoneinfo_parse_zone(char **buf, struct zoneinfo_zone *zone) {
    for (char *line = strtok_r(NULL, "\n", buf); line;
         line = strtok_r(NULL, "\n", buf)) {
        char *cp;
        char *ap;
        char *save_ptr;
        int64_t val;
        int field_idx;
        enum field_match_result match_res;

        cp = strtok_r(line, " ", &save_ptr);
        if (!cp) {
            return false;
        }

        field_idx = find_field(cp, zoneinfo_zone_spec_field_names, ZI_ZONE_SPEC_FIELD_COUNT);
        if (field_idx >= 0) {
            /* special field */
            if (field_idx == ZI_ZONE_SPEC_PAGESETS) {
                /* no mode fields we are interested in */
                return true;
            }

            /* protection field */
            ap = strtok_r(NULL, ")", &save_ptr);
            if (ap) {
                zoneinfo_parse_protection(ap, zone);
            }
            continue;
        }

        ap = strtok_r(NULL, " ", &save_ptr);
        if (!ap) {
            continue;
        }

        match_res = match_field(cp, ap, zoneinfo_zone_field_names, ZI_ZONE_FIELD_COUNT,
            &val, &field_idx);
        if (match_res == PARSE_FAIL) {
            return false;
        }
        if (match_res == PARSE_SUCCESS) {
            zone->fields.arr[field_idx] = val;
        }
        if (field_idx == ZI_ZONE_PRESENT && val == 0) {
            /* zone is not populated, stop parsing it */
            return true;
        }
    }
    return false;
}

static int zoneinfo_parse_node(char **buf, struct zoneinfo_node *node) {
    int fields_to_match = ZI_NODE_FIELD_COUNT;

    for (char *line = strtok_r(NULL, "\n", buf); line;
         line = strtok_r(NULL, "\n", buf)) {
        char *cp;
        char *ap;
        char *save_ptr;
        int64_t val;
        int field_idx;
        enum field_match_result match_res;

        cp = strtok_r(line, " ", &save_ptr);
        if (!cp) {
            return false;
        }

        ap = strtok_r(NULL, " ", &save_ptr);
        if (!ap) {
            return false;
        }

        match_res = match_field(cp, ap, zoneinfo_node_field_names, ZI_NODE_FIELD_COUNT,
            &val, &field_idx);
        if (match_res == PARSE_FAIL) {
            return false;
        }
        if (match_res == PARSE_SUCCESS) {
            node->fields.arr[field_idx] = val;
            fields_to_match--;
            if (!fields_to_match) {
                return true;
            }
        }
    }
    return false;
}

static int zoneinfo_parse(struct zoneinfo *zi) {
    static struct reread_data file_data = {
        .filename = ZONEINFO_PATH,
        .fd = -1,
    };
    char *buf;
    char *save_ptr;
    char *line;
    char zone_name[LINE_MAX + 1];
    struct zoneinfo_node *node = NULL;
    int node_idx = 0;
    int zone_idx = 0;

    memset(zi, 0, sizeof(struct zoneinfo));

    if ((buf = reread_file(&file_data)) == NULL) {
        return -1;
    }

    for (line = strtok_r(buf, "\n", &save_ptr); line;
         line = strtok_r(NULL, "\n", &save_ptr)) {
        int node_id;
        if (sscanf(line, "Node %d, zone %" STRINGIFY(LINE_MAX) "s", &node_id, zone_name) == 2) {
            if (!node || node->id != node_id) {
                /* new node is found */
                if (node) {
                    node->zone_count = zone_idx + 1;
                    node_idx++;
                    if (node_idx == MAX_NR_NODES) {
                        /* max node count exceeded */
                        ALOGE("%s parse error", file_data.filename);
                        return -1;
                    }
                }
                node = &zi->nodes[node_idx];
                node->id = node_id;
                zone_idx = 0;
                if (!zoneinfo_parse_node(&save_ptr, node)) {
                    ALOGE("%s parse error", file_data.filename);
                    return -1;
                }
            } else {
                /* new zone is found */
                zone_idx++;
            }
            if (!zoneinfo_parse_zone(&save_ptr, &node->zones[zone_idx])) {
                ALOGE("%s parse error", file_data.filename);
                return -1;
            }
        }
    }
    if (!node) {
        ALOGE("%s parse error", file_data.filename);
        return -1;
    }
    node->zone_count = zone_idx + 1;
    zi->node_count = node_idx + 1;

    /* calculate totals fields */
    for (node_idx = 0; node_idx < zi->node_count; node_idx++) {
        node = &zi->nodes[node_idx];
        for (zone_idx = 0; zone_idx < node->zone_count; zone_idx++) {
            struct zoneinfo_zone *zone = &zi->nodes[node_idx].zones[zone_idx];
            zi->totalreserve_pages += zone->max_protection + zone->fields.field.high;
        }
        zi->total_inactive_file += node->fields.field.nr_inactive_file;
        zi->total_active_file += node->fields.field.nr_active_file;
    }
    return 0;
}

/* /proc/meminfo parsing routines */
static bool meminfo_parse_line(char *line, union meminfo *mi) {
    char *cp = line;
    char *ap;
    char *save_ptr;
    int64_t val;
    int field_idx;
    enum field_match_result match_res;

    cp = strtok_r(line, " ", &save_ptr);
    if (!cp) {
        return false;
    }

    ap = strtok_r(NULL, " ", &save_ptr);
    if (!ap) {
        return false;
    }

    match_res = match_field(cp, ap, meminfo_field_names, MI_FIELD_COUNT,
        &val, &field_idx);
    if (match_res == PARSE_SUCCESS) {
        mi->arr[field_idx] = val / page_k;
    }
    return (match_res != PARSE_FAIL);
}

static int64_t read_gpu_total_kb() {
    static int fd = android::bpf::bpfFdGet(
            "/sys/fs/bpf/map_gpu_mem_gpu_mem_total_map", BPF_F_RDONLY);
    static constexpr uint64_t kBpfKeyGpuTotalUsage = 0;
    uint64_t value;

    if (fd < 0) {
        return 0;
    }

    return android::bpf::findMapEntry(fd, &kBpfKeyGpuTotalUsage, &value)
            ? 0
            : (int32_t)(value / 1024);
}

static int meminfo_parse(union meminfo *mi) {
    static struct reread_data file_data = {
        .filename = MEMINFO_PATH,
        .fd = -1,
    };
    char *buf;
    char *save_ptr;
    char *line;

    memset(mi, 0, sizeof(union meminfo));

    if ((buf = reread_file(&file_data)) == NULL) {
        return -1;
    }

    for (line = strtok_r(buf, "\n", &save_ptr); line;
         line = strtok_r(NULL, "\n", &save_ptr)) {
        if (!meminfo_parse_line(line, mi)) {
            ALOGE("%s parse error", file_data.filename);
            return -1;
        }
    }
    mi->field.nr_file_pages = mi->field.cached + mi->field.swap_cached +
        mi->field.buffers;
    mi->field.total_gpu_kb = read_gpu_total_kb();

    return 0;
}

/* /proc/vmstat parsing routines */
static bool vmstat_parse_line(char *line, union vmstat *vs) {
    char *cp;
    char *ap;
    char *save_ptr;
    int64_t val;
    int field_idx;
    enum field_match_result match_res;

    cp = strtok_r(line, " ", &save_ptr);
    if (!cp) {
        return false;
    }

    ap = strtok_r(NULL, " ", &save_ptr);
    if (!ap) {
        return false;
    }

    match_res = match_field(cp, ap, vmstat_field_names, VS_FIELD_COUNT,
        &val, &field_idx);
    if (match_res == PARSE_SUCCESS) {
        vs->arr[field_idx] = val;
    }
    return (match_res != PARSE_FAIL);
}

static int vmstat_parse(union vmstat *vs) {
    static struct reread_data file_data = {
        .filename = VMSTAT_PATH,
        .fd = -1,
    };
    char *buf;
    char *save_ptr;
    char *line;
    int i;

    memset(vs, 0, sizeof(union vmstat));

    /*
     * Per-zone related info need not present. Prefill them.
     * If exist, they can be overridden. This change helps
     * us to check which all zone info we can look into.
     */
    for (i = VS_PGSKIP_FIRST_ZONE; i <= VS_PGSKIP_LAST_ZONE; i++)
        vs->arr[i] = -EINVAL;
    if ((buf = reread_file(&file_data)) == NULL) {
        return -1;
    }

    for (line = strtok_r(buf, "\n", &save_ptr); line;
         line = strtok_r(NULL, "\n", &save_ptr)) {
        if (!vmstat_parse_line(line, vs)) {
            ALOGE("%s parse error", file_data.filename);
            return -1;
        }
    }

    return 0;
}

static int psi_parse(struct reread_data *file_data, struct psi_stats stats[], bool full) {
    char *buf;
    char *save_ptr;
    char *line;

    if ((buf = reread_file(file_data)) == NULL) {
        return -1;
    }

    line = strtok_r(buf, "\n", &save_ptr);
    if (parse_psi_line(line, PSI_SOME, stats)) {
        return -1;
    }
    if (full) {
        line = strtok_r(NULL, "\n", &save_ptr);
        if (parse_psi_line(line, PSI_FULL, stats)) {
            return -1;
        }
    }

    return 0;
}

static int psi_parse_mem(struct psi_data *psi_data) {
    static struct reread_data file_data = {
        .filename = PSI_PATH_MEMORY,
        .fd = -1,
    };
    return psi_parse(&file_data, psi_data->mem_stats, true);
}

static int psi_parse_io(struct psi_data *psi_data) {
    static struct reread_data file_data = {
        .filename = PSI_PATH_IO,
        .fd = -1,
    };
    return psi_parse(&file_data, psi_data->io_stats, true);
}

static int psi_parse_cpu(struct psi_data *psi_data) {
    static struct reread_data file_data = {
        .filename = PSI_PATH_CPU,
        .fd = -1,
    };
    return psi_parse(&file_data, psi_data->cpu_stats, false);
}

enum wakeup_reason {
    Event,
    Polling
};

struct wakeup_info {
    struct timespec wakeup_tm;
    struct timespec prev_wakeup_tm;
    struct timespec last_event_tm;
    int wakeups_since_event;
    int skipped_wakeups;
};

/*
 * After the initial memory pressure event is received lmkd schedules periodic wakeups to check
 * the memory conditions and kill if needed (polling). This is done because pressure events are
 * rate-limited and memory conditions can change in between events. Therefore after the initial
 * event there might be multiple wakeups. This function records the wakeup information such as the
 * timestamps of the last event and the last wakeup, the number of wakeups since the last event
 * and how many of those wakeups were skipped (some wakeups are skipped if previously killed
 * process is still freeing its memory).
 */
static void record_wakeup_time(struct timespec *tm, enum wakeup_reason reason,
                               struct wakeup_info *wi) {
    wi->prev_wakeup_tm = wi->wakeup_tm;
    wi->wakeup_tm = *tm;
    if (reason == Event) {
        wi->last_event_tm = *tm;
        wi->wakeups_since_event = 0;
        wi->skipped_wakeups = 0;
    } else {
        wi->wakeups_since_event++;
    }
}

static void killinfo_log(struct proc* procp, int min_oom_score, int rss_kb,
                         int swap_kb, int kill_reason, union meminfo *mi,
                         struct wakeup_info *wi, struct timespec *tm,
                         struct psi_data *pd) {
    /* log process information */
    android_log_write_int32(ctx, procp->pid);
    android_log_write_int32(ctx, procp->uid);
    android_log_write_int32(ctx, procp->oomadj);
    android_log_write_int32(ctx, min_oom_score);
    android_log_write_int32(ctx, (int32_t)min(rss_kb, INT32_MAX));
    android_log_write_int32(ctx, kill_reason);

    /* log meminfo fields */
    for (int field_idx = 0; field_idx < MI_FIELD_COUNT; field_idx++) {
        android_log_write_int32(ctx, (int32_t)min(mi->arr[field_idx] * page_k, INT32_MAX));
    }

    /* log lmkd wakeup information */
    android_log_write_int32(ctx, (int32_t)get_time_diff_ms(&wi->last_event_tm, tm));
    android_log_write_int32(ctx, (int32_t)get_time_diff_ms(&wi->prev_wakeup_tm, tm));
    android_log_write_int32(ctx, wi->wakeups_since_event);
    android_log_write_int32(ctx, wi->skipped_wakeups);
    android_log_write_int32(ctx, (int32_t)min(swap_kb, INT32_MAX));
    android_log_write_int32(ctx, (int32_t)mi->field.total_gpu_kb);

    if (pd) {
        android_log_write_float32(ctx, pd->mem_stats[PSI_SOME].avg10);
        android_log_write_float32(ctx, pd->mem_stats[PSI_FULL].avg10);
        android_log_write_float32(ctx, pd->io_stats[PSI_SOME].avg10);
        android_log_write_float32(ctx, pd->io_stats[PSI_FULL].avg10);
        android_log_write_float32(ctx, pd->cpu_stats[PSI_SOME].avg10);
    } else {
        for (int i = 0; i < 5; i++) {
            android_log_write_float32(ctx, 0);
        }
    }

    android_log_write_list(ctx, LOG_ID_EVENTS);
    android_log_reset(ctx);
}

/*
 * no strtok_r since that modifies buffer and we want to use multiline sscanf
 */
static char *nextln(char *buf)
{
    char *x;

    x = static_cast<char*>(memchr(buf, '\n', strlen(buf)));
    if (!x) {
        return buf + strlen(buf);
    }
    return x + 1;
}

static int parse_one_zone_watermark(char *buf, struct watermark_info *w)
{
    char *start = buf;
    int nargs;
    int ret = 0;

    while (*buf) {
        nargs = sscanf(buf, "Node %*u, zone %" STRINGIFY(LINE_MAX) "s", w->name);
        buf = nextln(buf);
        if (nargs == 1) {
            break;
        }
    }

    while(*buf) {
        nargs = sscanf(buf,
                    " pages free %d"
                    " min %*d"
                    " low %*d"
                    " high %d"
                    " spanned %*d"
                    " present %d"
                    " managed %*d",
                    &w->free, &w->high, &w->present);
        buf = nextln(buf);
        if (nargs == 3) {
            break;
        }
    }

    while(*buf) {
        nargs = sscanf(buf,
                    " protection: (%d, %d, %d, %d, %d, %d)",
                    &w->lowmem_reserve[0], &w->lowmem_reserve[1],
                    &w->lowmem_reserve[2], &w->lowmem_reserve[3],
                    &w->lowmem_reserve[4], &w->lowmem_reserve[5]);
        buf = nextln(buf);
        if (nargs >= 1) {
            break;
        }
    }

    while(*buf) {
        nargs = sscanf(buf,
                    " nr_zone_inactive_anon %d"
                    " nr_zone_active_anon %d"
                    " nr_zone_inactive_file %d"
                    " nr_zone_active_file %d",
                    &w->inactive_anon, &w->active_anon,
                    &w->inactive_file, &w->active_file);
        buf = nextln(buf);
        if (nargs == 4) {
            break;
        }
    }

    while (*buf) {
        nargs = sscanf(buf, " nr_free_cma %u", &w->cma);
        buf = nextln(buf);
        if (nargs == 1) {
            ret = buf - start;
            break;
        }
    }

    return ret;
}

static void trace_log(const char *fmt, ...)
{
    char buf[PAGE_SIZE];
    va_list ap;
    static int fd = -1;
    ssize_t len, ret;

    if (fd < 0) {
        fd = open(TRACE_MARKER_PATH, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            ALOGE("Error opening " TRACE_MARKER_PATH "; errno=%d",
                errno);
            return;
        }
    }

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    len = strlen(buf);
    ret = TEMP_FAILURE_RETRY(write(fd, buf, len));
    if (ret < 0) {
        if (errno != EBADF) {
            ALOGE("Error writing " TRACE_MARKER_PATH ";errno=%d", errno);
            close(fd);
            fd = -1;
        }
        return;
    } else if (ret < len) {
        ALOGE("Short write on " TRACE_MARKER_PATH "; length=%zd", ret);
    }
}

#define ULMK_LOG(X, fmt...) ({ \
    ALOG##X(fmt);              \
    trace_log(fmt);            \
})

static int file_cache_to_adj(enum vmpressure_level __unused lvl, int nr_free,
int nr_file)
{
    int min_score_adj = OOM_SCORE_ADJ_MAX + 1;
    int minfree;
    int i;
    int crit_minfree;
    int s_crit_adj_level = level_oomadj[VMPRESS_LEVEL_SUPER_CRITICAL];

    /*
     * Below condition is to catch the zones where the file pages
     * are not allowed to, eg: Movable zone.
     * A corner case is where file_cache = 0 in the allowed zones
     * which is a very rare scenario.
     */
    if (!nr_file) {
        goto out;
    }

    for (i = 0; i < lowmem_targets_size; i++) {
        minfree = lowmem_minfree[i];
        if (nr_file < minfree) {
            min_score_adj = lowmem_adj[i];
            break;
        }
    }

    crit_minfree = lowmem_minfree[lowmem_targets_size - 1];
    if (lowmem_targets_size >= 2) {
        crit_minfree = lowmem_minfree[lowmem_targets_size - 1] +
                    (lowmem_minfree[lowmem_targets_size - 1] -
                    lowmem_minfree[lowmem_targets_size - 2]);
    }

    /* Adjust the selected adj in accordance with pressure. */
    if (s_crit_event && !s_crit_event_upgraded && (min_score_adj > s_crit_adj_level)) {
        min_score_adj = s_crit_adj_level;
    } else {
        if (s_crit_event_upgraded &&
                nr_free < lowmem_minfree[lowmem_targets_size -1] &&
                nr_file < crit_minfree &&
                min_score_adj > s_crit_adj_level) {
            min_score_adj = s_crit_adj_level;
        }
    }

out:
    /*
     * If event is upgraded, just allow one kill in that window. This
     * is to avoid the aggressiveness of kills by upgrading the event.
     */
    if (s_crit_event_upgraded) {
        s_crit_event_upgraded = s_crit_event = false;
    }
    if (debug_process_killing) {
        ULMK_LOG(E, "adj:%d file_cache: %d\n", min_score_adj, nr_file);
    }
    return min_score_adj;
}

/*
 * Returns OOM_XCORE_ADJ_MAX + 1  on parsing error.
 */
static int zone_watermarks_ok(enum vmpressure_level level)
{
    static struct reread_data file_data = {
        .filename = ZONEINFO_PATH,
        .fd = -1,
    };
    char *buf;
    char *offset;
    struct watermark_info w[MAX_NR_ZONES];
    static union vmstat vs1, vs2;
    int zone_id, i, nr, present_zones = 0;
    bool lowmem_reserve_ok[MAX_NR_ZONES];
    int nr_file = 0;
    int min_score_adj = OOM_SCORE_ADJ_MAX + 1;

    if ((buf = reread_file(&file_data)) == NULL) {
        return min_score_adj;
    }

    memset(&w, 0, sizeof(w));
    memset(&lowmem_reserve_ok, 0, sizeof(lowmem_reserve_ok));
    offset = buf;

    /* Parse complete zone info. */
    for (zone_id = 0; zone_id < MAX_NR_ZONES; zone_id++, present_zones++) {
        nr = parse_one_zone_watermark(offset, &w[zone_id]);
        if (!nr) {
            break;
        }
        offset += nr;
    }
    if (!present_zones) {
        goto out;
    }

    if (vmstat_parse(&vs1) < 0) {
        ULMK_LOG(E, "Failed to parse vmstat!");
        goto out;
    }

    for (zone_id = 0, i = VS_PGSKIP_FIRST_ZONE;
            i <= VS_PGSKIP_LAST_ZONE && zone_id < present_zones; ++i) {
        if (vs1.arr[i] == -EINVAL) {
            continue;
        }
        /*
         * If no page is skipped while reclaiming, then consider this
         * zone file cache stats.
         */
        if (!(vs1.arr[i] - vs2.arr[i])) {
            nr_file += w[zone_id].inactive_file + w[zone_id].active_file;
        }

        ++zone_id;
    }

    vs2 = vs1;
    for (zone_id = 0; zone_id < present_zones; zone_id++) {
        int margin;

        if (debug_process_killing) {
            ULMK_LOG(D, "Zone %s: free:%d high:%d cma:%d reserve:(%d %d %d)"
                " anon:(%d %d) file:(%d %d)\n",
                w[zone_id].name, w[zone_id].free, w[zone_id].high, w[zone_id].cma,
                w[zone_id].lowmem_reserve[0], w[zone_id].lowmem_reserve[1],
                w[zone_id].lowmem_reserve[2],
                w[zone_id].inactive_anon, w[zone_id].active_anon,
                w[zone_id].inactive_file, w[zone_id].active_file);
        }

        /* Zone is empty */
        if (!w[zone_id].present) {
            continue;
        }

        margin = w[zone_id].free - w[zone_id].cma - w[zone_id].high;
        for (i = 0; i < present_zones; i++)
            if (w[zone_id].lowmem_reserve[i] && (margin > w[zone_id].lowmem_reserve[i])) {
                lowmem_reserve_ok[i] = true;
            }

        if (!s_crit_event && (margin >= 0 || lowmem_reserve_ok[zone_id])) {
            continue;
        }

        return file_cache_to_adj(level, w[zone_id].free, nr_file);
    }

out:
    if (offset == buf) {
        ALOGE("Parsing watermarks failed in %s", file_data.filename);
    }

    return min_score_adj;
}

static struct proc *proc_adj_lru(int oomadj) {
    return (struct proc *)adjslot_tail(&procadjslot_list[ADJTOSLOT(oomadj)]);
}

static struct proc *proc_get_heaviest(int oomadj) {
    struct adjslot_list *head = &procadjslot_list[ADJTOSLOT(oomadj)];
    struct adjslot_list *curr = head->next;
    struct proc *maxprocp = NULL;
    int maxsize = 0;

    /* Filter out PApps */
    struct proc *maxprocp_pa = NULL;
    int maxsize_pa = 0;
    char *tmp_taskname;
    char buf[LINE_MAX];

    while (curr != head) {
        int pid = ((struct proc *)curr)->pid;
        long tasksize = proc_get_size(pid);
        if (tasksize < 0) {
            struct adjslot_list *next = curr->next;
            pid_remove(pid);
            curr = next;
        } else {
            tmp_taskname = proc_get_name(pid, buf, sizeof(buf));
            if (enable_preferred_apps && tmp_taskname != NULL && strstr(preferred_apps, tmp_taskname)) {
                if (tasksize > maxsize_pa) {
                    maxsize_pa = tasksize;
                    maxprocp_pa = (struct proc *)curr;
                }
            } else {
                if (tasksize > maxsize) {
                    maxsize = tasksize;
                    maxprocp = (struct proc *)curr;
                }
            }
            curr = curr->next;
        }
    }
    if (maxsize > 0) {
        return maxprocp;
    } else {
        return maxprocp_pa;
    }
}


static void set_process_group_and_prio(uid_t uid, int pid,
                                       const std::vector<std::string>& profiles, int prio) {
    DIR* d;
    char proc_path[PATH_MAX];
    struct dirent* de;

    if (!SetProcessProfilesCached(uid, pid, profiles)) {
        ALOGW("Failed to set task profiles for the process (%d) being killed", pid);
    }

    snprintf(proc_path, sizeof(proc_path), "/proc/%d/task", pid);
    if (!(d = opendir(proc_path))) {
        ALOGW("Failed to open %s; errno=%d: process pid(%d) might have died", proc_path, errno,
              pid);
        return;
    }

    while ((de = readdir(d))) {
        int t_pid;

        if (de->d_name[0] == '.') continue;
        t_pid = atoi(de->d_name);

        if (!t_pid) {
            ALOGW("Failed to get t_pid for '%s' of pid(%d)", de->d_name, pid);
            continue;
        }

        if (setpriority(PRIO_PROCESS, t_pid, prio) && errno != ESRCH) {
            ALOGW("Unable to raise priority of killing t_pid (%d): errno=%d", t_pid, errno);
        }
    }
    closedir(d);
}

/*
 * Allow lmkd to "find" shell scripts with oom_score_adj >= 0
 * Since we are not informed when a shell script exit, the generated
 * list may be obsolete. This case is handled by the loop in
 * find_and_kill_processes.
 */
static long proc_get_script(enum vmpressure_level level)
{
    static DIR* d = NULL;
    struct dirent* de;
    static char path[PATH_MAX];
    static char line[LINE_MAX];
    ssize_t len;
    int fd, oomadj = OOM_SCORE_ADJ_MIN;
    int r;
    uint32_t pid;
    long total_vm;
    long tasksize = 0;
    static bool retry_eligible = false;
    struct timespec curr_tm;
    static struct timespec last_traverse_time;
    static bool check_time = false;

    if (check_time) {
        clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
        if (get_time_diff_ms(&last_traverse_time, &curr_tm) <
                PSI_PROC_TRAVERSE_DELAY_MS) {
            return 0;
        }
    }
repeat:
    if (!d && !(d = opendir("/proc"))) {
        ALOGE("Failed to open /proc");
        return 0;
    }

    while ((de = readdir(d))) {
        if (sscanf(de->d_name, "%u", &pid) != 1) {
            continue;
        }

        /* Don't attempt to kill init */
        if (pid == 1) {
            continue;
        }

        /*
     * Don't attempt to kill kthreads. Rely on total_vm for this.
     */
        total_vm = proc_get_vm(pid);
        if (total_vm <= 0) {
            continue;
        }

        snprintf(path, sizeof(path), "/proc/%u/oom_score_adj", pid);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        len = read_all(fd, line, sizeof(line) - 1);
        close(fd);

        if (len < 0) {
            continue;
        }

        line[LINE_MAX - 1] = '\0';

        if (sscanf(line, "%d", &oomadj) != 1) {
            ALOGE("Parsing oomadj %s failed", line);
            continue;
        }

        if (oomadj < 0) {
            continue;
        }

        tasksize = proc_get_size(pid);
        if (tasksize <= 0) {
            continue;
        }

        retry_eligible = true;
        check_time = false;
        r = kill(pid, SIGKILL);
        if (r) {
            ALOGE("kill(%d): errno=%d", pid, errno);
            tasksize = 0;
        } else {
            ULMK_LOG(I, "Kill native with pid %u, oom_adj %d, to free %ld pages",
                            pid, oomadj, tasksize);
            hvalve_kill_cnt[level]++;
        }

        return tasksize;
    }
    closedir(d);
    d = NULL;
    if (retry_eligible) {
        retry_eligible = false;
        goto repeat;
    }
    check_time = true;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &last_traverse_time);
    ALOGI("proc_get_script: No tasks are found to kill");

    return 0;
}

static bool is_kill_pending(void) {
    char buf[24];

    if (last_kill_pid_or_fd < 0) {
        return false;
    }

    if (pidfd_supported) {
        return true;
    }

    /* when pidfd is not supported base the decision on /proc/<pid> existence */
    snprintf(buf, sizeof(buf), "/proc/%d/", last_kill_pid_or_fd);
    if (access(buf, F_OK) == 0) {
        return true;
    }

    return false;
}

static bool is_waiting_for_kill(void) {
    return pidfd_supported && last_kill_pid_or_fd >= 0;
}

static void stop_wait_for_proc_kill(bool finished) {
    struct epoll_event epev;

    if (last_kill_pid_or_fd < 0) {
        return;
    }

    if (debug_process_killing) {
        struct timespec curr_tm;

        if (clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm) != 0) {
            /*
             * curr_tm is used here merely to report kill duration, so this failure is not fatal.
             * Log an error and continue.
             */
            ALOGE("Failed to get current time");
        }

        if (finished) {
            ALOGI("Process got killed in %ldms",
                get_time_diff_ms(&last_kill_tm, &curr_tm));
        } else {
            ALOGI("Stop waiting for process kill after %ldms",
                get_time_diff_ms(&last_kill_tm, &curr_tm));
        }
    }

    if (pidfd_supported) {
        /* unregister fd */
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, last_kill_pid_or_fd, &epev)) {
            // Log an error and keep going
            ALOGE("epoll_ctl for last killed process failed; errno=%d", errno);
        }
        maxevents--;
        close(last_kill_pid_or_fd);
    }

    last_kill_pid_or_fd = -1;
}

static void kill_done_handler(int data __unused, uint32_t events __unused,
                              struct polling_params *poll_params) {
    stop_wait_for_proc_kill(true);
    poll_params->update = POLLING_RESUME;
}

static void start_wait_for_proc_kill(int pid_or_fd) {
    static struct event_handler_info kill_done_hinfo = { 0, kill_done_handler };
    struct epoll_event epev;

    if (last_kill_pid_or_fd >= 0) {
        /* Should not happen but if it does we should stop previous wait */
        ALOGE("Attempt to wait for a kill while another wait is in progress");
        stop_wait_for_proc_kill(false);
    }

    last_kill_pid_or_fd = pid_or_fd;

    if (!pidfd_supported) {
        /* If pidfd is not supported just store PID and exit */
        return;
    }

    epev.events = EPOLLIN;
    epev.data.ptr = (void *)&kill_done_hinfo;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, last_kill_pid_or_fd, &epev) != 0) {
        ALOGE("epoll_ctl for last kill failed; errno=%d", errno);
        close(last_kill_pid_or_fd);
        last_kill_pid_or_fd = -1;
        return;
    }
    maxevents++;
}

struct kill_info {
    enum kill_reasons kill_reason;
    const char *kill_desc;
    int thrashing;
    int max_thrashing;
};

/* Kill one process specified by procp.  Returns the size (in pages) of the process killed */
static int kill_one_process(struct proc* procp, int min_oom_score, struct kill_info *ki,
                            union meminfo *mi, struct wakeup_info *wi, struct timespec *tm,
                            struct psi_data *pd) {
    int pid = procp->pid;
    int pidfd = procp->pidfd;
    uid_t uid = procp->uid;
    char *taskname;
    int r;
    int result = -1;
    struct memory_stat *mem_st;
    struct kill_stat kill_st;
    int64_t tgid;
    int64_t rss_kb;
    int64_t swap_kb;
    static char buf[PAGE_SIZE];
    int oomadj_score;

    if (!read_proc_status(pid, buf, sizeof(buf))) {
        goto out;
    }
    if (!parse_status_tag(buf, PROC_STATUS_TGID_FIELD, &tgid)) {
        ALOGE("Unable to parse tgid from /proc/%d/status", pid);
        goto out;
    }
    if (tgid != pid) {
        ALOGE("Possible pid reuse detected (pid %d, tgid %" PRId64 ")!", pid, tgid);
        goto out;
    }
    // Zombie processes will not have RSS / Swap fields.
    if (!parse_status_tag(buf, PROC_STATUS_RSS_FIELD, &rss_kb)) {
        goto out;
    }
    if (!parse_status_tag(buf, PROC_STATUS_SWAP_FIELD, &swap_kb)) {
        goto out;
    }
    if (procp) {
        android::meminfo::ProcMemInfo proc_mem(pid);
        const android::meminfo::MemUsage& usage = proc_mem.Usage();
        ALOGI("LMKD_STAT: pid: %d rss: %lu uss: %lu", procp->pid, usage.rss, usage.uss);
    }
    if (hvalve_kill_pending != KILL_PENDING) {
        hvalve_kill_pending = KILL_PENDING;
        syscall(__NR_hvalve, 5, procp->uid, rss_kb, procp->oomadj);
        return -987654321;
    }

    taskname = proc_get_name(pid, buf, sizeof(buf));
    // taskname will point inside buf, do not reuse buf onwards.
    if (!taskname) {
        goto out;
    }

    mem_st = stats_read_memory_stat(per_app_memcg, pid, uid, rss_kb * 1024, swap_kb * 1024);

    TRACE_KILL_START(pid);

    /* CAP_KILL required */
    if (pidfd < 0) {
        start_wait_for_proc_kill(pid);
        r = kill(pid, SIGKILL);
    } else {
        start_wait_for_proc_kill(pidfd);
        r = pidfd_send_signal(pidfd, SIGKILL, NULL, 0);
    }

    oomadj_score = procp->oomadj;

    if (oomadj_score >= CACHED_APP_MIN_ADJ)
        kill_cnt_hist[0]++;
    else if (oomadj_score >= SERVICE_B_ADJ)
        kill_cnt_hist[1]++;
    else if (oomadj_score >= PREVIOUS_APP_ADJ)
        kill_cnt_hist[2]++;
    else if (oomadj_score >= HOME_APP_ADJ)
        kill_cnt_hist[3]++;
    else if (oomadj_score >= SERVICE_ADJ)
        kill_cnt_hist[4]++;
    else if (oomadj_score >= HEAVY_WEIGHT_APP_ADJ)
        kill_cnt_hist[5]++;
    else if (oomadj_score >= BACKUP_APP_ADJ)
        kill_cnt_hist[6]++;
    else if (oomadj_score >= PERCEPTIBLE_LOW_APP_ADJ)
        kill_cnt_hist[7]++;
    else if (oomadj_score >= PERCEPTIBLE_MEDIUM_APP_ADJ)
        kill_cnt_hist[8]++;
    else if (oomadj_score >= PERCEPTIBLE_APP_ADJ)
        kill_cnt_hist[9]++;
    else if (oomadj_score >= VISIBLE_APP_ADJ)
        kill_cnt_hist[10]++;
    else if (oomadj_score >= PERCEPTIBLE_RECENT_FOREGROUND_APP_ADJ)
        kill_cnt_hist[11]++;
    else if (oomadj_score >= FOREGROUND_APP_ADJ)
        kill_cnt_hist[12]++;
    else if (oomadj_score >= PERSISTENT_SERVICE_ADJ)
        kill_cnt_hist[13]++;
    else if (oomadj_score >= PERSISTENT_PROC_ADJ)
        kill_cnt_hist[14]++;
    else if (oomadj_score >= SYSTEM_ADJ)
        kill_cnt_hist[15]++;
    else
        kill_cnt_hist[16]++;

    ULMK_LOG(I,"KILL_HIST: pid: %d, uid: %d, hist: %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d",
            pid, uid,
            kill_cnt_hist[0], kill_cnt_hist[1], kill_cnt_hist[2], kill_cnt_hist[3], kill_cnt_hist[4],
            kill_cnt_hist[5], kill_cnt_hist[6], kill_cnt_hist[7], kill_cnt_hist[8], kill_cnt_hist[9],
            kill_cnt_hist[10], kill_cnt_hist[11], kill_cnt_hist[12], kill_cnt_hist[13], kill_cnt_hist[14],
            kill_cnt_hist[15], kill_cnt_hist[16]);

    TRACE_KILL_END();

    if (r) {
        stop_wait_for_proc_kill(false);
        ALOGE("kill(%d): errno=%d", pid, errno);
        /* Delete process record even when we fail to kill so that we don't get stuck on it */
        goto out;
    }

    set_process_group_and_prio(uid, pid, {"CPUSET_SP_FOREGROUND", "SCHED_SP_FOREGROUND"},
                               ANDROID_PRIORITY_HIGHEST);

    last_kill_tm = *tm;

    inc_killcnt(procp->oomadj);

    if (ki) {
        kill_st.kill_reason = ki->kill_reason;
        kill_st.thrashing = ki->thrashing;
        kill_st.max_thrashing = ki->max_thrashing;
        killinfo_log(procp, min_oom_score, rss_kb, swap_kb, ki->kill_reason, mi, wi, tm, pd);
        ULMK_LOG(I,"Kill '%s' (%d), uid %d, oom_score_adj %d to free %" PRId64 "kB rss, %" PRId64
              "kB swap; reason: %s", taskname, pid, uid, procp->oomadj, rss_kb, swap_kb,
              ki->kill_desc);
    } else {
        kill_st.kill_reason = NONE;
        kill_st.thrashing = 0;
        kill_st.max_thrashing = 0;
        killinfo_log(procp, min_oom_score, rss_kb, swap_kb, NONE, mi, wi, tm, pd);
        ULMK_LOG(I,"Kill '%s' (%d), uid %d, oom_score_adj %d to free %" PRId64 "kB rss, %" PRId64
              "kb swap", taskname, pid, uid, procp->oomadj, rss_kb, swap_kb);
    }

    kill_st.uid = static_cast<int32_t>(uid);
    kill_st.taskname = taskname;
    kill_st.oom_score = procp->oomadj;
    kill_st.min_oom_score = min_oom_score;
    kill_st.free_mem_kb = mi->field.nr_free_pages * page_k;
    kill_st.free_swap_kb = mi->field.free_swap * page_k;
    stats_write_lmk_kill_occurred(&kill_st, mem_st);

    ctrl_data_write_lmk_kill_occurred((pid_t)pid, uid);

    result = rss_kb / page_k;

out:
    /*
     * WARNING: After pid_remove() procp is freed and can't be used!
     * Therefore placed at the end of the function.
     */
    pid_remove(pid);
    return result;
}

/*
 * Find one process to kill at or above the given oom_score_adj level.
 * Returns size of the killed process.
 */
static int find_and_kill_process(int min_score_adj, struct kill_info *ki, union meminfo *mi,
                                 struct wakeup_info *wi, struct timespec *tm,
                                 struct psi_data *pd, enum vmpressure_level level) {
    int i;
    int killed_size = 0;
    bool lmk_state_change_start = false;
    bool choose_heaviest_task = kill_heaviest_task;

    for (i = OOM_SCORE_ADJ_MAX; i >= min_score_adj; i--) {
        struct proc *procp;

        if (!choose_heaviest_task && i <= PERCEPTIBLE_APP_ADJ) {
            /*
             * If we have to choose a perceptible process, choose the heaviest one to
             * hopefully minimize the number of victims.
             */
            choose_heaviest_task = true;
        }

        while (true) {
            procp = choose_heaviest_task ?
                proc_get_heaviest(i) : proc_adj_lru(i);

            if (!procp)
                break;

            killed_size = kill_one_process(procp, min_score_adj, ki, mi, wi, tm, pd);
            if (killed_size >= 0) {
                hvalve_kill_pending = TRIGGER;
                if (!lmk_state_change_start) {
                    lmk_state_change_start = true;
                    stats_write_lmk_state_changed(STATE_START);
                }
                break;
            } else if (killed_size == -987654321) {
                return 0;
            }
        }
        if (killed_size) {
            hvalve_kill_cnt[level]++;
            break;
        }
    }

    if (!killed_size && !min_score_adj && is_userdebug_or_eng_build) {
        killed_size = proc_get_script(level);
    }

    if (lmk_state_change_start) {
        stats_write_lmk_state_changed(STATE_STOP);
    }

    return killed_size;
}

static int64_t get_memory_usage(struct reread_data *file_data) {
    int64_t mem_usage;
    char *buf;

    if ((file_data->fd == -1) && access(file_data->filename, F_OK)) {
        return -1;
    }

    if ((buf = reread_file(file_data)) == NULL) {
        return -1;
    }

    if (!parse_int64(buf, &mem_usage)) {
        ALOGE("%s parse error", file_data->filename);
        return -1;
    }
    if (mem_usage == 0) {
        ALOGE("No memory!");
        return -1;
    }
    return mem_usage;
}

void record_low_pressure_levels(union meminfo *mi) {
    if (low_pressure_mem.min_nr_free_pages == -1 ||
        low_pressure_mem.min_nr_free_pages > mi->field.nr_free_pages) {
        if (debug_process_killing) {
            ALOGI("Low pressure min memory update from %" PRId64 " to %" PRId64,
                low_pressure_mem.min_nr_free_pages, mi->field.nr_free_pages);
        }
        low_pressure_mem.min_nr_free_pages = mi->field.nr_free_pages;
    }
    /*
     * Free memory at low vmpressure events occasionally gets spikes,
     * possibly a stale low vmpressure event with memory already
     * freed up (no memory pressure should have been reported).
     * Ignore large jumps in max_nr_free_pages that would mess up our stats.
     */
    if (low_pressure_mem.max_nr_free_pages == -1 ||
        (low_pressure_mem.max_nr_free_pages < mi->field.nr_free_pages &&
         mi->field.nr_free_pages - low_pressure_mem.max_nr_free_pages <
         low_pressure_mem.max_nr_free_pages * 0.1)) {
        if (debug_process_killing) {
            ALOGI("Low pressure max memory update from %" PRId64 " to %" PRId64,
                low_pressure_mem.max_nr_free_pages, mi->field.nr_free_pages);
        }
        low_pressure_mem.max_nr_free_pages = mi->field.nr_free_pages;
    }
}

enum vmpressure_level upgrade_level(enum vmpressure_level level) {
    return (enum vmpressure_level)((level < VMPRESS_LEVEL_CRITICAL) ?
        level + 1 : level);
}

enum vmpressure_level downgrade_level(enum vmpressure_level level) {
    return (enum vmpressure_level)((level > VMPRESS_LEVEL_LOW) ?
        level - 1 : level);
}

enum zone_watermark {
    WMARK_MIN = 0,
    WMARK_LOW,
    WMARK_HIGH,
    WMARK_NONE
};

struct zone_watermarks {
    long high_wmark;
    long low_wmark;
    long min_wmark;
};

struct zone_meminfo {
    int64_t nr_free_pages;
    int64_t cma_free;
    struct zone_watermarks watermarks;

};

static bool should_consider_cache_free(uint32_t events, enum vmpressure_level level)
{
    if (cache_percent) {
        return events? level != VMPRESS_LEVEL_SUPER_CRITICAL && level != VMPRESS_LEVEL_CRITICAL : true;
    }
    return false;
}

/*
 * Returns lowest breached watermark or WMARK_NONE.
 */
static enum zone_watermark get_lowest_watermark(union meminfo *mi,
                                                struct zone_meminfo *zmi, enum vmpressure_level level, uint32_t events)
{
    struct zone_watermarks *watermarks = &zmi->watermarks;
    int64_t nr_free_pages = zmi->nr_free_pages - zmi->cma_free;
    int64_t nr_cached_pages = 0;

    if (should_consider_cache_free(events, level)) {
        nr_cached_pages = (int64_t)(cache_percent * mi->field.cached);
    }
    if (nr_free_pages + nr_cached_pages < watermarks->min_wmark) {
        return WMARK_MIN;
    }
    if (nr_free_pages + nr_cached_pages * wbf_effective < wbf_effective * watermarks->low_wmark) {
        return WMARK_LOW;
    }
    if (nr_free_pages + nr_cached_pages * wbf_effective < wbf_effective * watermarks->high_wmark) {
        return WMARK_HIGH;
    }
    return WMARK_NONE;
}

static void log_zone_watermarks(struct zoneinfo *zi,
                                struct zone_watermarks *wmarks) {
    int i, j;
    struct zoneinfo_node *node;
    union zoneinfo_zone_fields *zone_fields;

    for (i = 0; i < zi->node_count; i++) {
        node = &zi->nodes[i];

        for (j = 0; j < node->zone_count; j++) {
            zone_fields = &node->zones[j].fields;

            if (debug_process_killing) {
                ULMK_LOG(D, "Zone: %d nr_free_pages: %" PRId64 " min: %" PRId64
                     " low: %" PRId64 " high: %" PRId64 " present: %" PRId64
                     " nr_cma_free: %" PRId64 " max_protection: %" PRId64,
                     j, zone_fields->field.nr_free_pages,
                     zone_fields->field.min, zone_fields->field.low,
                     zone_fields->field.high, zone_fields->field.present,
                     zone_fields->field.nr_free_cma,
                     node->zones[j].max_protection);
            }
        }
    }

    if (debug_process_killing) {
        ULMK_LOG(D, "Aggregate wmarks: min: %ld low: %ld high: %ld",
             wmarks->min_wmark, wmarks->low_wmark, wmarks->high_wmark);
    }
}

void calc_zone_watermarks(struct zoneinfo *zi, struct zone_meminfo *zmi, int64_t *pgskip_deltas) {
    struct zone_watermarks *watermarks;

    memset(zmi, 0, sizeof(struct zone_meminfo));
    watermarks = &zmi->watermarks;

    for (int node_idx = 0; node_idx < zi->node_count; node_idx++) {
        struct zoneinfo_node *node = &zi->nodes[node_idx];
        int i = VS_PGSKIP_FIRST_ZONE;
        for (int zone_idx = 0; zone_idx < node->zone_count; zone_idx++) {
            struct zoneinfo_zone *zone = &node->zones[zone_idx];

            while (pgskip_deltas[PGSKIP_IDX(i)]  < 0) ++i;

            if (!zone->fields.field.present) {
                i++;
                continue;
            }

            if (!pgskip_deltas[PGSKIP_IDX(i++)]) {
                zmi->nr_free_pages += zone->fields.field.nr_free_pages;
                zmi->cma_free += zone->fields.field.nr_free_cma;
                watermarks->high_wmark += zone->max_protection + zone->fields.field.high;
                watermarks->low_wmark += zone->max_protection + zone->fields.field.low;
                watermarks->min_wmark += zone->max_protection + zone->fields.field.min;
            }
        }
    }

    log_zone_watermarks(zi, watermarks);
}

static void log_meminfo(union meminfo *mi, enum zone_watermark wmark)
{
    char wmark_str[LINE_MAX];

    if (wmark == WMARK_MIN) {
        strlcpy(wmark_str, "min", LINE_MAX);
    } else if (wmark == WMARK_LOW) {
        strlcpy(wmark_str, "low", LINE_MAX);
    } else if (wmark == WMARK_HIGH) {
        strlcpy(wmark_str, "high", LINE_MAX);
    } else {
        strlcpy(wmark_str, "none", LINE_MAX);
    }

    if (debug_process_killing) {
        ULMK_LOG(D, "smallest wmark breached: %s nr_free_pages: %" PRId64
             " active_anon: %" PRId64 " inactive_anon: %" PRId64
             " cma_free: %" PRId64, wmark_str, mi->field.nr_free_pages,
             mi->field.active_anon, mi->field.inactive_anon,
             mi->field.cma_free);
    }
}

static void fill_log_pgskip_stats(union vmstat *vs, int64_t *init_pgskip, int64_t *pgskip_deltas)
{
    unsigned int i;

    for (i = VS_PGSKIP_FIRST_ZONE; i <= VS_PGSKIP_LAST_ZONE; i++) {
        if (vs->arr[i] >= 0) {
            pgskip_deltas[PGSKIP_IDX(i)] = vs->arr[i] -
                                           init_pgskip[PGSKIP_IDX(i)];
        } else {
            pgskip_deltas[PGSKIP_IDX(i)] = -1;
        }
    }

    if (debug_process_killing) {
        ULMK_LOG(D, "pgskip deltas: DMA: %" PRId64 "DMA32: %" PRId64 " Normal: %" PRId64 " High: %"
             PRId64 " Movable: %" PRId64,
             pgskip_deltas[PGSKIP_IDX(VS_PGSKIP_DMA)],
             pgskip_deltas[PGSKIP_IDX(VS_PGSKIP_DMA32)],
             pgskip_deltas[PGSKIP_IDX(VS_PGSKIP_NORMAL)],
             pgskip_deltas[PGSKIP_IDX(VS_PGSKIP_HIGH)],
             pgskip_deltas[PGSKIP_IDX(VS_PGSKIP_MOVABLE)]);
    }
}

static int calc_swap_utilization(union meminfo *mi) {
    int64_t swap_used = mi->field.total_swap - mi->field.free_swap;
    int64_t total_swappable = mi->field.active_anon + mi->field.inactive_anon +
                              mi->field.shmem + swap_used;
    return total_swappable > 0 ? (swap_used * 100) / total_swappable : 0;
}

static void mp_event_psi(int data, uint32_t events, struct polling_params *poll_params) {
    enum reclaim_state {
        NO_RECLAIM = 0,
        KSWAPD_RECLAIM,
        DIRECT_RECLAIM,
        DIRECT_RECLAIM_THROTTLE,
    };
    static int64_t init_ws_refault;
    static int64_t prev_workingset_refault;
    static int64_t base_file_lru;
    static int64_t init_pgscan_kswapd;
    static int64_t init_pgscan_direct;
    static int64_t init_direct_throttle;
    static int64_t init_pgskip[VS_PGSKIP_LAST_ZONE - VS_PGSKIP_FIRST_ZONE + 1];
    static int64_t swap_low_threshold;
    static bool killing;
    static int thrashing_limit = thrashing_limit_pct;
    static struct wakeup_info wi;
    static struct zone_meminfo zone_mem_info;
    static struct timespec last_pa_update_tm;
    static int64_t init_compact_stall;
    static struct timespec thrashing_reset_tm;
    static int64_t prev_thrash_growth = 0;
    static bool check_filecache = false;
    static int max_thrashing = 0;

    union meminfo mi;
    union vmstat vs;
    struct psi_data psi_data;
    struct timespec curr_tm;
    int64_t thrashing = 0;
    bool swap_is_low = false;
    enum vmpressure_level level = (enum vmpressure_level)data;
    enum kill_reasons kill_reason = NONE;
    bool cycle_after_kill = false;
    enum reclaim_state reclaim = NO_RECLAIM;
    enum zone_watermark wmark = WMARK_NONE;
    char kill_desc[LINE_MAX];
    bool cut_thrashing_limit = false;
    unsigned int i;
    int min_score_adj = 0;
    bool in_compaction = false;
    int swap_util = 0;
    long since_thrashing_reset_ms;
    int64_t workingset_refault_file;
    int64_t pgskip_deltas[VS_PGSKIP_LAST_ZONE - VS_PGSKIP_FIRST_ZONE + 1] = {0};
    struct zoneinfo zi;
    bool critical_stall = false;

    struct timespec start_tm;
    struct timespec end_tm;

    clock_gettime(CLOCK_MONOTONIC, &start_tm);

    ALOGI("CIH: mp_event_psi: %s pressure event %s, low: %d mid: %d critical: %d super critical: %d",
        level_name[level], events ? "triggered" : "polling check", hvalve_kill_cnt[0], hvalve_kill_cnt[1], hvalve_kill_cnt[2], hvalve_kill_cnt[3]);

    ULMK_LOG(D, "%s pressure event %s", level_name[level], events ?
             "triggered" : "polling check");

    if (events > 0) {
        hvalve_kill_pending = TRIGGER;
        syscall(__NR_hvalve, 4, level, 0, 0);
    }

    if (events &&
       (!poll_params->poll_handler || data >= poll_params->poll_handler->data)) {
           wbf_effective = wmark_boost_factor;
    }

    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm) != 0) {
        ALOGE("Failed to get current time");
        return;
    }

    record_wakeup_time(&curr_tm, events ? Event : Polling, &wi);

    if (level == VMPRESS_LEVEL_MEDIUM) {
        if (enable_preferred_apps &&
                (get_time_diff_ms(&last_pa_update_tm, &curr_tm) >= pa_update_timeout_ms)) {
            perf_ux_engine_trigger(PAPP_OPCODE, preferred_apps);
            last_pa_update_tm = curr_tm;
        }
    }

    bool kill_pending = is_kill_pending();
    if (kill_pending && (kill_timeout_ms == 0 ||
        get_time_diff_ms(&last_kill_tm, &curr_tm) < static_cast<long>(kill_timeout_ms))) {
        /* Skip while still killing a process */
        wi.skipped_wakeups++;
        ULMK_LOG(D, "Ignoring %s pressure event; kill already in progress",
                 level_name[level]);
        goto no_kill;
    }
    /*
     * Process is dead or kill timeout is over, stop waiting. This has no effect if pidfds are
     * supported and death notification already caused waiting to stop.
     */
    stop_wait_for_proc_kill(!kill_pending);

    if (vmstat_parse(&vs) < 0) {
        ALOGE("Failed to parse vmstat!");
        return;
    }
    /* Starting 5.9 kernel workingset_refault vmstat field was renamed workingset_refault_file */
    workingset_refault_file = vs.field.workingset_refault ? : vs.field.workingset_refault_file;

    if (meminfo_parse(&mi) < 0) {
        ALOGE("Failed to parse meminfo!");
        return;
    }

    /* Reset states after process got killed */
    if (killing) {
        killing = false;
        cycle_after_kill = true;
        /* Reset file-backed pagecache size and refault amounts after a kill */
        base_file_lru = vs.field.nr_inactive_file + vs.field.nr_active_file;
        init_ws_refault = workingset_refault_file;
        thrashing_reset_tm = curr_tm;
        prev_thrash_growth = 0;
    }

    if (debug_process_killing) {
        ULMK_LOG(D, "nr_free_pages: %" PRId64 " nr_inactive_file: %" PRId64
             " nr_active_file: %" PRId64  " workingset_refault: %" PRId64
             " pgscan_kswapd: %" PRId64 " pgscan_direct: %" PRId64
             " pgscan_direct_throttle: %" PRId64 " init_pgscan_direct: %" PRId64
             " init_pgscan_kswapd: %" PRId64 " base_file_lru: %" PRId64
             " init_ws_refault: %" PRId64 " free_swap: %" PRId64
             " total_swap: %" PRId64 " swap_free_percentage: %" PRId64 "%%",
             vs.field.nr_free_pages, vs.field.nr_inactive_file,
             vs.field.nr_active_file, vs.field.workingset_refault,
             vs.field.pgscan_kswapd, vs.field.pgscan_direct,
             vs.field.pgscan_direct_throttle, init_pgscan_direct,
             init_pgscan_kswapd, base_file_lru, init_ws_refault,
             mi.field.free_swap, mi.field.total_swap,
             (mi.field.free_swap * 100) / (mi.field.total_swap + 1));
    }
    fill_log_pgskip_stats(&vs, init_pgskip, pgskip_deltas);

    /* Check free swap levels */
    if (swap_free_low_percentage) {
        if (!swap_low_threshold) {
            swap_low_threshold = mi.field.total_swap * swap_free_low_percentage / 100;
        }
        swap_is_low = mi.field.free_swap < swap_low_threshold;
    }

    if (vs.field.compact_stall > init_compact_stall) {
        init_compact_stall = vs.field.compact_stall;
        in_compaction = true;
    }

    /* Identify reclaim state */
    if (vs.field.pgscan_direct > init_pgscan_direct) {
        init_pgscan_direct = vs.field.pgscan_direct;
        init_pgscan_kswapd = vs.field.pgscan_kswapd;
        for (i = VS_PGSKIP_FIRST_ZONE; i <= VS_PGSKIP_LAST_ZONE; i++) {
            init_pgskip[PGSKIP_IDX(i)] = vs.arr[i];
        }
        reclaim = DIRECT_RECLAIM;
    }  else if (vs.field.pgscan_direct_throttle > init_direct_throttle) {
        init_direct_throttle = vs.field.pgscan_direct_throttle;
        reclaim = DIRECT_RECLAIM_THROTTLE;
    } else if (vs.field.pgscan_kswapd > init_pgscan_kswapd) {
        init_pgscan_kswapd = vs.field.pgscan_kswapd;
        for (i = VS_PGSKIP_FIRST_ZONE; i <= VS_PGSKIP_LAST_ZONE; i++) {
            init_pgskip[PGSKIP_IDX(i)] = vs.arr[i];
        }
        reclaim = KSWAPD_RECLAIM;
    } else if (workingset_refault_file == prev_workingset_refault) {
        if (enable_preferred_apps &&
                  (get_time_diff_ms(&last_pa_update_tm, &curr_tm) >= pa_update_timeout_ms)) {
              perf_ux_engine_trigger(PAPP_OPCODE, preferred_apps);
              last_pa_update_tm = curr_tm;
        }

        if (!in_compaction) {
            /* Skip if system is not reclaiming */
            ULMK_LOG(D, "Ignoring %s pressure event; system is not in reclaim",
                     level_name[level]);
            goto no_kill;
        }
    }

    prev_workingset_refault = workingset_refault_file;

     /*
     * It's possible we fail to find an eligible process to kill (ex. no process is
     * above oom_adj_min). When this happens, we should retry to find a new process
     * for a kill whenever a new eligible process is available. This is especially
     * important for a slow growing refault case. While retrying, we should keep
     * monitoring new thrashing counter as someone could release the memory to mitigate
     * the thrashing. Thus, when thrashing reset window comes, we decay the prev thrashing
     * counter by window counts. If the counter is still greater than thrashing limit,
     * we preserve the current prev_thrash counter so we will retry kill again. Otherwise,
     * we reset the prev_thrash counter so we will stop retrying.
     */
    since_thrashing_reset_ms = get_time_diff_ms(&thrashing_reset_tm, &curr_tm);
    if (since_thrashing_reset_ms > THRASHING_RESET_INTERVAL_MS) {
        long windows_passed;
        /* Calculate prev_thrash_growth if we crossed THRASHING_RESET_INTERVAL_MS */
        prev_thrash_growth = (workingset_refault_file - init_ws_refault) * 100
                            / (base_file_lru + 1);
        windows_passed = (since_thrashing_reset_ms / THRASHING_RESET_INTERVAL_MS);
        /*
         * Decay prev_thrashing unless over-the-limit thrashing was registered in the window we
         * just crossed, which means there were no eligible processes to kill. We preserve the
         * counter in that case to ensure a kill if a new eligible process appears.
         */
        if (windows_passed > 1 || prev_thrash_growth < thrashing_limit) {
            prev_thrash_growth >>= windows_passed;
        }

        /* Record file-backed pagecache size when crossing THRASHING_RESET_INTERVAL_MS */
        base_file_lru = vs.field.nr_inactive_file + vs.field.nr_active_file;
        init_ws_refault = workingset_refault_file;
        thrashing_reset_tm = curr_tm;
        thrashing_limit = thrashing_limit_pct;
    } else {
        /* Calculate what % of the file-backed pagecache refaulted so far */
        thrashing = (workingset_refault_file - init_ws_refault) * 100 / (base_file_lru + 1);
        ULMK_LOG(D, "thrashing: %" PRId64 "%% thrashing_limit: %d%%", thrashing,
                 thrashing_limit);
    }
    /* Add previous cycle's decayed thrashing amount */
    thrashing += prev_thrash_growth;
    if (max_thrashing < thrashing) {
        max_thrashing = thrashing;
    }

    if (zoneinfo_parse(&zi) < 0) {
        ALOGE("Failed to parse zoneinfo!");
        return;
    }

    calc_zone_watermarks(&zi, &zone_mem_info, pgskip_deltas);

    /* Find out which watermark is breached if any */
    wmark = get_lowest_watermark(&mi, &zone_mem_info, level, events);
    log_meminfo(&mi, wmark);
    if (level < VMPRESS_LEVEL_CRITICAL && (reclaim == DIRECT_RECLAIM ||
            reclaim == DIRECT_RECLAIM_THROTTLE)) {
        last_event_upgraded = true;
    }

    if (!psi_parse_mem(&psi_data)) {
        critical_stall = psi_data.mem_stats[PSI_FULL].avg10 > (float)stall_limit_critical;
    }
    /*
     * TODO: move this logic into a separate function
     * Decide if killing a process is necessary and record the reason
     */
    if (cycle_after_kill && wmark <= WMARK_LOW) {
        /*
         * Prevent kills not freeing enough memory which might lead to OOM kill.
         * This might happen when a process is consuming memory faster than reclaim can
         * free even after a kill. Mostly happens when running memory stress tests.
         */
        kill_reason = PRESSURE_AFTER_KILL;
        strlcpy(kill_desc, "min watermark is breached even after kill", sizeof(kill_desc));
        min_score_adj = PERCEPTIBLE_RECENT_FOREGROUND_APP_ADJ;
        if (wmark > WMARK_MIN) {
            min_score_adj = VISIBLE_APP_ADJ;
        }
    } else if (reclaim == DIRECT_RECLAIM_THROTTLE) {
        kill_reason = DIRECT_RECL_AND_THROT;
        strlcpy(kill_desc, "system processes are being throttled", sizeof(kill_desc));
    } else if (level == VMPRESS_LEVEL_CRITICAL && wmark <= WMARK_HIGH) {
        /*
         * Device is too busy reclaiming memory which might lead to ANR.
         * Critical level is triggered when PSI complete stall (all tasks are blocked because
         * of the memory congestion) breaches the configured threshold.
         */
        kill_reason = CRITICAL_KILL;
        strlcpy(kill_desc, "critical pressure and device is low on memory", sizeof(kill_desc));
        min_score_adj = PERCEPTIBLE_RECENT_FOREGROUND_APP_ADJ;
    } else if (level == VMPRESS_LEVEL_SUPER_CRITICAL && wmark <= WMARK_HIGH) {
        /*
         * Device is too busy reclaiming memory which might lead to ANR.
         * Critical level is triggered when PSI complete stall (all tasks are blocked because
         * of the memory congestion) breaches the configured threshold.
         */
        /*
         * The preexisting NOT_RESPONDING event was only allowed 1 kill per psi window. Instead
         * allow very agressive killing of all apps without considering thrashing, under the
         * assumption that this event will not be triggered unless file cache is very small.
         * Intended to kill memory stress tests which allocate memory with the intention of
         * reaching OOM.
         */
        kill_reason = NOT_RESPONDING;
        strlcpy(kill_desc, "device is not responding", sizeof(kill_desc));
    } else if (swap_is_low && thrashing > thrashing_limit_pct) {
        /* Page cache is thrashing while swap is low */
        kill_reason = LOW_SWAP_AND_THRASHING;
        snprintf(kill_desc, sizeof(kill_desc), "device is low on swap (%" PRId64
            "kB < %" PRId64 "kB) and thrashing (%" PRId64 "%%)",
            mi.field.free_swap * page_k, swap_low_threshold * page_k, thrashing);
        /* Do not kill perceptible apps unless below min watermark or heavily thrashing */
        if (wmark > WMARK_MIN && thrashing < thrashing_critical_pct) {
            min_score_adj = PERCEPTIBLE_APP_ADJ + 1;
        }
        check_filecache = true;
    } else if (swap_is_low && wmark <= WMARK_HIGH) {
        /* Both free memory and swap are low */
        kill_reason = LOW_MEM_AND_SWAP;
        snprintf(kill_desc, sizeof(kill_desc), "%s watermark is breached and swap is low (%"
            PRId64 "kB < %" PRId64 "kB)", wmark < WMARK_LOW ? "min" : "low",
            mi.field.free_swap * page_k, swap_low_threshold * page_k);
        /* Do not kill perceptible apps unless below min watermark or heavily thrashing */
        if (wmark > WMARK_MIN && thrashing < thrashing_critical_pct) {
            min_score_adj = PERCEPTIBLE_APP_ADJ + 1;
        }
    } else if (wmark < WMARK_HIGH && swap_util_max < 100 &&
               (swap_util = calc_swap_utilization(&mi)) > swap_util_max) {
        /*
         * Too much anon memory is swapped out but swap is not low.
         * Non-swappable allocations created memory pressure.
         */
        kill_reason = LOW_MEM_AND_SWAP_UTIL;
        snprintf(kill_desc, sizeof(kill_desc), "%s watermark is breached and swap utilization"
            " is high (%d%% > %d%%)", wmark < WMARK_LOW ? "min" : "low",
            swap_util, swap_util_max);
    } else if (wmark <= WMARK_HIGH && thrashing > thrashing_limit) {
        /* Page cache is thrashing while memory is low */
        kill_reason = LOW_MEM_AND_THRASHING;
        snprintf(kill_desc, sizeof(kill_desc), "%s watermark is breached and thrashing (%"
            PRId64 "%%)", wmark < WMARK_LOW ? "min" : "low", thrashing);
        cut_thrashing_limit = true;
        min_score_adj = VISIBLE_APP_ADJ;

        check_filecache = true;
    } else if (reclaim == DIRECT_RECLAIM && thrashing > thrashing_limit) {
        /* Page cache is thrashing while in direct reclaim (mostly happens on lowram devices) */
        kill_reason = DIRECT_RECL_AND_THRASHING;
        snprintf(kill_desc, sizeof(kill_desc), "device is in direct reclaim and thrashing (%"
            PRId64 "%%)", thrashing);
        cut_thrashing_limit = true;
        /* Do not kill perceptible apps unless thrashing at critical levels */
        min_score_adj = PERCEPTIBLE_APP_ADJ + 1;

        check_filecache = true;
    } else if (check_filecache) {
        int64_t file_lru_kb = (vs.field.nr_inactive_file + vs.field.nr_active_file) * page_k;

        if (file_lru_kb < filecache_min_kb) {
            /* File cache is too low after thrashing, keep killing background processes */
            kill_reason = LOW_FILECACHE_AFTER_THRASHING;
            snprintf(kill_desc, sizeof(kill_desc),
                "filecache is low (%" PRId64 "kB < %" PRId64 "kB) after thrashing",
                file_lru_kb, filecache_min_kb);
            min_score_adj = PERCEPTIBLE_APP_ADJ + 1;
        } else {
            /* File cache is big enough, stop checking */
            check_filecache = false;
        }
    } else if (reclaim == DIRECT_RECLAIM && wmark <= WMARK_HIGH) {
        kill_reason = DIRECT_RECL_AND_LOW_MEM;
        strlcpy(kill_desc, "device is in direct reclaim and low on memory", sizeof(kill_desc));
        min_score_adj = PERCEPTIBLE_APP_ADJ;
    } else if (in_compaction && wmark <= WMARK_HIGH) {
        kill_reason = COMPACTION;
        strlcpy(kill_desc, "device is in compaction and low on memory", sizeof(kill_desc));
        min_score_adj = VISIBLE_APP_ADJ;
    }

    /* Kill a process if necessary */
    if (kill_reason != NONE) {
        struct kill_info ki = {
            .kill_reason = kill_reason,
            .kill_desc = kill_desc,
            .thrashing = (int)thrashing,
            .max_thrashing = max_thrashing,
        };

        /* Allow killing perceptible apps if the system is stalled */
        if (critical_stall) {
            min_score_adj = 0;
        }
        psi_parse_io(&psi_data);
        psi_parse_cpu(&psi_data);
        int pages_freed = find_and_kill_process(min_score_adj, &ki, &mi, &wi, &curr_tm, &psi_data, level);
        if (pages_freed > 0) {
            killing = true;
            max_thrashing = 0;
            /* Killed..Just reduce/increase the boost... */
            if (kill_reason == CRITICAL_KILL || kill_reason == DIRECT_RECL_AND_THROT) {
                wbf_effective =  min(wbf_effective + wbf_step, wmark_boost_factor);
            } else {
                wbf_effective = max(wbf_effective - wbf_step, 1);
            }
            if (cut_thrashing_limit) {
                /*
                 * Cut thrasing limit by thrashing_limit_decay_pct percentage of the current
                 * thrashing limit until the system stops thrashing.
                 */
                thrashing_limit = (thrashing_limit * (100 - thrashing_limit_decay_pct)) / 100;
            }
        } else {
            ULMK_LOG(D, "No processes to kill with adj score >= %d",
                     min_score_adj);
        }
    } else {
        ULMK_LOG(D, "Not killing for %s pressure event %s", level_name[level],
                 events ? "trigger" : "polling check");
    }

    clock_gettime(CLOCK_MONOTONIC, &end_tm);

    ALOGI("LMKD_ELAPSED_TIME: %lld", (1000000000LL * (end_tm.tv_sec - start_tm.tv_sec) + (end_tm.tv_nsec - start_tm.tv_nsec))/1000);

no_kill:
    /* Do not poll if kernel supports pidfd waiting */
    if (is_waiting_for_kill()) {
        /* Pause polling if we are waiting for process death notification */
        poll_params->update = POLLING_PAUSE;
        return;
    }

    /*
     * Start polling after initial PSI event;
     * extend polling while device is in direct reclaim or process is being killed;
     * do not extend when kswapd reclaims because that might go on for a long time
     * without causing memory pressure
     */

    if (events || killing || reclaim == DIRECT_RECLAIM || reclaim == DIRECT_RECLAIM_THROTTLE) {
        if (count_upgraded_event >= psi_cont_event_thresh) {
            poll_params->update = POLLING_CRIT_UPGRADE;
            count_upgraded_event = 0;
        } else if (!poll_params->poll_handler || data >= poll_params->poll_handler->data) {
            poll_params->update = POLLING_START;
            if (!killing) {
                wbf_effective = max(wbf_effective - wbf_step, 1);
            }
        }
    }

    /* Decide the polling interval */
    if (swap_is_low || killing) {
        /* Fast polling during and after a kill or when swap is low */
        poll_params->polling_interval_ms = PSI_POLL_PERIOD_SHORT_MS;
    } else if (level == VMPRESS_LEVEL_SUPER_CRITICAL) {
        poll_params->polling_interval_ms = psi_poll_period_scrit_ms;
    } else {
        /* By default use long intervals */
        poll_params->polling_interval_ms = PSI_POLL_PERIOD_LONG_MS;
    }
}

enum vmpressure_level upgrade_vmpressure_event(enum vmpressure_level level)
{
    static union vmstat base;
    union vmstat current;
    int64_t throttle, pressure;
    static int64_t sync, async;

    switch (level) {
        case VMPRESS_LEVEL_LOW:
            if (vmstat_parse(&base) < 0) {
                ULMK_LOG(E, "Failed to parse vmstat!");
                goto out;
            }
            break;
        case VMPRESS_LEVEL_MEDIUM:
        case VMPRESS_LEVEL_CRITICAL:
            if (vmstat_parse(&current) < 0) {
                ULMK_LOG(E, "Failed to parse vmstat!");
                goto out;
            }
            throttle = current.field.pgscan_direct_throttle -
                    base.field.pgscan_direct_throttle;
            sync += (current.field.pgscan_direct - base.field.pgscan_direct);
            async += (current.field.pgscan_kswapd - base.field.pgscan_kswapd);
            /* Here scan window size is put at default 4MB(=1024 pages). */
            if (throttle || (sync + async) >= reclaim_scan_threshold) {
                pressure = ((100 * sync)/(sync + async + 1));
                if (throttle || (pressure >= direct_reclaim_pressure)) {
                    last_event_upgraded = true;
                    if (count_upgraded_event >= 4) {
                        count_upgraded_event = 0;
                        s_crit_event = true;
                        if (debug_process_killing) {
                            ULMK_LOG(D, "Medium/Critical is permanently upgraded to Supercritical event\n");
                        }
                    } else {
                        s_crit_event = s_crit_event_upgraded = true;
                        if (debug_process_killing) {
                            ULMK_LOG(D, "Medium/Critical is upgraded to Supercritical event\n");
                        }
                    }
                    s_crit_base = current;
                }
                sync = async = 0;
            }
            base = current;
            break;
        default:
            ;
    }
out:
    return level;
}

static void mp_event_common(int data, uint32_t events, struct polling_params *poll_params) {
    unsigned long long evcount;
    int64_t mem_usage, memsw_usage;
    int64_t mem_pressure;
    union meminfo mi;
    struct zoneinfo zi;
    union vmstat s_crit_current;
    struct timespec curr_tm;
    static struct timespec last_pa_update_tm;
    static unsigned long kill_skip_count = 0;
    enum vmpressure_level level = (enum vmpressure_level)data;
    long other_free = 0, other_file = 0;
    int min_score_adj;
    int minfree = 0;
    static struct reread_data mem_usage_file_data = {
        .filename = MEMCG_MEMORY_USAGE,
        .fd = -1,
    };
    static struct reread_data memsw_usage_file_data = {
        .filename = MEMCG_MEMORYSW_USAGE,
        .fd = -1,
    };
    static struct wakeup_info wi;

    if (!s_crit_event) {
        level = upgrade_vmpressure_event(level);
    }

    if (debug_process_killing) {
        ALOGI("%s memory pressure event is triggered", level_name[level]);
    }

    ALOGI("CIH: mp_event_common enter");

    if (!use_psi_monitors) {
        /*
         * Check all event counters from low to critical
         * and upgrade to the highest priority one. By reading
         * eventfd we also reset the event counters.
         */
        for (int lvl = VMPRESS_LEVEL_LOW; lvl < VMPRESS_LEVEL_COUNT; lvl++) {
            if (mpevfd[lvl] != -1 &&
                TEMP_FAILURE_RETRY(read(mpevfd[lvl],
                                   &evcount, sizeof(evcount))) > 0 &&
                evcount > 0 && lvl > level) {
                level = static_cast<vmpressure_level>(lvl);
            }
        }
    }

    /* Start polling after initial PSI event */
    if (use_psi_monitors && events) {
        /* Override polling params only if current event is more critical */
        if (!poll_params->poll_handler || data > poll_params->poll_handler->data) {
            poll_params->polling_interval_ms = PSI_POLL_PERIOD_SHORT_MS;
            poll_params->update = POLLING_START;
        }
        /*
         * Nonzero events indicates handler call due to recieved epoll_event,
         * rather than due to epoll_event timeout.
         */
        if (events) {
            if (data == VMPRESS_LEVEL_SUPER_CRITICAL) {
                s_crit_event = true;
                poll_params->polling_interval_ms = psi_poll_period_scrit_ms;
                vmstat_parse(&s_crit_base);
            }
            else if (s_crit_event) {
                /* Override the supercritical event only if the system
                 * is not in direct reclaim.
                 */
                int64_t throttle, sync;

                vmstat_parse(&s_crit_current);
                throttle = s_crit_current.field.pgscan_direct_throttle -
                            s_crit_base.field.pgscan_direct_throttle;
                sync = s_crit_current.field.pgscan_direct -
                        s_crit_base.field.pgscan_direct;
                if (!throttle && !sync) {
                    s_crit_event = false;
                }
                s_crit_base = s_crit_current;
            }
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm) != 0) {
        ALOGE("Failed to get current time");
        return;
    }

    record_wakeup_time(&curr_tm, events ? Event : Polling, &wi);

    if (kill_timeout_ms &&
        get_time_diff_ms(&last_kill_tm, &curr_tm) < static_cast<long>(kill_timeout_ms)) {
        /*
         * If we're within the no-kill timeout, see if there's pending reclaim work
         * from the last killed process. If so, skip killing for now.
         */
        if (is_kill_pending()) {
            kill_skip_count++;
            wi.skipped_wakeups++;
            return;
        }
        /*
         * Process is dead, stop waiting. This has no effect if pidfds are supported and
         * death notification already caused waiting to stop.
         */
        stop_wait_for_proc_kill(true);
    } else {
        /*
         * Killing took longer than no-kill timeout. Stop waiting for the last process
         * to die because we are ready to kill again.
         */
        stop_wait_for_proc_kill(false);
    }

    if (kill_skip_count > 0) {
        ALOGI("%lu memory pressure events were skipped after a kill!",
              kill_skip_count);
        kill_skip_count = 0;
    }

    if (meminfo_parse(&mi) < 0 || zoneinfo_parse(&zi) < 0) {
        ALOGE("Failed to get free memory!");
        return;
    }

    if (use_minfree_levels) {
        int i;

        other_free = mi.field.nr_free_pages - zi.totalreserve_pages;
        if (mi.field.nr_file_pages > (mi.field.shmem + mi.field.unevictable + mi.field.swap_cached)) {
            other_file = (mi.field.nr_file_pages - mi.field.shmem -
                          mi.field.unevictable - mi.field.swap_cached);
        } else {
            other_file = 0;
        }

        min_score_adj = OOM_SCORE_ADJ_MAX + 1;
        for (i = 0; i < lowmem_targets_size; i++) {
            minfree = lowmem_minfree[i];
            if (other_free < minfree && other_file < minfree) {
                min_score_adj = lowmem_adj[i];
                // Adaptive LMK
                if (enable_adaptive_lmk && level == VMPRESS_LEVEL_CRITICAL &&
                        i > lowmem_targets_size-4) {
                    min_score_adj = lowmem_adj[i-1];
                }
                break;
            }
        }

        if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
            if (debug_process_killing) {
                ALOGI("Ignore %s memory pressure event "
                      "(free memory=%ldkB, cache=%ldkB, limit=%ldkB)",
                      level_name[level], other_free * page_k, other_file * page_k,
                      (long)lowmem_minfree[lowmem_targets_size - 1] * page_k);
            }
            return;
        }

        goto do_kill;
    }

    if (level == VMPRESS_LEVEL_LOW) {
        record_low_pressure_levels(&mi);
        if (enable_preferred_apps) {
            if (get_time_diff_ms(&last_pa_update_tm, &curr_tm) >= pa_update_timeout_ms) {
                perf_ux_engine_trigger(PAPP_OPCODE, preferred_apps);
                last_pa_update_tm = curr_tm;
            }
        }
    }

    if (level_oomadj[level] > OOM_SCORE_ADJ_MAX) {
        /* Do not monitor this pressure level */
        return;
    }

    if ((mem_usage = get_memory_usage(&mem_usage_file_data)) < 0) {
        goto do_kill;
    }
    if ((memsw_usage = get_memory_usage(&memsw_usage_file_data)) < 0) {
        goto do_kill;
    }

    // Calculate percent for swappinness.
    mem_pressure = (mem_usage * 100) / memsw_usage;
    ALOGI("CIH: memory pressure: %ld%%, level: %s", mem_pressure, level_name[level]);

    if (enable_pressure_upgrade && level != VMPRESS_LEVEL_CRITICAL) {
        // We are swapping too much.
        if (mem_pressure < upgrade_pressure) {
            level = upgrade_level(level);
            if (debug_process_killing) {
                ALOGI("Event upgraded to %s", level_name[level]);
            }
        }
    }

    // If we still have enough swap space available, check if we want to
    // ignore/downgrade pressure events.
    if (mi.field.total_swap && (mi.field.free_swap >=
        mi.field.total_swap * swap_free_low_percentage / 100)) {
        // If the pressure is larger than downgrade_pressure lmk will not
        // kill any process, since enough memory is available.
        if (mem_pressure > downgrade_pressure) {
            if (debug_process_killing) {
                ALOGI("Ignore %s memory pressure", level_name[level]);
            }
            return;
        } else if (level == VMPRESS_LEVEL_CRITICAL && mem_pressure > upgrade_pressure) {
            if (debug_process_killing) {
                ALOGI("Downgrade critical memory pressure");
            }
            // Downgrade event, since enough memory available.
            level = downgrade_level(level);
        }
    }

do_kill:
    ALOGI("CIH: do_kill: level: %s", level_name[level]);
    if (low_ram_device && per_app_memcg) {
        /* For Go devices kill only one task */
        if (find_and_kill_process(level_oomadj[level], NULL, &mi, &wi, &curr_tm, NULL, level) == 0) {
            if (debug_process_killing) {
                ALOGI("Nothing to kill");
            }
        }
    } else {
        int pages_freed;
        static struct timespec last_report_tm;
        static unsigned long report_skip_count = 0;

        if (!use_minfree_levels) {
            if (!enable_watermark_check) {
                /* Free up enough memory to downgrate the memory pressure to low level */
                if (mi.field.nr_free_pages >= low_pressure_mem.max_nr_free_pages) {
                    if (debug_process_killing) {
                        ULMK_LOG(I, "Ignoring pressure since more memory is "
                            "available (%" PRId64 ") than watermark (%" PRId64 ")",
                            mi.field.nr_free_pages, low_pressure_mem.max_nr_free_pages);
                    }
                    return;
                }
                min_score_adj = level_oomadj[level];
            } else {
                min_score_adj = zone_watermarks_ok(level);
                if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
                    ULMK_LOG(I, "Ignoring pressure since per-zone watermarks ok");
                    return;
                }
            }
        }

        pages_freed = find_and_kill_process(min_score_adj, NULL, &mi, &wi, &curr_tm, NULL, level);

        if (pages_freed == 0) {
            /* Rate limit kill reports when nothing was reclaimed */
            if (get_time_diff_ms(&last_report_tm, &curr_tm) < FAIL_REPORT_RLIMIT_MS) {
                report_skip_count++;
                return;
            }
        }

        /* Log whenever we kill or when report rate limit allows */
        if (use_minfree_levels) {
            ALOGI("Reclaimed %ldkB, cache(%ldkB) and free(%" PRId64 "kB)-reserved(%" PRId64 "kB) "
                "below min(%ldkB) for oom_score_adj %d",
                pages_freed * page_k,
                other_file * page_k, mi.field.nr_free_pages * page_k,
                zi.totalreserve_pages * page_k,
                minfree * page_k, min_score_adj);
        } else {
            ALOGI("Reclaimed %ldkB at oom_score_adj %d", pages_freed * page_k, min_score_adj);
        }

        if (report_skip_count > 0) {
            ALOGI("Suppressed %lu failed kill reports", report_skip_count);
            report_skip_count = 0;
        }

        last_report_tm = curr_tm;
    }
    if (is_waiting_for_kill()) {
        /* pause polling if we are waiting for process death notification */
        poll_params->update = POLLING_PAUSE;
    }
}

static bool init_mp_psi(enum vmpressure_level level, bool use_new_strategy) {
    int fd;

    /* Do not register a handler if threshold_ms is not set */
    if (!psi_thresholds[level].threshold_ms) {
        return true;
    }

    fd = init_psi_monitor(psi_thresholds[level].stall_type,
        psi_thresholds[level].threshold_ms * US_PER_MS,
        psi_window_size_ms * US_PER_MS);

    if (fd < 0) {
        return false;
    }

    vmpressure_hinfo[level].handler = use_new_strategy ? mp_event_psi : mp_event_common;
    vmpressure_hinfo[level].data = level;
    if (register_psi_monitor(epollfd, fd, &vmpressure_hinfo[level]) < 0) {
        destroy_psi_monitor(fd);
        return false;
    }
    maxevents++;
    mpevfd[level] = fd;

    return true;
}

static void destroy_mp_psi(enum vmpressure_level level) {
    int fd = mpevfd[level];

    if (fd < 0) {
        return;
    }

    if (unregister_psi_monitor(epollfd, fd) < 0) {
        ALOGE("Failed to unregister psi monitor for %s memory pressure; errno=%d",
            level_name[level], errno);
    }
    maxevents--;
    destroy_psi_monitor(fd);
    mpevfd[level] = -1;
}

static bool init_psi_monitors() {
    /*
     * When PSI is used on low-ram devices or on high-end devices without memfree levels
     * use new kill strategy based on zone watermarks, free swap and thrashing stats
     */
    bool use_new_strategy =
        GET_LMK_PROPERTY(bool, "use_new_strategy", low_ram_device || !use_minfree_levels);
    if (force_use_old_strategy) {
        use_new_strategy = false;
    }

    /* In default PSI mode override stall amounts using system properties */
    if (use_new_strategy) {
        /* Do not use low pressure level */
        psi_thresholds[VMPRESS_LEVEL_LOW].threshold_ms = 0;
        psi_thresholds[VMPRESS_LEVEL_MEDIUM].threshold_ms = psi_partial_stall_ms;
        psi_thresholds[VMPRESS_LEVEL_CRITICAL].threshold_ms = psi_complete_stall_ms;
    } else {
        psi_thresholds[VMPRESS_LEVEL_LOW].threshold_ms = PSI_OLD_LOW_THRESH_MS;
        psi_thresholds[VMPRESS_LEVEL_MEDIUM].threshold_ms = PSI_OLD_MED_THRESH_MS;
        psi_thresholds[VMPRESS_LEVEL_CRITICAL].threshold_ms = PSI_OLD_CRIT_THRESH_MS;
    }

    if (!init_mp_psi(VMPRESS_LEVEL_LOW, use_new_strategy)) {
        return false;
    }
    if (!init_mp_psi(VMPRESS_LEVEL_MEDIUM, use_new_strategy)) {
        destroy_mp_psi(VMPRESS_LEVEL_LOW);
        return false;
    }
    if (!init_mp_psi(VMPRESS_LEVEL_CRITICAL, use_new_strategy)) {
        destroy_mp_psi(VMPRESS_LEVEL_MEDIUM);
        destroy_mp_psi(VMPRESS_LEVEL_LOW);
        return false;
    }
    if (!init_mp_psi(VMPRESS_LEVEL_SUPER_CRITICAL, use_new_strategy)) {
        destroy_mp_psi(VMPRESS_LEVEL_CRITICAL);
        destroy_mp_psi(VMPRESS_LEVEL_MEDIUM);
        destroy_mp_psi(VMPRESS_LEVEL_LOW);
        return false;
    }
    return true;
}

static bool init_mp_common(enum vmpressure_level level) {
    int mpfd;
    int evfd;
    int evctlfd;
    char buf[256];
    struct epoll_event epev;
    int ret;
    int level_idx = (int)level;
    const char *levelstr = level_name[level_idx];

    /* gid containing AID_SYSTEM required */
    mpfd = open(MEMCG_SYSFS_PATH "memory.pressure_level", O_RDONLY | O_CLOEXEC);
    if (mpfd < 0) {
        ALOGI("No kernel memory.pressure_level support (errno=%d)", errno);
        goto err_open_mpfd;
    }

    evctlfd = open(MEMCG_SYSFS_PATH "cgroup.event_control", O_WRONLY | O_CLOEXEC);
    if (evctlfd < 0) {
        ALOGI("No kernel memory cgroup event control (errno=%d)", errno);
        goto err_open_evctlfd;
    }

    evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd < 0) {
        ALOGE("eventfd failed for level %s; errno=%d", levelstr, errno);
        goto err_eventfd;
    }

    ret = snprintf(buf, sizeof(buf), "%d %d %s", evfd, mpfd, levelstr);
    if (ret >= (ssize_t)sizeof(buf)) {
        ALOGE("cgroup.event_control line overflow for level %s", levelstr);
        goto err;
    }

    ret = TEMP_FAILURE_RETRY(write(evctlfd, buf, strlen(buf) + 1));
    if (ret == -1) {
        ALOGE("cgroup.event_control write failed for level %s; errno=%d",
              levelstr, errno);
        goto err;
    }

    epev.events = EPOLLIN;
    /* use data to store event level */
    vmpressure_hinfo[level_idx].data = level_idx;
    vmpressure_hinfo[level_idx].handler = mp_event_common;
    epev.data.ptr = (void *)&vmpressure_hinfo[level_idx];
    ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, evfd, &epev);
    if (ret == -1) {
        ALOGE("epoll_ctl for level %s failed; errno=%d", levelstr, errno);
        goto err;
    }
    maxevents++;
    mpevfd[level] = evfd;
    close(evctlfd);
    return true;

err:
    close(evfd);
err_eventfd:
    close(evctlfd);
err_open_evctlfd:
    close(mpfd);
err_open_mpfd:
    return false;
}

static void destroy_mp_common(enum vmpressure_level level) {
    struct epoll_event epev;
    int fd = mpevfd[level];

    if (fd < 0) {
        return;
    }

    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &epev)) {
        // Log an error and keep going
        ALOGE("epoll_ctl for level %s failed; errno=%d", level_name[level], errno);
    }
    maxevents--;
    close(fd);
    mpevfd[level] = -1;
}

static void kernel_event_handler(int data __unused, uint32_t events __unused,
                                 struct polling_params *poll_params __unused) {
    poll_kernel(kpoll_fd);
}

static bool init_monitors() {
    /* Try to use psi monitor first if kernel has it */
    use_psi_monitors = GET_LMK_PROPERTY(bool, "use_psi", true) &&
        init_psi_monitors();
    /* Fall back to vmpressure */
    if (!use_psi_monitors &&
        (!init_mp_common(VMPRESS_LEVEL_LOW) ||
        !init_mp_common(VMPRESS_LEVEL_MEDIUM) ||
        !init_mp_common(VMPRESS_LEVEL_CRITICAL))) {
        ALOGE("Kernel does not support memory pressure events or in-kernel low memory killer");
        return false;
    }
    if (use_psi_monitors) {
        ALOGI("Using psi monitors for memory pressure detection");
    } else {
        ALOGI("Using vmpressure for memory pressure detection");
    }
    return true;
}

static void destroy_monitors() {
    if (use_psi_monitors) {
        destroy_mp_psi(VMPRESS_LEVEL_SUPER_CRITICAL);
        destroy_mp_psi(VMPRESS_LEVEL_CRITICAL);
        destroy_mp_psi(VMPRESS_LEVEL_MEDIUM);
        destroy_mp_psi(VMPRESS_LEVEL_LOW);
    } else {
        destroy_mp_common(VMPRESS_LEVEL_CRITICAL);
        destroy_mp_common(VMPRESS_LEVEL_MEDIUM);
        destroy_mp_common(VMPRESS_LEVEL_LOW);
    }
}

static void update_psi_window_size() {
    union meminfo info;

    if (force_use_old_strategy) {
        if (!meminfo_parse(&info)) {
            /*
             * Set the optimal settings for lowram targets.
             */
            if (info.field.nr_total_pages < (int64_t)(SZ_4G / PAGE_SIZE)) {
                if (psi_window_size_ms > 500) {
                    psi_window_size_ms = 500;
                    ULMK_LOG(I, "PSI window size is changed to %dms\n", psi_window_size_ms);
                }
                if (psi_poll_period_scrit_ms < PSI_POLL_PERIOD_LONG_MS) {
                    psi_poll_period_scrit_ms = PSI_POLL_PERIOD_LONG_MS;
                    ULMK_LOG(I, "PSI poll period for super critical event is changed to %dms\n",psi_poll_period_scrit_ms);
                }
            }
        } else
            ULMK_LOG(E, "Failed to parse the meminfo\n");
    }
    /*
     * Ensure min polling period for supercritical event is no less than
     * PSI_POLL_PERIOD_SHORT_MS.
     */
    if (psi_poll_period_scrit_ms < PSI_POLL_PERIOD_SHORT_MS) {
        psi_poll_period_scrit_ms = PSI_POLL_PERIOD_SHORT_MS;
    }
}

static int init(void) {
    static struct event_handler_info kernel_poll_hinfo = { 0, kernel_event_handler };
    struct reread_data file_data = {
        .filename = ZONEINFO_PATH,
        .fd = -1,
    };
    struct epoll_event epev;
    int pidfd;
    int i;
    int ret;

    page_k = sysconf(_SC_PAGESIZE);
    if (page_k == -1)
        page_k = PAGE_SIZE;
    page_k /= 1024;

    update_psi_window_size();

    epollfd = epoll_create(MAX_EPOLL_EVENTS);
    if (epollfd == -1) {
        ALOGE("epoll_create failed (errno=%d)", errno);
        return -1;
    }

    // mark data connections as not connected
    for (int i = 0; i < MAX_DATA_CONN; i++) {
        data_sock[i].sock = -1;
    }

    ctrl_sock.sock = android_get_control_socket("lmkd");
    if (ctrl_sock.sock < 0) {
        ALOGE("get lmkd control socket failed");
        return -1;
    }

    ret = listen(ctrl_sock.sock, MAX_DATA_CONN);
    if (ret < 0) {
        ALOGE("lmkd control socket listen failed (errno=%d)", errno);
        return -1;
    }

    epev.events = EPOLLIN;
    ctrl_sock.handler_info.handler = ctrl_connect_handler;
    epev.data.ptr = (void *)&(ctrl_sock.handler_info);
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, ctrl_sock.sock, &epev) == -1) {
        ALOGE("epoll_ctl for lmkd control socket failed (errno=%d)", errno);
        return -1;
    }
    maxevents++;

    has_inkernel_module = !access(INKERNEL_MINFREE_PATH, W_OK);
    use_inkernel_interface = has_inkernel_module && !enable_userspace_lmk;

    if (use_inkernel_interface) {
        ALOGI("Using in-kernel low memory killer interface");
        if (init_poll_kernel()) {
            epev.events = EPOLLIN;
            epev.data.ptr = (void*)&kernel_poll_hinfo;
            if (epoll_ctl(epollfd, EPOLL_CTL_ADD, kpoll_fd, &epev) != 0) {
                ALOGE("epoll_ctl for lmk events failed (errno=%d)", errno);
                close(kpoll_fd);
                kpoll_fd = -1;
            } else {
                maxevents++;
                /* let the others know it does support reporting kills */
                property_set("sys.lmk.reportkills", "1");
            }
        }
    } else {
        if (!init_monitors()) {
            return -1;
        }
        /* let the others know it does support reporting kills */
        property_set("sys.lmk.reportkills", "1");
    }

    for (i = 0; i <= ADJTOSLOT(OOM_SCORE_ADJ_MAX); i++) {
        procadjslot_list[i].next = &procadjslot_list[i];
        procadjslot_list[i].prev = &procadjslot_list[i];
    }

    memset(killcnt_idx, KILLCNT_INVALID_IDX, sizeof(killcnt_idx));

    /*
     * Read zoneinfo as the biggest file we read to create and size the initial
     * read buffer and avoid memory re-allocations during memory pressure
     */
    if (reread_file(&file_data) == NULL) {
        ALOGE("Failed to read %s: %s", file_data.filename, strerror(errno));
    }

    /* check if kernel supports pidfd_open syscall */
    pidfd = TEMP_FAILURE_RETRY(pidfd_open(getpid(), 0));
    if (pidfd < 0) {
        pidfd_supported = (errno != ENOSYS);
    } else {
        pidfd_supported = true;
        close(pidfd);
    }
    ALOGI("Process polling is %s", pidfd_supported ? "supported" : "not supported" );

    return 0;
}

static bool polling_paused(struct polling_params *poll_params) {
    return poll_params->paused_handler != NULL;
}

static void resume_polling(struct polling_params *poll_params, struct timespec curr_tm) {
    poll_params->poll_start_tm = curr_tm;
    poll_params->poll_handler = poll_params->paused_handler;
    poll_params->polling_interval_ms = PSI_POLL_PERIOD_SHORT_MS;
    poll_params->paused_handler = NULL;
}

static void call_handler(struct event_handler_info* handler_info,
                         struct polling_params *poll_params, uint32_t events) {
    struct timespec curr_tm;

    poll_params->update = POLLING_DO_NOT_CHANGE;
    handler_info->handler(handler_info->data, events, poll_params);
    clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
    if (poll_params->poll_handler == handler_info) {
        poll_params->last_poll_tm = curr_tm;
    }

    switch (poll_params->update) {
    case POLLING_START:
        /*
         * Poll for the duration of PSI_WINDOW_SIZE_MS after the
         * initial PSI event because psi events are rate-limited
         * at one per sec.
         */
        poll_params->poll_start_tm = curr_tm;
        poll_params->poll_handler = handler_info;
        break;
    case POLLING_PAUSE:
        poll_params->paused_handler = handler_info;
        poll_params->poll_handler = NULL;
        break;
    case POLLING_RESUME:
        resume_polling(poll_params, curr_tm);
        break;
    case POLLING_DO_NOT_CHANGE:
        if (get_time_diff_ms(&poll_params->poll_start_tm, &curr_tm) > psi_window_size_ms) {
            /* Polled for the duration of PSI window, time to stop */
            hvalve_kill_pending = IDLE;
            poll_params->poll_handler = NULL;
            poll_params->paused_handler = NULL;
            s_crit_event = false;
            wbf_effective = wmark_boost_factor;
        }
        break;
    case POLLING_CRIT_UPGRADE:
        poll_params->poll_start_tm = curr_tm;
        if ((enum vmpressure_level)handler_info->data <= VMPRESS_LEVEL_CRITICAL)
            poll_params->poll_handler = &vmpressure_hinfo[VMPRESS_LEVEL_CRITICAL];
        break;
    }
}

static bool have_psi_events(struct epoll_event *evt, int nevents)
{
    int i;
    struct event_handler_info* handler_info;

    for (i = 0; i < nevents; i++, evt++) {
        if (evt->events & (EPOLLERR | EPOLLHUP)) {
            continue;
        }
        if (evt->data.ptr) {
            handler_info = (struct event_handler_info*)evt->data.ptr;
            if (handler_info->handler == mp_event_common) {
                return true;
            }
        }
    }

    return false;
}

static void check_cont_lmkd_events(int lvl)
{
    static struct timespec tmed, tcrit, tupgrad;
    struct timespec now, prev;

    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);

    if (lvl == VMPRESS_LEVEL_MEDIUM) {
        prev = tmed;
        tmed = now;
    } else {
        prev = tcrit;
        tcrit = now;
    }

    /*
     * Consider it as contiguous if two successive medium/critical events fall
     * in window + 1/2(window) period.
     */
    if (get_time_diff_ms(&prev, &now) < ((psi_window_size_ms * 3) >> 1)) {
        if (get_time_diff_ms(&tupgrad, &now) > psi_window_size_ms) {
            if (last_event_upgraded) {
                count_upgraded_event++;
                last_event_upgraded = false;
                tupgrad = now;
            } else {
                count_upgraded_event = 0;
            }
        }
    } else {
        count_upgraded_event = 0;
    }
}

static void mainloop(void) {
    struct event_handler_info* handler_info;
    struct polling_params poll_params;
    struct timespec curr_tm;
    struct epoll_event *evt;
    long delay = -1;

    poll_params.poll_handler = NULL;
    poll_params.paused_handler = NULL;
    union vmstat poll1, poll2;

    memset(&poll1, 0, sizeof(union vmstat));
    memset(&poll2, 0, sizeof(union vmstat));
    while (1) {
        struct epoll_event events[MAX_EPOLL_EVENTS];
        int nevents;
        int i;
        bool skip_call_handler = false;

        if (poll_params.poll_handler) {
            bool poll_now;

            clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
            if (poll_params.update == POLLING_RESUME) {
                /* Just transitioned into POLLING_RESUME, poll immediately. */
                poll_now = true;
                nevents = 0;
            } else {
                /* Calculate next timeout */
                delay = get_time_diff_ms(&poll_params.last_poll_tm, &curr_tm);
                delay = (delay < poll_params.polling_interval_ms) ?
                    poll_params.polling_interval_ms - delay : poll_params.polling_interval_ms;

                /* Wait for events until the next polling timeout */
                nevents = epoll_wait(epollfd, events, maxevents, delay);

                /* Update current time after wait */
                clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
                poll_now = (get_time_diff_ms(&poll_params.last_poll_tm, &curr_tm) >=
                    poll_params.polling_interval_ms);
            }
            if (poll_now) {
		if (force_use_old_strategy) {
			struct timespec curr_tm;

			clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
			if (s_crit_event &&
			    (get_time_diff_ms(&poll_params.poll_start_tm, &curr_tm) < psi_window_size_ms)) {
				vmstat_parse(&poll2);
				if ((nevents > 0 && have_psi_events(events, nevents)) ||
				    (!(poll2.field.pgscan_direct - poll1.field.pgscan_direct) &&
				    !(poll2.field.pgscan_kswapd - poll1.field.pgscan_kswapd) &&
				    !(poll2.field.pgscan_direct_throttle - poll1.field.pgscan_direct_throttle))) {
					skip_call_handler = true;
					/*
					 * In the case of skipping call handler, make sure that poll_params.update
					 * changes from POLLING_RESUME. If call_handler() is not skipped, this
					 * would be set there.
					 */
					poll_params.update = POLLING_DO_NOT_CHANGE;
				}
				poll1 = poll2;
			}
		}
		if (!skip_call_handler) {
			call_handler(poll_params.poll_handler, &poll_params, 0);
		}
            }
        } else {
            if (kill_timeout_ms && is_waiting_for_kill()) {
                clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
                delay = kill_timeout_ms - get_time_diff_ms(&last_kill_tm, &curr_tm);
                /* Wait for pidfds notification or kill timeout to expire */
                nevents = (delay > 0) ? epoll_wait(epollfd, events, maxevents, delay) : 0;
                if (nevents == 0) {
                    /* Kill notification timed out */
                    stop_wait_for_proc_kill(false);
                    if (polling_paused(&poll_params)) {
                        clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
                        poll_params.update = POLLING_RESUME;
                        resume_polling(&poll_params, curr_tm);
                    }
                }
            } else {
                /* Wait for events with no timeout */
                nevents = epoll_wait(epollfd, events, maxevents, -1);
            }
        }

        if (nevents == -1) {
            if (errno == EINTR)
                continue;
            ALOGE("epoll_wait failed (errno=%d)", errno);
            continue;
        }

        /*
         * First pass to see if any data socket connections were dropped.
         * Dropped connection should be handled before any other events
         * to deallocate data connection and correctly handle cases when
         * connection gets dropped and reestablished in the same epoll cycle.
         * In such cases it's essential to handle connection closures first.
         */
        for (i = 0, evt = &events[0]; i < nevents; ++i, evt++) {
            if ((evt->events & EPOLLHUP) && evt->data.ptr) {
                ALOGI("lmkd data connection dropped");
                handler_info = (struct event_handler_info*)evt->data.ptr;
                ctrl_data_close(handler_info->data);
            }
        }

        /* Second pass to handle all other events */
        int num_events = 0;
        for (i = 0, evt = &events[0]; i < nevents; ++i, evt++) {
            if (evt->events & EPOLLERR) {
                ALOGD("EPOLLERR on event #%d", i);
            }
            if (evt->events & EPOLLHUP) {
                /* This case was handled in the first pass */
                continue;
            }
            if (evt->data.ptr) {
                handler_info = (struct event_handler_info*)evt->data.ptr;
                if ((handler_info->handler == mp_event_common ||
                     handler_info->handler == mp_event_psi) &&
                    (handler_info->data == VMPRESS_LEVEL_MEDIUM ||
                     handler_info->data == VMPRESS_LEVEL_CRITICAL)) {
                    check_cont_lmkd_events(handler_info->data);
                }
                if (handler_info->handler == mp_event_psi)
                    num_events++;
                call_handler(handler_info, &poll_params, evt->events);
            }
        }
        if (num_events > 0)
            ALOGD("Number of Events: %d", num_events);
    }
}

int issue_reinit() {
    int sock;

    sock = lmkd_connect();
    if (sock < 0) {
        ALOGE("failed to connect to lmkd: %s", strerror(errno));
        return -1;
    }

    enum update_props_result res = lmkd_update_props(sock);
    switch (res) {
    case UPDATE_PROPS_SUCCESS:
        ALOGI("lmkd updated properties successfully");
        break;
    case UPDATE_PROPS_SEND_ERR:
        ALOGE("failed to send lmkd request: %s", strerror(errno));
        break;
    case UPDATE_PROPS_RECV_ERR:
        ALOGE("failed to receive lmkd reply: %s", strerror(errno));
        break;
    case UPDATE_PROPS_FORMAT_ERR:
        ALOGE("lmkd reply is invalid");
        break;
    case UPDATE_PROPS_FAIL:
        ALOGE("lmkd failed to update its properties");
        break;
    }

    close(sock);
    return res == UPDATE_PROPS_SUCCESS ? 0 : -1;
}

static void init_PreferredApps() {
    void *handle = NULL;
    handle = dlopen(IOPD_LIB, RTLD_NOW);
    if (handle != NULL) {
        perf_ux_engine_trigger = (void (*)(int, char *))dlsym(handle, "perf_ux_engine_trigger");

        if (!perf_ux_engine_trigger) {
            ALOGE("Couldn't obtain perf_ux_engine_trigger");
            enable_preferred_apps = false;
        } else {
            // Initialize preferred_apps
            preferred_apps = (char *) malloc ( PREFERRED_OUT_LENGTH * sizeof(char));
            if (preferred_apps == NULL) {
                enable_preferred_apps = false;
            } else {
                memset(preferred_apps, 0, PREFERRED_OUT_LENGTH);
                preferred_apps[0] = '\0';
            }
        }
    }
}

static void update_perf_props() {

    enable_watermark_check =
    property_get_bool("ro.lmk.enable_watermark_check", false);
    enable_preferred_apps =
    property_get_bool("ro.lmk.enable_preferred_apps", false);

    /* Loading the vendor library at runtime to access property value */
    PropVal (*perf_get_prop)(const char *, const char *) = NULL;
    void *handle = NULL;
    handle = dlopen(PERFD_LIB, RTLD_NOW);
    if (handle != NULL) {
        perf_get_prop = (PropVal (*)(const char *, const char *))dlsym(handle, "perf_get_prop");
    }

    if (!perf_get_prop) {
        ALOGE("Couldn't get perf_get_prop function handle.");
    } else {
        char property[PROPERTY_VALUE_MAX];
        char default_value[PROPERTY_VALUE_MAX];

        /*Currently only the following properties introduced by Google
        *are used outside. Hence their names are mirrored to _dup
        *If it doesnot get value via get_prop it will use the value
        *set by Google by default. To use the properties mentioned
        *above, same can be followed*/
        strlcpy(default_value, (kill_heaviest_task)? "true" : "false", PROPERTY_VALUE_MAX);
        strlcpy(property, perf_get_prop("ro.lmk.kill_heaviest_task_dup", default_value).value,
            PROPERTY_VALUE_MAX);
        kill_heaviest_task = (!strncmp(property,"false",PROPERTY_VALUE_MAX))? false : true;

        snprintf(default_value, PROPERTY_VALUE_MAX, "%lu", (kill_timeout_ms));
        strlcpy(property, perf_get_prop("ro.lmk.kill_timeout_ms_dup", default_value).value,
            PROPERTY_VALUE_MAX);
        kill_timeout_ms =  strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d",
            level_oomadj[VMPRESS_LEVEL_SUPER_CRITICAL]);
        strlcpy(property, perf_get_prop("ro.lmk.super_critical", default_value).value,
            PROPERTY_VALUE_MAX);
        level_oomadj[VMPRESS_LEVEL_SUPER_CRITICAL] = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", direct_reclaim_pressure);
        strlcpy(property, perf_get_prop("ro.lmk.direct_reclaim_pressure", default_value).value,
            PROPERTY_VALUE_MAX);
        direct_reclaim_pressure = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", PSI_WINDOW_SIZE_MS);
        strlcpy(property, perf_get_prop("ro.lmk.psi_window_size_ms", default_value).value,
            PROPERTY_VALUE_MAX);
        psi_window_size_ms = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", PSI_SCRIT_COMPLETE_STALL_MS);
        strlcpy(property, perf_get_prop("ro.lmk.psi_scrit_complete_stall_ms", default_value).value,
            PROPERTY_VALUE_MAX);
        psi_thresholds[VMPRESS_LEVEL_SUPER_CRITICAL].threshold_ms = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", PSI_POLL_PERIOD_SHORT_MS);
        strlcpy(property, perf_get_prop("ro.lmk.psi_poll_period_scrit_ms", default_value).value,
            PROPERTY_VALUE_MAX);
        psi_poll_period_scrit_ms = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", reclaim_scan_threshold);
        strlcpy(property, perf_get_prop("ro.lmk.reclaim_scan_threshold", default_value).value,
            PROPERTY_VALUE_MAX);
        reclaim_scan_threshold = strtod(property, NULL);

        strlcpy(default_value, (use_minfree_levels)? "true" : "false", PROPERTY_VALUE_MAX);
        strlcpy(property, perf_get_prop("ro.lmk.use_minfree_levels_dup", default_value).value,
            PROPERTY_VALUE_MAX);
        use_minfree_levels = (!strncmp(property,"false",PROPERTY_VALUE_MAX))? false : true;

        strlcpy(default_value, (force_use_old_strategy)? "true" : "false", PROPERTY_VALUE_MAX);
        strlcpy(property, perf_get_prop("ro.lmk.use_new_strategy_dup", default_value).value,
            PROPERTY_VALUE_MAX);
        force_use_old_strategy = (!strncmp(property,"false",PROPERTY_VALUE_MAX))? false : true;

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", PSI_CONT_EVENT_THRESH);
        strlcpy(property, perf_get_prop("ro.lmk.psi_cont_event_thresh", default_value).value,
            PROPERTY_VALUE_MAX);
        psi_cont_event_thresh = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", DEF_THRASHING);
        strlcpy(property, perf_get_prop("ro.lmk.thrashing_threshold", default_value).value,
            PROPERTY_VALUE_MAX);
        thrashing_limit_pct = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", DEF_THRASHING_DECAY);
        strlcpy(property, perf_get_prop("ro.lmk.thrashing_decay", default_value).value,
            PROPERTY_VALUE_MAX);
        thrashing_limit_decay_pct = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", DEF_LOW_SWAP);
        strlcpy(property, perf_get_prop("ro.lmk.nstrat_low_swap", default_value).value,
            PROPERTY_VALUE_MAX);
        swap_free_low_percentage = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", psi_partial_stall_ms);
        strlcpy(property, perf_get_prop("ro.lmk.nstrat_psi_partial_ms", default_value).value,
            PROPERTY_VALUE_MAX);
        psi_partial_stall_ms = strtod(property, NULL);

        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", psi_complete_stall_ms);
        strlcpy(property, perf_get_prop("ro.lmk.nstrat_psi_complete_ms", default_value).value,
            PROPERTY_VALUE_MAX);
        psi_complete_stall_ms = strtod(property, NULL);

        /*The following properties are not intoduced by Google
        *hence kept as it is */
        strlcpy(property, perf_get_prop("ro.lmk.enhance_batch_kill", "true").value, PROPERTY_VALUE_MAX);
        enhance_batch_kill = (!strncmp(property,"false",PROPERTY_VALUE_MAX))? false : true;
        strlcpy(property, perf_get_prop("ro.lmk.enable_adaptive_lmk", "false").value, PROPERTY_VALUE_MAX);
        enable_adaptive_lmk = (!strncmp(property,"false",PROPERTY_VALUE_MAX))? false : true;
        strlcpy(property, perf_get_prop("ro.lmk.enable_userspace_lmk", "false").value, PROPERTY_VALUE_MAX);
        enable_userspace_lmk = (!strncmp(property,"false",PROPERTY_VALUE_MAX))? false : true;
        strlcpy(property, perf_get_prop("ro.lmk.enable_watermark_check", "false").value, PROPERTY_VALUE_MAX);
        enable_watermark_check = (!strncmp(property,"false",PROPERTY_VALUE_MAX))? false : true;
        strlcpy(property, perf_get_prop("ro.lmk.enable_preferred_apps", "false").value, PROPERTY_VALUE_MAX);
        enable_preferred_apps = (!strncmp(property,"false",PROPERTY_VALUE_MAX))? false : true;
        snprintf(default_value, PROPERTY_VALUE_MAX, "%d", wmark_boost_factor);
        strlcpy(property,
            perf_get_prop("ro.lmk.nstrat_wmark_boost_factor", default_value).value,
            PROPERTY_VALUE_MAX);
        wmark_boost_factor = strtod(property, NULL);
        wbf_effective = wmark_boost_factor;

        snprintf(default_value, PROPERTY_VALUE_MAX, "%f", cache_percent);
        strlcpy(property, perf_get_prop("ro.lmk.cache_percent", default_value).value, PROPERTY_VALUE_MAX);
        cache_percent = (float)(strtod(property, NULL) * 0.01);

        //Update kernel interface during re-init.
        use_inkernel_interface = has_inkernel_module && !enable_userspace_lmk;
        update_psi_window_size();
    }

    /* Load IOP library for PApps */
    if (enable_preferred_apps) {
        init_PreferredApps();
    }
}

static void update_props() {
    /* By default disable low level vmpressure events */
    level_oomadj[VMPRESS_LEVEL_LOW] =
        GET_LMK_PROPERTY(int32, "low", OOM_SCORE_ADJ_MAX + 1);
    level_oomadj[VMPRESS_LEVEL_MEDIUM] =
        GET_LMK_PROPERTY(int32, "medium", 800);
    level_oomadj[VMPRESS_LEVEL_CRITICAL] =
        GET_LMK_PROPERTY(int32, "critical", 0);
    debug_process_killing = GET_LMK_PROPERTY(bool, "debug", false);
    is_userdebug_or_eng_build = property_get_bool("ro.debuggable", false);

    /* By default disable upgrade/downgrade logic */
    enable_pressure_upgrade =
        GET_LMK_PROPERTY(bool, "critical_upgrade", false);
    upgrade_pressure =
        (int64_t)GET_LMK_PROPERTY(int32, "upgrade_pressure", 100);
    downgrade_pressure =
        (int64_t)GET_LMK_PROPERTY(int32, "downgrade_pressure", 100);
    kill_heaviest_task =
        GET_LMK_PROPERTY(bool, "kill_heaviest_task", false);
    low_ram_device = property_get_bool("ro.config.low_ram", false);
    kill_timeout_ms =
        (unsigned long)GET_LMK_PROPERTY(int32, "kill_timeout_ms", 100);
    use_minfree_levels =
        GET_LMK_PROPERTY(bool, "use_minfree_levels", false);
    per_app_memcg =
        property_get_bool("ro.config.per_app_memcg", low_ram_device);
    swap_free_low_percentage = clamp(0, 100, GET_LMK_PROPERTY(int32, "swap_free_low_percentage",
        DEF_LOW_SWAP));
    psi_partial_stall_ms = GET_LMK_PROPERTY(int32, "psi_partial_stall_ms",
        low_ram_device ? DEF_PARTIAL_STALL_LOWRAM : DEF_PARTIAL_STALL);
    psi_complete_stall_ms = GET_LMK_PROPERTY(int32, "psi_complete_stall_ms",
        DEF_COMPLETE_STALL);
    thrashing_limit_pct = max(0, GET_LMK_PROPERTY(int32, "thrashing_limit",
        low_ram_device ? DEF_THRASHING_LOWRAM : DEF_THRASHING));
    thrashing_limit_decay_pct = clamp(0, 100, GET_LMK_PROPERTY(int32, "thrashing_limit_decay",
        low_ram_device ? DEF_THRASHING_DECAY_LOWRAM : DEF_THRASHING_DECAY));
    thrashing_critical_pct = max(0, GET_LMK_PROPERTY(int32, "thrashing_limit_critical",
        thrashing_limit_pct * 2));
    swap_util_max = clamp(0, 100, GET_LMK_PROPERTY(int32, "swap_util_max", 100));
    filecache_min_kb = GET_LMK_PROPERTY(int64, "filecache_min_kb", 0);

    // Update Perf Properties
    update_perf_props();
    stall_limit_critical = GET_LMK_PROPERTY(int64, "stall_limit_critical", 100);
}

int main(int argc, char **argv) {
    if ((argc > 1) && argv[1] && !strcmp(argv[1], "--reinit")) {
        if (property_set(LMKD_REINIT_PROP, "")) {
            ALOGE("Failed to reset " LMKD_REINIT_PROP " property");
        }
        return issue_reinit();
    }

    update_props();

    ctx = create_android_logger(KILLINFO_LOG_TAG);

    if (!init()) {
        if (!use_inkernel_interface) {
            /*
             * MCL_ONFAULT pins pages as they fault instead of loading
             * everything immediately all at once. (Which would be bad,
             * because as of this writing, we have a lot of mapped pages we
             * never use.) Old kernels will see MCL_ONFAULT and fail with
             * EINVAL; we ignore this failure.
             *
             * N.B. read the man page for mlockall. MCL_CURRENT | MCL_ONFAULT
             * pins ⊆ MCL_CURRENT, converging to just MCL_CURRENT as we fault
             * in pages.
             */
            /* CAP_IPC_LOCK required */
            if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) && (errno != EINVAL)) {
                ALOGW("mlockall failed %s", strerror(errno));
            }

            /* CAP_NICE required */
            struct sched_param param = {
                    .sched_priority = 1,
            };
            if (sched_setscheduler(0, SCHED_FIFO, &param)) {
                ALOGW("set SCHED_FIFO failed %s", strerror(errno));
            }
        }

        mainloop();
    }

    android_log_destroy(&ctx);

    ALOGI("exiting");
    return 0;
}
