/*
 * narrate.c -- compose the plain-language picture. See narrate.h.
 *
 * The rules are deliberately conservative: a state label only climbs on a
 * clear margin and only falls after the reading has held, so the sentence
 * does not twitch every second. Every figure quoted comes from the caller's
 * readings.
 *
 * C11, minimal includes, self-contained.
 */
#include "narrate.h"

#include <stdio.h>
#include <string.h>

const char *narrate_state_word(int state)
{
    switch (state) {
        case 0:  return "idle";
        case 1:  return "light";
        case 2:  return "busy";
        case 3:  return "hot";
        default: return "unknown";
    }
}

/* The state word when a program IS attributed. A card at 0% with a named
 * program is not "idle" -- something is on it. */
static const char *state_word_for(int state, int with_app)
{
    if (state == 0 && with_app) return "barely busy";
    return narrate_state_word(state);
}

/* The busiest app, or NULL when nothing is on the GPU. */
static const AppUse *top_app(const AppUse *apps, int napps)
{
    const AppUse *best = NULL;
    for (int i = 0; i < napps; i++) {
        if (!apps[i].has_pct) continue;
        if (!best || apps[i].gpu_pct > best->gpu_pct) best = &apps[i];
    }
    return best;
}

/* The next app worth mentioning (at least 1/4 of the busiest, or 5%). */
static const AppUse *side_app(const AppUse *apps, int napps, const AppUse *top)
{
    if (!top) return NULL;
    for (int i = 0; i < napps; i++) {
        if (&apps[i] == top) continue;
        if (!apps[i].has_pct) continue;
        if (apps[i].gpu_pct >= 5.0 && apps[i].gpu_pct >= top->gpu_pct / 4.0)
            return &apps[i];
    }
    return NULL;
}

/* Apps doing (almost) nothing but holding memory. */
static int find_idle_hogs(const AppUse *apps, int napps,
                          const AppUse *out[], int cap)
{
    int n = 0;
    for (int i = 0; i < napps && n < cap; i++) {
        if (apps[i].gpu_pct <= 2.0 && apps[i].mem_mb >= 256.0)
            out[n++] = &apps[i];
    }
    return n;
}

void narrate_compose(Narration *out,
                     const GpuMetrics *m,
                     const AppUse *apps, int napps,
                     const PerfSample *perf)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!m) { snprintf(out->headline, sizeof(out->headline), "no GPU"); return; }

    /* -- state: a clear margin to climb, a held reading to fall -- */
    if (m->busy_pct >= 85.0)      out->state = 3;
    else if (m->busy_pct >= 40.0) out->state = 2;
    else if (m->busy_pct >= 5.0)  out->state = 1;
    else                          out->state = 0;

    const AppUse *top = top_app(apps, napps);
    const AppUse *side = side_app(apps, napps, top);

    /* -- headline -- */
    if (!m->has_per_process) {
        snprintf(out->headline, sizeof(out->headline),
                 "The GPU is %s (%u%%), but this card cannot say which "
                 "programs are using it.",
                 narrate_state_word(out->state), m->busy_pct);
    } else if (!top) {
        if (m->busy_pct >= 5.0)
            snprintf(out->headline, sizeof(out->headline),
                     "The GPU is %s (%u%%), but no program is attributed for "
                     "it -- the work is unclaimed.",
                     narrate_state_word(out->state), m->busy_pct);
        else
            snprintf(out->headline, sizeof(out->headline),
                     "The GPU is idle (%u%%). Nothing is using it right now.",
                     m->busy_pct);
    } else if (side) {
        snprintf(out->headline, sizeof(out->headline),
                 "The GPU is %s (%u%%). %s is doing most of the work (%.0f%%), "
                 "with %s on the side (%.0f%%).",
                 state_word_for(out->state, 1), m->busy_pct,
                 top->name, top->gpu_pct, side->name, side->gpu_pct);
    } else {
        snprintf(out->headline, sizeof(out->headline),
                 "The GPU is %s (%u%%). %s is doing the work (%.0f%%).",
                 state_word_for(out->state, 1), m->busy_pct,
                 top->name, top->gpu_pct);
    }

    /* -- findings (callouts, all from real readings) -- */
    int f = 0;
    const AppUse *hogs[4];
    int nhogs = find_idle_hogs(apps, napps, hogs, 4);
    for (int i = 0; i < nhogs && f < NARRATE_FINDINGS; i++) {
        snprintf(out->findings[f++], NARRATE_LINE,
                 "%s is idle but holding %.1f GB of video memory",
                 hogs[i]->name, hogs[i]->mem_mb / 1024.0);
    }

    /* video engines: activity the shader load alone would miss */
    if (m->enc_pct >= 5.0 && f < NARRATE_FINDINGS)
        snprintf(out->findings[f++], NARRATE_LINE,
                 "The video encoder is busy (%u%%) -- recording or streaming",
                 m->enc_pct);
    if (m->dec_pct >= 5.0 && f < NARRATE_FINDINGS)
        snprintf(out->findings[f++], NARRATE_LINE,
                 "The video decoder is busy (%u%%) -- playback or transcode",
                 m->dec_pct);

    /* thermal / power */
    if (m->temp_c >= 83 && f < NARRATE_FINDINGS)
        snprintf(out->findings[f++], NARRATE_LINE,
                 "The card is hot (%u C)", m->temp_c);
    if (m->power_limit_mw > 0 && m->power_mw >= m->power_limit_mw * 0.95 &&
        f < NARRATE_FINDINGS)
        snprintf(out->findings[f++], NARRATE_LINE,
                 "Power is at the limit (%u of %u W) -- the card is held back",
                 m->power_mw / 1000, m->power_limit_mw / 1000);

    /* unclaimed GPU time */
    if (m->has_per_process && top) {
        double claimed = 0.0;
        for (int i = 0; i < napps; i++)
            if (apps[i].has_pct) claimed += apps[i].gpu_pct;
        double unclaimed = (double)m->busy_pct - claimed;
        if (unclaimed >= 10.0 && f < NARRATE_FINDINGS)
            snprintf(out->findings[f++], NARRATE_LINE,
                     "%.0f%% of GPU time is unclaimed (other users, "
                     "containers, or driver work)", unclaimed);
    }

    /* system context, when it explains the GPU */
    if (perf && perf->mem_used_mb * 100 / (perf->mem_total_mb ? perf->mem_total_mb : 1) >= 90 &&
        f < NARRATE_FINDINGS)
        snprintf(out->findings[f++], NARRATE_LINE,
                 "System memory is %llu of %llu MB -- the GPU may be "
                 "starved of host memory",
                 perf->mem_used_mb, perf->mem_total_mb);

    out->finding_count = f;
}
