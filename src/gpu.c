/*
 * gpu.c -- the NVML and FAKE backends behind one interface. See gpu.h.
 *
 * The NVML library is loaded at run time (dlopen), so the binary needs no
 * NVML headers or development package to build, and starts on any machine:
 * when the driver library is absent the lens says so instead of failing.
 *
 * Only the handful of NVML types and constants this file uses are declared
 * here, matching the driver's ABI.
 *
 * C11, minimal includes, self-contained.
 */
#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

/* -- NVML ABI (the subset we use) ---------------------------------------- */
typedef unsigned int nvmlReturn_t;
typedef void        *nvmlDevice_t;

typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
typedef struct {
    unsigned int        pid;
    unsigned long long  timeStamp;
    unsigned int        smUtil;
    unsigned int        memUtil;
    unsigned int        encUtil;
    unsigned int        decUtil;
} nvmlProcUtilSample_t;

/* nvmlProcessInfo_t: pid + video memory in bytes (the utilization samples
 * carry no memory, so per-app VRAM comes from here). */
typedef struct {
    unsigned int        pid;
    unsigned long long  usedGpuMemory;
} nvmlProcInfo_t;

#define NVML_SUCCESS                 0
#define NVML_ERROR_NOT_SUPPORTED     3
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_ERROR_GPU_IS_IDLE       15
#define NVML_ERROR_NOT_FOUND         6
#define NVML_TEMPERATURE_GPU         0
#define NVML_CLOCK_SM                1
#define NVML_CLOCK_MEM               2
#define NVML_PCIE_UTIL_TX_BYTES      0
#define NVML_PCIE_UTIL_RX_BYTES      1

/* -- function pointer types ---------------------------------------------- */
typedef nvmlReturn_t (*fn_init)(void);
typedef nvmlReturn_t (*fn_shutdown)(void);
typedef nvmlReturn_t (*fn_get_count)(unsigned int *);
typedef nvmlReturn_t (*fn_get_handle)(unsigned int, nvmlDevice_t *);
typedef nvmlReturn_t (*fn_get_name)(nvmlDevice_t, char *, unsigned int);
typedef nvmlReturn_t (*fn_get_util)(nvmlDevice_t, nvmlUtilization_t *);
typedef nvmlReturn_t (*fn_get_mem)(nvmlDevice_t, nvmlMemory_t *);
typedef nvmlReturn_t (*fn_get_power)(nvmlDevice_t, unsigned int *);
typedef nvmlReturn_t (*fn_get_power_limit)(nvmlDevice_t, unsigned int *);
typedef nvmlReturn_t (*fn_get_temp)(nvmlDevice_t, unsigned int, unsigned int *);
typedef nvmlReturn_t (*fn_get_clock)(nvmlDevice_t, unsigned int, unsigned int, unsigned int *);
typedef nvmlReturn_t (*fn_get_pcie)(nvmlDevice_t, unsigned int, unsigned int *);
typedef nvmlReturn_t (*fn_get_enc)(nvmlDevice_t, unsigned int *, unsigned int *);
typedef nvmlReturn_t (*fn_get_dec)(nvmlDevice_t, unsigned int *, unsigned int *);
typedef nvmlReturn_t (*fn_get_proc_util)(nvmlDevice_t, nvmlProcUtilSample_t *,
                                         unsigned int *, unsigned long long);
typedef nvmlReturn_t (*fn_get_procs)(nvmlDevice_t, unsigned int *, nvmlProcInfo_t *);

#define GPU_MAX_GPUS   8
#define GPU_MAX_PROCS  256      /* the per-GPU sample buffer we start with */
#define GPU_MAX_PROCS_GROW 1024 /* the cap after an INSUFFICIENT_SIZE retry */
#define GPU_NAME_LEN   128

struct GpuBackend {
    GpuKind   kind;
    char      err[256];
    int       count;

    GpuMetrics metrics[GPU_MAX_GPUS];
    GpuProc    procs[GPU_MAX_GPUS][GPU_MAX_PROCS_GROW];
    int        proc_count[GPU_MAX_GPUS];

