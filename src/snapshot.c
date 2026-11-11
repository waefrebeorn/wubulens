/*
 * snapshot.c -- fill and render the lens picture. See snapshot.h.
 *
 * C11, minimal includes, self-contained.
 */
#include "snapshot.h"
#include "classify.h"
#include "proc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int cmp_apps(const void *a, const void *b)
{
    const AppUse *x = (const AppUse *)a;
    const AppUse *y = (const AppUse *)b;
    if (x->gpu_pct > y->gpu_pct) return -1;
    if (x->gpu_pct < y->gpu_pct) return 1;
    if (x->mem_mb > y->mem_mb) return -1;
    if (x->mem_mb < y->mem_mb) return 1;
    return 0;
}

void snapshot_take(Snapshot *s, struct GpuBackend *b)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));

    /* system resources */
    wubu_perf_sample(&s->perf);
    s->have_perf = wubu_perf_available();

    if (!b) {
        s->gpu_error = gpu_last_open_error();
        return;
    }
    if (!gpu_sample(b)) {
        s->gpu_error = gpu_error(b);
        return;
    }

    const GpuMetrics *m = gpu_metrics(b, 0);
    if (!m) { s->gpu_error = "no GPU"; return; }
    s->gpu = *m;
    s->have_gpu = true;

    GpuProc procs[64];
    int np = gpu_procs(b, 0, procs, 64);
    s->nprocs = np < 64 ? np : 64;
    memcpy(s->procs, procs, (size_t)s->nprocs * sizeof(GpuProc));

    /* Resolve each GPU process into an app, merging the processes that share
     * one app (Chromium's helpers, Steam's launcher, a container). */
    for (int i = 0; i < s->nprocs; i++) {
        ProcInfo pi;
        if (!proc_lookup(procs[i].pid, &pi)) {
            /* the process vanished between the driver's reading and ours */
            pi.exe[0] = '\0';
            pi.cmdline[0] = '\0';
            snprintf(pi.exe, sizeof(pi.exe), "process %u", procs[i].pid);
        }

        char key[128];
        classify_group_key(pi.exe, pi.cmdline, key, sizeof(key));

        AppUse *app = NULL;
        for (int j = 0; j < s->napps; j++) {
            if (strcmp(s->apps[j].name, key) == 0) { app = &s->apps[j]; break; }
        }
        if (!app && s->napps < 64) {
            app = &s->apps[s->napps++];
            memset(app, 0, sizeof(*app));
            snprintf(app->name, sizeof(app->name), "%.*s",
                     (int)sizeof(app->name) - 1, key);
            app->kind[0] = '\0';
            AppKind k = classify_kind(pi.exe, pi.cmdline);
            snprintf(app->kind, sizeof(app->kind), "%s", classify_kind_name(k));
        }
        if (!app) continue;

        app->mem_mb += (double)procs[i].mem_mb;
        if (procs[i].has_pct) {
            app->gpu_pct += (double)procs[i].sm_pct;
            app->has_pct = true;
        }
        if (app->pid == 0) app->pid = procs[i].pid;
        if (app->npids < 8) app->pids[app->npids++] = procs[i].pid;
    }

    qsort(s->apps, (size_t)s->napps, sizeof(AppUse), cmp_apps);
    narrate_compose(&s->story, &s->gpu, s->apps, s->napps, &s->perf);
}

/* -- text ---------------------------------------------------------------- */

static void render_text(const Snapshot *s, char *buf, unsigned cap)
{
    unsigned w = 0;
    if (!s->have_gpu) {
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                "No GPU readable%s%s\n",
                                s->gpu_error ? ": " : "",
                                s->gpu_error ? s->gpu_error : "");
        return;
    }

    const GpuMetrics *m = &s->gpu;
    w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                            "%s\n"
                            "  %u%% busy   %.1f/%.1f GB   %u C   %u W",
                            m->name,
                            m->busy_pct,
                            (double)m->mem_used_mb / 1024.0,
                            (double)m->mem_total_mb / 1024.0,
                            m->temp_c, m->power_mw / 1000);
    if (m->power_limit_mw > 0)
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                " / %u W", m->power_limit_mw / 1000);
    w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "\n\n");

    w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "%s\n",
                            s->story.headline);
    for (int i = 0; i < s->story.finding_count; i++)
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                "  - %s\n", s->story.findings[i]);

    if (s->napps > 0) {
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                "\n  %-24s %6s  %10s  %s\n",
                                "APP", "GPU%", "MEMORY", "KIND");
        for (int i = 0; i < s->napps; i++) {
            const AppUse *a = &s->apps[i];
            char pct[16];
            if (a->has_pct) snprintf(pct, sizeof(pct), "%.0f", a->gpu_pct);
            else            snprintf(pct, sizeof(pct), "-");
            w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                    "  %-24s %6s  %8.1f MB  %s\n",
                                    a->name, pct, a->mem_mb, a->kind);
        }
    }

    if (s->have_perf) {
        const PerfSample *p = &s->perf;
        char up[64];
        wubu_perf_uptime_str(p, up, sizeof(up));
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                "\n  CPU %.0f%%   MEM %llu/%llu MB   "
                                "DISK %.1f/%.1f MB/s   NET %.2f/%.2f MB/s   UP %s\n",
                                p->cpu_total_pct,
                                p->mem_used_mb, p->mem_total_mb,
                                p->disk_read_mbps, p->disk_write_mbps,
                                p->net_rx_mbps, p->net_tx_mbps, up);
    }
}

