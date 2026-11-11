/*
 * snapshot.h -- render the lens as text or JSON.
 *
 * `snapshot` is the scriptable surface: the same picture a person reads, in
 * a form a script or an agent can consume. The JSON schema number only
 * changes on a breaking change.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_SNAPSHOT_H
#define WUBULENS_SNAPSHOT_H

#include "gpu.h"
#include "narrate.h"
#include "perf.h"

#define SNAPSHOT_SCHEMA 1

typedef struct {
    GpuMetrics  gpu;
    bool        have_gpu;
    GpuProc     procs[64];
    int         nprocs;
    AppUse      apps[64];
    int         napps;
    Narration   story;
    PerfSample  perf;
    bool        have_perf;
    const char *gpu_error;      /* why there is no GPU, or "" */
} Snapshot;

/* Fill a snapshot: sample the card, resolve each GPU process into an app
 * (name + kind), and compose the narration. */
void snapshot_take(Snapshot *s, struct GpuBackend *b);

/* Render into buf. json != 0 selects the machine-readable form. */
void snapshot_render(const Snapshot *s, char *buf, unsigned cap, int json);

#endif /* WUBULENS_SNAPSHOT_H */
