/*
 * narrate.h -- the plain-language layer.
 *
 * Turns readings into a sentence the way a person would say it. Every word is
 * derived from a real number; no number appears that was not measured.
 *
 * The caller resolves each GPU process into an AppUse (name, kind, figures);
 * this module only composes, so it can be tested with no GPU and no /proc.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_NARRATE_H
#define WUBULENS_NARRATE_H

#include "gpu.h"
#include "perf.h"

#define NARRATE_LINE 256
#define NARRATE_FINDINGS 4

typedef struct {
    unsigned int pid;
    char     name[64];
    char     kind[16];       /* ai, game, video, creative, browser, desktop,
                                dev, system, other */
    double   gpu_pct;        /* SM utilisation 0..100 */
    double   enc_pct;        /* video encoder 0..100 */
    double   dec_pct;        /* video decoder 0..100 */
    double   mem_mb;
    bool     has_pct;        /* false when the card cannot attribute */
    unsigned int pids[8];    /* every process in the group (end as a tree) */
    int      npids;
} AppUse;

typedef struct {
    char headline[NARRATE_LINE];
    char findings[NARRATE_FINDINGS][NARRATE_LINE];
    int  finding_count;
    int  state;              /* 0 idle, 1 light, 2 busy, 3 hot */
} Narration;

void narrate_compose(Narration *out,
                     const GpuMetrics *m,
                     const AppUse *apps, int napps,
                     const PerfSample *perf);

const char *narrate_state_word(int state);

#endif /* WUBULENS_NARRATE_H */