/* -- JSON ---------------------------------------------------------------- */

static void json_str(const char *s, char *out, unsigned cap)
{
    unsigned w = 0;
    if (!out || cap == 0) return;
    out[0] = '"';
    w = 1;
    for (const char *p = s ? s : ""; *p && w + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[w++] = '\\'; out[w++] = (char)c; }
        else if (c == '\n') { out[w++] = '\\'; out[w++] = 'n'; }
        else if (c < 0x20) { /* skip control characters */ }
        else out[w++] = (char)c;
    }
    out[w++] = '"';
    out[w] = '\0';
}

static void render_json(const Snapshot *s, char *buf, unsigned cap)
{
    unsigned w = 0;
    char tmp[512];
    w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                            "{\"schema\":%d,\"gpu\":{", SNAPSHOT_SCHEMA);
    if (!s->have_gpu) {
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                "\"error\":");
        json_str(s->gpu_error ? s->gpu_error : "none", tmp, sizeof(tmp));
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "%s", tmp);
    } else {
        const GpuMetrics *m = &s->gpu;
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                "\"name\":");
        json_str(m->name, tmp, sizeof(tmp));
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "%s", tmp);
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                ",\"busy_pct\":%u,\"mem_total_mb\":%llu,"
                                "\"mem_used_mb\":%llu,\"temp_c\":%u,"
                                "\"power_mw\":%u,\"power_limit_mw\":%u,"
                                "\"clock_sm_mhz\":%u,\"clock_mem_mhz\":%u,"
                                "\"pcie_rx_kbps\":%u,\"pcie_tx_kbps\":%u,"
                                "\"enc_pct\":%u,\"dec_pct\":%u,"
                                "\"has_per_process\":%s,",
                                m->busy_pct, m->mem_total_mb, m->mem_used_mb,
                                m->temp_c, m->power_mw, m->power_limit_mw,
                                m->clock_sm_mhz, m->clock_mem_mhz,
                                m->pcie_rx_kbps, m->pcie_tx_kbps,
                                m->enc_pct, m->dec_pct,
                                m->has_per_process ? "true" : "false");
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                "\"headline\":");
        json_str(s->story.headline, tmp, sizeof(tmp));
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "%s", tmp);
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                ",\"findings\":[");
        for (int i = 0; i < s->story.finding_count; i++) {
            if (i) w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, ",");
            json_str(s->story.findings[i], tmp, sizeof(tmp));
            w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "%s", tmp);
        }
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "],\"apps\":[");
        for (int i = 0; i < s->napps; i++) {
            const AppUse *a = &s->apps[i];
            if (i) w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, ",");
            json_str(a->name, tmp, sizeof(tmp));
            w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                    "{\"name\":%s,\"kind\":", tmp);
            json_str(a->kind, tmp, sizeof(tmp));
            w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                    "%s,\"pid\":%u,\"gpu_pct\":%.1f,"
                                    "\"mem_mb\":%.1f,\"has_pct\":%s}",
                                    tmp, a->pid, a->gpu_pct, a->mem_mb,
                                    a->has_pct ? "true" : "false");
        }
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "]");
    }
    w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "}");
    if (s->have_perf) {
        const PerfSample *p = &s->perf;
        w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0,
                                ",\"perf\":{\"cpu_pct\":%.1f,\"mem_total_mb\":%llu,"
                                "\"mem_used_mb\":%llu,\"swap_used_mb\":%llu,"
                                "\"disk_read_mbps\":%.2f,\"disk_write_mbps\":%.2f,"
                                "\"net_rx_mbps\":%.2f,\"net_tx_mbps\":%.2f,"
                                "\"load\":[%.2f,%.2f,%.2f],\"uptime_secs\":%.0f}",
                                p->cpu_total_pct, p->mem_total_mb, p->mem_used_mb,
                                p->swap_used_mb, p->disk_read_mbps, p->disk_write_mbps,
                                p->net_rx_mbps, p->net_tx_mbps,
                                p->load_avg[0], p->load_avg[1], p->load_avg[2],
                                p->uptime_secs);
    }
    w += (unsigned)snprintf(buf + w, cap > w ? cap - w : 0, "}\n");
}

void snapshot_render(const Snapshot *s, char *buf, unsigned cap, int json)
{
    if (!buf || cap == 0) return;
    memset(buf, 0, cap);
    if (json) render_json(s, buf, cap);
    else      render_text(s, buf, cap);
}