    /* NVML handles */
    void          *lib;
    nvmlDevice_t   dev[GPU_MAX_GPUS];
    unsigned long long last_ts[GPU_MAX_GPUS];
    fn_init             p_init;
    fn_shutdown         p_shutdown;
    fn_get_count        p_get_count;
    fn_get_handle       p_get_handle;
    fn_get_name         p_get_name;
    fn_get_util         p_get_util;
    fn_get_mem          p_get_mem;
    fn_get_power        p_get_power;
    fn_get_power_limit  p_get_power_limit;
    fn_get_temp         p_get_temp;
    fn_get_clock        p_get_clock;
    fn_get_pcie         p_get_pcie;
    fn_get_enc          p_get_enc;
    fn_get_dec          p_get_dec;
    fn_get_proc_util    p_get_proc_util;
    fn_get_procs        p_get_compute_procs;
    fn_get_procs        p_get_graphics_procs;
};

static char g_open_error[256];
const char *gpu_last_open_error(void) { return g_open_error; }

static void set_err(struct GpuBackend *b, const char *what, const char *detail)
{
    if (!b) return;
    if (detail)
        snprintf(b->err, sizeof(b->err), "%s: %s", what, detail);
    else
        snprintf(b->err, sizeof(b->err), "%s", what);
}

static void *sym(void *lib, const char *name)
{
    return dlsym(lib, name);
}

/* -- NVML backend -------------------------------------------------------- */

static int nvml_load(struct GpuBackend *b)
{
    const char *names[] = {
        "libnvidia-ml.so.1", "libnvidia-ml.so", "libnvidia-ml.so.615.71.09"
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        b->lib = dlopen(names[i], RTLD_LAZY);
        if (b->lib) break;
    }
    if (!b->lib) {
        snprintf(g_open_error, sizeof(g_open_error),
                 "NVML not available (dlopen failed: %s)", dlerror());
        return -1;
    }

    b->p_init            = (fn_init)           sym(b->lib, "nvmlInit_v2");
    if (!b->p_init)      b->p_init            = (fn_init)           sym(b->lib, "nvmlInit");
    b->p_shutdown        = (fn_shutdown)       sym(b->lib, "nvmlShutdown");
    b->p_get_count       = (fn_get_count)      sym(b->lib, "nvmlDeviceGetCount_v2");
    if (!b->p_get_count) b->p_get_count       = (fn_get_count)      sym(b->lib, "nvmlDeviceGetCount");
    b->p_get_handle      = (fn_get_handle)     sym(b->lib, "nvmlDeviceGetHandleByIndex_v2");
    if (!b->p_get_handle) b->p_get_handle     = (fn_get_handle)     sym(b->lib, "nvmlDeviceGetHandleByIndex");
    b->p_get_name        = (fn_get_name)       sym(b->lib, "nvmlDeviceGetName");
    b->p_get_util        = (fn_get_util)       sym(b->lib, "nvmlDeviceGetUtilizationRates");
    b->p_get_mem         = (fn_get_mem)        sym(b->lib, "nvmlDeviceGetMemoryInfo");
    b->p_get_power       = (fn_get_power)      sym(b->lib, "nvmlDeviceGetPowerUsage");
    b->p_get_power_limit = (fn_get_power_limit) sym(b->lib, "nvmlDeviceGetPowerManagementLimit");
    b->p_get_temp        = (fn_get_temp)       sym(b->lib, "nvmlDeviceGetTemperature");
    b->p_get_clock       = (fn_get_clock)      sym(b->lib, "nvmlDeviceGetClockInfo");
    b->p_get_pcie        = (fn_get_pcie)       sym(b->lib, "nvmlDeviceGetPcieThroughput");
    b->p_get_enc         = (fn_get_enc)        sym(b->lib, "nvmlDeviceGetEncoderUtilization");
    b->p_get_dec         = (fn_get_dec)        sym(b->lib, "nvmlDeviceGetDecoderUtilization");
    b->p_get_proc_util   = (fn_get_proc_util)  sym(b->lib, "nvmlDeviceGetProcessUtilization");
    b->p_get_compute_procs = (fn_get_procs) sym(b->lib, "nvmlDeviceGetComputeRunningProcesses_v3");
    b->p_get_graphics_procs = (fn_get_procs) sym(b->lib, "nvmlDeviceGetGraphicsRunningProcesses_v3");

    if (!b->p_init || !b->p_shutdown || !b->p_get_count || !b->p_get_handle ||
        !b->p_get_util || !b->p_get_mem) {
        snprintf(g_open_error, sizeof(g_open_error),
                 "NVML loaded but core functions are missing");
        return -1;
    }
    return 0;
}

