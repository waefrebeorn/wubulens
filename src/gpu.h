/*
 * gpu.h -- the GPU side of the lens: one interface, two backends.
 *
 *   NVML   -- the real card, loaded at run time with dlopen (so the binary
 *             builds and runs anywhere and says what is missing)
 *   FAKE   -- a scripted card, so the whole pipeline is testable with no GPU
 *
 * Per-process figures come from nvmlDeviceGetProcessUtilization: SM, memory,
 * encoder and decoder utilisation per pid. A GPU that cannot report
 * per-process data (older cards, MIG, vGPU) sets has_per_process = false and
 * the lens says so instead of inventing numbers.
 *
 * C11, opaque backend, minimal includes.
 */
#ifndef WUBULENS_GPU_H
#define WUBULENS_GPU_H

#include <stdbool.h>

typedef enum { GPU_KIND_NONE = 0, GPU_KIND_NVML, GPU_KIND_FAKE } GpuKind;

typedef struct {
    int      index;
    char     name[128];
    bool     has_per_process;

    unsigned int      busy_pct;          /* overall 3D/compute utilisation */
    unsigned long long mem_total_mb;
    unsigned long long mem_used_mb;
    unsigned int      power_mw;
    unsigned int      power_limit_mw;
    unsigned int      temp_c;
    unsigned int      clock_sm_mhz;
    unsigned int      clock_mem_mhz;
    unsigned int      pcie_rx_kbps;
    unsigned int      pcie_tx_kbps;
    unsigned int      enc_pct;
    unsigned int      dec_pct;
} GpuMetrics;

typedef struct {
    unsigned int      pid;
    unsigned long long mem_mb;
    unsigned int      sm_pct;
    unsigned int      mem_pct;
    unsigned int      enc_pct;
    unsigned int      dec_pct;
    bool              has_pct;           /* false: memory-only reporting */
} GpuProc;

struct GpuBackend;

/* Open the real card. Returns NULL when NVML is missing or fails; the
 * reason is available from gpu_error() on a non-NULL backend only, so call
 * gpu_last_open_error() after a NULL return. */
struct GpuBackend *gpu_open_nvml(void);
struct GpuBackend *gpu_open_fake(void);
void               gpu_close(struct GpuBackend *b);
const char        *gpu_last_open_error(void);

GpuKind            gpu_kind(const struct GpuBackend *b);
int                gpu_count(const struct GpuBackend *b);
const char        *gpu_error(const struct GpuBackend *b);

/* Refresh every GPU's metrics. Returns true when at least one sample landed. */
bool               gpu_sample(struct GpuBackend *b);

/* Metrics for GPU idx, or NULL. Valid until the next gpu_sample(). */
const GpuMetrics  *gpu_metrics(const struct GpuBackend *b, int idx);

/* Per-process figures for GPU idx. Returns the count written to out[]. */
int                gpu_procs(const struct GpuBackend *b, int idx,
                             GpuProc *out, int cap);

#endif /* WUBULENS_GPU_H */