static bool nvml_handles(struct GpuBackend *b)
{
    unsigned int n = 0;
    if (b->p_get_count && b->p_get_count(&n) != NVML_SUCCESS) {
        set_err(b, "nvmlDeviceGetCount failed", NULL);
        return false;
    }
    if (n > GPU_MAX_GPUS) n = GPU_MAX_GPUS;
    for (unsigned i = 0; i < n; i++) {
        if (b->p_get_handle && b->p_get_handle(i, &b->dev[i]) != NVML_SUCCESS) {
            set_err(b, "nvmlDeviceGetHandleByIndex failed", NULL);
            return false;
        }
    }
    b->count = (int)n;
    return true;
}

struct GpuBackend *gpu_open_nvml(void)
{
    g_open_error[0] = '\0';
    struct GpuBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->kind = GPU_KIND_NVML;

    if (nvml_load(b) != 0) { gpu_close(b); return NULL; }
    if (b->p_init() != NVML_SUCCESS) {
        snprintf(g_open_error, sizeof(g_open_error), "nvmlInit failed");
        gpu_close(b);
        return NULL;
    }
    if (!nvml_handles(b)) { gpu_close(b); return NULL; }
    return b;
}

/* -- FAKE backend (a scripted card, for tests) ---------------------------- */

struct GpuBackend *gpu_open_fake(void)
{
    struct GpuBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->kind = GPU_KIND_FAKE;
    b->count = 1;
    snprintf(b->metrics[0].name, sizeof(b->metrics[0].name), "WuBu Fake GPU (demo)");
    b->metrics[0].has_per_process = true;
    b->metrics[0].busy_pct = 42;
    b->metrics[0].mem_total_mb = 8192;
    b->metrics[0].mem_used_mb = 2048;
    b->metrics[0].power_mw = 61000;
    b->metrics[0].power_limit_mw = 115000;
    b->metrics[0].temp_c = 58;
    b->metrics[0].clock_sm_mhz = 2100;
    b->metrics[0].clock_mem_mhz = 7750;
    b->metrics[0].enc_pct = 0;
    b->metrics[0].dec_pct = 0;

    b->proc_count[0] = 2;
    /* The demo names this very process, so the app row resolves to a real
     * program name instead of "process <pid>". */
    b->procs[0][0].pid = (unsigned)getpid();
    b->procs[0][0].mem_mb = 1024;
    b->procs[0][0].sm_pct = 36;
    b->procs[0][0].mem_pct = 36;
    b->procs[0][0].enc_pct = 0;
    b->procs[0][0].dec_pct = 0;
    b->procs[0][0].has_pct = true;
    b->procs[0][1].pid = 7777;
    b->procs[0][1].mem_mb = 800;
    b->procs[0][1].sm_pct = 6;
    b->procs[0][1].mem_pct = 6;
    b->procs[0][1].has_pct = true;
    return b;
}

/* -- shared API ---------------------------------------------------------- */

void gpu_close(struct GpuBackend *b)
{
    if (!b) return;
    if (b->kind == GPU_KIND_NVML && b->lib) {
        if (b->p_shutdown) b->p_shutdown();
        dlclose(b->lib);
    }
    free(b);
}

GpuKind gpu_kind(const struct GpuBackend *b) { return b ? b->kind : GPU_KIND_NONE; }
int gpu_count(const struct GpuBackend *b) { return b ? b->count : 0; }
const char *gpu_error(const struct GpuBackend *b) { return b ? b->err : ""; }

const GpuMetrics *gpu_metrics(const struct GpuBackend *b, int idx)
{
    if (!b || idx < 0 || idx >= b->count) return NULL;
    return &b->metrics[idx];
}

int gpu_procs(const struct GpuBackend *b, int idx, GpuProc *out, int cap)
{
    if (!b || idx < 0 || idx >= b->count || !out || cap <= 0) return 0;
    int n = b->proc_count[idx];
    if (n > cap) n = cap;
    memcpy(out, b->procs[idx], (size_t)n * sizeof(GpuProc));
    return n;
}

/* -- NVML sampling ------------------------------------------------------- */


/* Fill per-process VIDEO MEMORY. The utilization samples carry percentages
 * only, so memory comes from the running-process list (compute + graphics).
 * Processes that hold memory but did nothing in the window get a row with
 * has_pct = false -- they are the "idle but hogging" callouts. */
static void nvml_proc_memory(struct GpuBackend *b, int idx)
{
    if (!b->p_get_compute_procs && !b->p_get_graphics_procs) return;

    nvmlProcInfo_t info[GPU_MAX_PROCS_GROW];
    unsigned int total = 0;

    if (b->p_get_compute_procs) {
        unsigned int c = GPU_MAX_PROCS_GROW;
        if (b->p_get_compute_procs(b->dev[idx], &c, info) == NVML_SUCCESS)
            total += c;
    }
    if (b->p_get_graphics_procs) {
        unsigned int c = GPU_MAX_PROCS_GROW - total;
        if (c > 0 && b->p_get_graphics_procs(b->dev[idx], &c, info + total) == NVML_SUCCESS)
            total += c;
    }
    if (total == 0) return;

    for (unsigned int i = 0; i < total; i++) {
        unsigned long long bytes = info[i].usedGpuMemory;
        if (bytes == 0xFFFFFFFFFFFFFFFFULL) continue;   /* not reported */
        double mb = (double)(bytes / (1024ULL * 1024ULL));

        /* match a row from the utilization samples */
        GpuProc *row = NULL;
        for (int j = 0; j < b->proc_count[idx]; j++) {
            if (b->procs[idx][j].pid == info[i].pid) { row = &b->procs[idx][j]; break; }
        }
        if (row) { row->mem_mb = mb; continue; }

        /* memory-only process: add a row with no percentage */
        if (b->proc_count[idx] < GPU_MAX_PROCS_GROW) {
            GpuProc *p = &b->procs[idx][b->proc_count[idx]++];
            memset(p, 0, sizeof(*p));
            p->pid = info[i].pid;
            p->mem_mb = mb;
            p->has_pct = false;
        }
    }
}

static void nvml_sample_one(struct GpuBackend *b, int idx)
{
    GpuMetrics *m = &b->metrics[idx];
    memset(m, 0, sizeof(*m));
    m->index = idx;
    m->has_per_process = false;

    if (b->p_get_name)
        b->p_get_name(b->dev[idx], m->name, GPU_NAME_LEN);

    nvmlUtilization_t util;
    if (b->p_get_util && b->p_get_util(b->dev[idx], &util) == NVML_SUCCESS)
        m->busy_pct = util.gpu;

    nvmlMemory_t mem;
    if (b->p_get_mem && b->p_get_mem(b->dev[idx], &mem) == NVML_SUCCESS) {
        m->mem_total_mb = mem.total / (1024ULL * 1024ULL);
        m->mem_used_mb  = mem.used  / (1024ULL * 1024ULL);
    }

    if (b->p_get_power)     b->p_get_power(b->dev[idx], &m->power_mw);
    if (b->p_get_power_limit) b->p_get_power_limit(b->dev[idx], &m->power_limit_mw);
    if (b->p_get_temp)      b->p_get_temp(b->dev[idx], NVML_TEMPERATURE_GPU, &m->temp_c);
    if (b->p_get_clock) {
        b->p_get_clock(b->dev[idx], NVML_CLOCK_SM,  0, &m->clock_sm_mhz);
        b->p_get_clock(b->dev[idx], NVML_CLOCK_MEM, 0, &m->clock_mem_mhz);
    }
    if (b->p_get_pcie) {
        b->p_get_pcie(b->dev[idx], NVML_PCIE_UTIL_RX_BYTES, &m->pcie_rx_kbps);
        b->p_get_pcie(b->dev[idx], NVML_PCIE_UTIL_TX_BYTES, &m->pcie_tx_kbps);
    }
    if (b->p_get_enc) {
        unsigned int period = 0;
        b->p_get_enc(b->dev[idx], &m->enc_pct, &period);
    }
    if (b->p_get_dec) {
        unsigned int period = 0;
        b->p_get_dec(b->dev[idx], &m->dec_pct, &period);
    }

    /* Per-process utilisation: ask for samples newer than last_ts. The
     * first call uses 0, which returns whatever the driver holds. The
     * driver answers INSUFFICIENT_SIZE with the size it needs, so grow and
     * ask once more rather than dropping the figures. */
    if (b->p_get_proc_util) {
        nvmlProcUtilSample_t stackbuf[GPU_MAX_PROCS];
        nvmlProcUtilSample_t *samples = stackbuf;
        unsigned int count = GPU_MAX_PROCS;
        unsigned long long ts = b->last_ts[idx];
        nvmlReturn_t rc = b->p_get_proc_util(b->dev[idx], samples, &count, ts);

        if (rc == NVML_ERROR_INSUFFICIENT_SIZE && count > GPU_MAX_PROCS &&
            count <= GPU_MAX_PROCS_GROW) {
            samples = calloc(count, sizeof(*samples));
            if (samples) {
                unsigned int c2 = count;
                rc = b->p_get_proc_util(b->dev[idx], samples, &c2, ts);
                count = c2;
            } else {
                count = GPU_MAX_PROCS;
                samples = stackbuf;
                rc = NVML_ERROR_INSUFFICIENT_SIZE;
            }
        }

        if (rc == NVML_SUCCESS) {
            /* The driver wrote `count` samples into the buffer. */
            m->has_per_process = true;
            int n = (count > GPU_MAX_PROCS_GROW) ? GPU_MAX_PROCS_GROW : (int)count;
            unsigned long long newest = ts;
            for (int i = 0; i < n; i++) {
                GpuProc *p = &b->procs[idx][i];
                memset(p, 0, sizeof(*p));
                p->pid      = samples[i].pid;
                p->sm_pct   = samples[i].smUtil;
                p->mem_pct  = samples[i].memUtil;
                p->enc_pct  = samples[i].encUtil;
                p->dec_pct  = samples[i].decUtil;
                p->has_pct  = true;
                if (samples[i].timeStamp > newest) newest = samples[i].timeStamp;
            }
            b->proc_count[idx] = n;
            if (newest > ts) b->last_ts[idx] = newest;
        } else if (rc == NVML_ERROR_GPU_IS_IDLE || rc == NVML_ERROR_NOT_FOUND) {
            /* The card CAN report per-process data; there is simply nothing
             * on the GPU right now. NOT_FOUND means the driver wrote ZERO
             * samples -- the buffer must not be read. */
            m->has_per_process = true;
            b->proc_count[idx] = 0;
        } else if (rc == NVML_ERROR_NOT_SUPPORTED) {
            m->has_per_process = false;   /* the card says so; we do not guess */
        }

        if (samples != stackbuf) free(samples);
    }

    /* Memory comes from the process list, not the utilization samples. */
    nvml_proc_memory(b, idx);
}

bool gpu_sample(struct GpuBackend *b)
{
    if (!b) return false;
    if (b->kind == GPU_KIND_FAKE) {
        /* vary the scripted numbers a little so a live view shows motion */
        static unsigned tick = 0;
        tick++;
        b->metrics[0].busy_pct = 40 + (tick % 5);
        b->procs[0][0].sm_pct = 30 + (tick % 9);
        return true;
    }
    if (b->kind != GPU_KIND_NVML) return false;
    for (int i = 0; i < b->count; i++)
        nvml_sample_one(b, i);
    return b->count > 0;
}
