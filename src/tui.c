/*
 * tui.c -- the interface. See tui.h.
 *
 * Self-contained terminal renderer: raw mode via termios, the alternate
 * screen, ANSI colour chosen from the terminal's own capabilities. No
 * external UI library -- the drawing is ours.
 *
 * C11, minimal includes, self-contained.
 */
#include "tui.h"
#include "gpu.h"
#include "perf.h"
#include "proc.h"
#include "narrate.h"
#include "snapshot.h"
#include "session.h"
#include "ops.h"
#include "classify.h"
#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

/* -- colour -------------------------------------------------------------- */

#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_DIM     "\x1b[2m"
#define C_REV     "\x1b[7m"
#define C_HEAD    "\x1b[38;5;208m"   /* the story line */
#define C_TAB     "\x1b[38;5;250m"
#define C_TAB_HI  "\x1b[1;38;5;214m"
#define C_LABEL   "\x1b[38;5;244m"
#define C_BAR     "\x1b[38;5;39m"
#define C_BAR_HI  "\x1b[38;5;208m"
#define C_GOOD    "\x1b[38;5;41m"
#define C_WARN    "\x1b[38;5;214m"
#define C_BAD     "\x1b[38;5;203m"
#define C_KEY     "\x1b[38;5;117m"

#define TABS 6
static const char *k_tab_names[TABS] = {
    "Processes", "Performance", "GPU", "Startup", "Services", "History"
};

/* -- terminal ------------------------------------------------------------ */

static struct termios g_saved;
static int g_raw = 0;

/* The terminal must be restored even when the process is signalled, or the
 * user's shell is left in raw mode. ISIG stays enabled so Ctrl-C arrives as
 * SIGINT (never as a 0x03 byte), and the handler only sets a flag -- the loop
 * notices it, restores, and exits cleanly. */
static volatile sig_atomic_t g_interrupted = 0;
static void on_signal(int sig) { (void)sig; g_interrupted = 1; }

static void raw_on(void)
{
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return;
    struct termios t = g_saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_raw = 1;
    g_interrupted = 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("\x1b[?1049h");          /* alternate screen */
    printf("\x1b[?25l");            /* hide the cursor */
    fflush(stdout);
}

static void raw_off(void)
{
    if (!g_raw) return;
    printf("\x1b[?25h");
    printf("\x1b[?1049l");          /* leave the alternate screen */
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
    g_raw = 0;
}

static void term_size(int *w, int *h)
{
    struct winsize ws;
    *w = 80; *h = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    }
}

static void clear(void) { printf("\x1b[2J\x1b[H"); }

/* -- drawing helpers ----------------------------------------------------- */

static void text(int x, int y, const char *s, const char *color)
{
    if (x < 0 || y < 0) return;
    printf("\x1b[%d;%dH%s%s%s", y + 1, x + 1, color, s, C_RESET);
}

/* A horizontal bar into a fixed-width cell. */
static void bar(int x, int y, int width, double pct, const char *color)
{
    if (width < 0) width = 0;
    char cell[256];
    int filled = (int)(pct / 100.0 * (double)width);
    if (filled > width) filled = width;
    int n = 0;
    n += snprintf(cell + n, sizeof(cell) - (unsigned)n, "%s", color);
    for (int i = 0; i < width && n < (int)sizeof(cell) - 8; i++)
        cell[n++] = i < filled ? '=' : '.';
    n += snprintf(cell + n, sizeof(cell) - (unsigned)n, "%s", C_RESET);
    cell[n] = '\0';
    text(x, y, cell, "");
}

/* Wrap a sentence into lines of at most `width`. Returns the line count. */
static int wrap(const char *s, int width, char lines[][256], int cap)
{
    int n = 0;
    if (width < 8) width = 8;
    const char *p = s;
    while (*p && n < cap) {
        int len = (int)strlen(p);
        if (len <= width) {
            snprintf(lines[n++], 256, "%s", p);
            break;
        }
        int cut = width;
        while (cut > 0 && p[cut] != ' ') cut--;
        if (cut == 0) cut = width;
        snprintf(lines[n++], 256, "%.*s", cut, p);
        p += cut;
        while (*p == ' ') p++;
    }
    return n;
}

/* -- sorting and filtering ------------------------------------------------ */

typedef enum { SORT_GPU = 0, SORT_MEM, SORT_NAME, SORT_KIND, SORT_COUNT } SortMode;

static const char *sort_name(SortMode m)
{
    switch (m) {
        case SORT_GPU:  return "GPU time";
        case SORT_MEM:  return "memory";
        case SORT_NAME: return "name";
        case SORT_KIND: return "kind";
        default:        return "GPU time";
    }
}

static SortMode g_sort = SORT_GPU;
static char g_filter[64];
static int  g_filtering;

/* a small case-insensitive substring search (no locale dependency) */
static int strcasestr_local(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               ((p[i] >= 'A' && p[i] <= 'Z') ? (char)(p[i] + 32) : p[i]) ==
               ((needle[i] >= 'A' && needle[i] <= 'Z') ? (char)(needle[i] + 32) : needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

static int match_filter(const AppUse *a)
{
    if (g_filter[0] == '\0') return 1;
    return strcasestr_local(a->name, g_filter) || strcasestr_local(a->kind, g_filter);
}

static int cmp_apps(const void *a, const void *b)
{
    const AppUse *x = (const AppUse *)a;
    const AppUse *y = (const AppUse *)b;
    int r = 0;
    switch (g_sort) {
        case SORT_MEM:  r = (x->mem_mb > y->mem_mb) ? -1 : (x->mem_mb < y->mem_mb) ? 1 : 0; break;
        case SORT_NAME: r = strcmp(x->name, y->name); break;
        case SORT_KIND: r = strcmp(x->kind, y->kind); break;
        default:        r = (x->gpu_pct > y->gpu_pct) ? -1 : (x->gpu_pct < y->gpu_pct) ? 1 : 0; break;
    }
    return r;
}

/* -- views --------------------------------------------------------------- */

static void draw_header(const Snapshot *s, int w, int tab)
{
    char line[512];
    snprintf(line, sizeof(line), " wubulens ");
    text(0, 0, line, C_BOLD);

    if (!s->have_gpu) {
        text(12, 0, "no GPU readable", C_BAD);
        return;
    }
    const GpuMetrics *m = &s->gpu;
    snprintf(line, sizeof(line),
             "%s   %u%% busy   %.1f/%.1f GB   %u C   %u W",
             m->name, m->busy_pct,
             (double)m->mem_used_mb / 1024.0, (double)m->mem_total_mb / 1024.0,
             m->temp_c, m->power_mw / 1000);
    text(12, 0, line, C_LABEL);
    (void)w;

    /* tabs */
    int x = 0;
    for (int i = 0; i < TABS; i++) {
        char t[32];
        snprintf(t, sizeof(t), " %s ", k_tab_names[i]);
        text(x, 1, t, i == tab ? C_TAB_HI : C_TAB);
        x += (int)strlen(t);
    }
    text(0, 2, "", C_RESET);
}

static void draw_processes(const Snapshot *s, int row, int w, int sel)
{
    int y = row;
    (void)w;

    if (s->napps == 0) {
        text(0, y++, "  Nothing is using the GPU right now.", C_LABEL);
        return;
    }

    /* filter, then sort */
    AppUse list[64];
    int n = 0;
    for (int i = 0; i < s->napps && n < 64; i++)
        if (match_filter(&s->apps[i])) list[n++] = s->apps[i];
    if (n > 1) qsort(list, (size_t)n, sizeof(AppUse), cmp_apps);

    if (n == 0) {
        text(0, y++, "  Nothing matches that filter.", C_LABEL);
        return;
    }

    char head[200];
    snprintf(head, sizeof(head), "  %-20s %6s %6s %6s  %9s  %-8s %s",
             "PROGRAM", "3D", "ENC", "DEC", "MEMORY", "KIND", "sorted by");
    text(0, y++, head, C_LABEL);
    char sm[64];
    snprintf(sm, sizeof(sm), "%s", sort_name(g_sort));
    text((int)strlen(head) + 1, y - 1, sm, C_DIM);

    if (sel >= n) sel = n - 1;
    for (int i = 0; i < n; i++) {
        const AppUse *a = &list[i];
        char pct[16];
        if (a->has_pct) snprintf(pct, sizeof(pct), "%.0f", a->gpu_pct);
        else            snprintf(pct, sizeof(pct), "-");
        char line[256];
        snprintf(line, sizeof(line), "  %-20s %6s %5.0f %5.0f  %7.0f MB  %s",
                 a->name, pct, a->enc_pct, a->dec_pct, a->mem_mb, a->kind);
        text(0, y, line, i == sel ? C_REV : "");
        y++;
    }

    /* the detail pane: what the selected program is */
    const AppUse *a = &list[sel];
    y++;
    text(0, y++, "  ---", C_LABEL);
    char d1[300];
    snprintf(d1, sizeof(d1), "  %s   (%s)", a->name, a->kind);
    text(0, y++, d1, C_BOLD);
    const char *desc = classify_description_by_name(a->kind);
    if (desc && desc[0]) {
        char wline[8][256];
        char pref[300];
        snprintf(pref, sizeof(pref), "  %s", desc);
        int nw = wrap(pref, 90, wline, 8);
        for (int i = 0; i < nw; i++) text(0, y++, wline[i], C_LABEL);
    }
    /* pids, one per line (up to four) */
    for (int i = 0; i < a->npids && i < 4; i++) {
        char pl[128];
        snprintf(pl, sizeof(pl), "  pid %u", a->pids[i]);
        text(0, y++, pl, C_LABEL);
    }
    char d3[300];
    snprintf(d3, sizeof(d3), "  gpu %.0f%%   enc %.0f%%   dec %.0f%%   memory %.0f MB   %d process%s",
             a->gpu_pct, a->enc_pct, a->dec_pct, a->mem_mb,
             a->npids, a->npids == 1 ? "" : "es");
    text(0, y++, d3, "");
}

static void draw_performance(const Snapshot *s, int row, int w)
{
    const PerfSample *p = &s->perf;
    int y = row;

    char line[256];
    snprintf(line, sizeof(line), "  CPU");
    text(0, y, line, C_LABEL);
    bar(10, y, (w > 20 ? w - 30 : 20), p->cpu_total_pct,
        p->cpu_total_pct >= 85 ? C_BAD : p->cpu_total_pct >= 50 ? C_WARN : C_BAR);
    snprintf(line, sizeof(line), " %.0f%%", p->cpu_total_pct);
    text((w > 20 ? w - 18 : 32), y, line, C_LABEL);
    y += 2;

    snprintf(line, sizeof(line), "  MEM");
    text(0, y, line, C_LABEL);
    double mem_pct = 0.0;
    if (p->mem_total_mb > 0)
        mem_pct = (double)p->mem_used_mb * 100.0 / (double)p->mem_total_mb;
    bar(10, y, (w > 20 ? w - 30 : 20), mem_pct,
        mem_pct >= 90 ? C_BAD : mem_pct >= 70 ? C_WARN : C_BAR);
    snprintf(line, sizeof(line), " %llu/%llu MB",
             p->mem_used_mb, p->mem_total_mb);
    text((w > 20 ? w - 18 : 32), y, line, C_LABEL);
    y += 2;

    snprintf(line, sizeof(line), "  DISK   read %.1f MB/s   write %.1f MB/s",
             p->disk_read_mbps, p->disk_write_mbps);
    text(0, y++, line, C_LABEL);
    snprintf(line, sizeof(line), "  NET    rx %.2f MB/s   tx %.2f MB/s",
             p->net_rx_mbps, p->net_tx_mbps);
    text(0, y++, line, C_LABEL);
    snprintf(line, sizeof(line), "  LOAD   %.2f %.2f %.2f",
             p->load_avg[0], p->load_avg[1], p->load_avg[2]);
    text(0, y++, line, C_LABEL);
    char up[64];
    wubu_perf_uptime_str(p, up, sizeof(up));
    snprintf(line, sizeof(line), "  UP     %s", up);
    text(0, y++, line, C_LABEL);
}

static void draw_gpu(const Snapshot *s, int row, int w)
{
    const GpuMetrics *m = &s->gpu;
    int y = row;
    char line[256];

    snprintf(line, sizeof(line), "  %-22s %s", m->name,
             m->has_per_process ? "" : "(no per-process reporting)");
    text(0, y++, line, C_LABEL);

    snprintf(line, sizeof(line),
             "  busy %u%%   memory %.1f/%.1f GB   temp %u C   power %u W",
             m->busy_pct, (double)m->mem_used_mb / 1024.0,
             (double)m->mem_total_mb / 1024.0, m->temp_c, m->power_mw / 1000);
    text(0, y++, line, "");
    if (m->power_limit_mw > 0) {
        snprintf(line, sizeof(line), "  power limit %u W   SM %u MHz   MEM %u MHz",
                 m->power_limit_mw / 1000, m->clock_sm_mhz, m->clock_mem_mhz);
        text(0, y++, line, "");
    }
    snprintf(line, sizeof(line),
             "  pcie rx %u KB/s   tx %u KB/s   enc %u%%   dec %u%%",
             m->pcie_rx_kbps, m->pcie_tx_kbps, m->enc_pct, m->dec_pct);
    text(0, y++, line, "");
    (void)w;

    y++;
    text(0, y++, "  The story", C_LABEL);
    char wrapped[8][256];
    int n = wrap(s->story.headline, (w > 4 ? w - 4 : 60), wrapped, 8);
    for (int i = 0; i < n; i++) text(2, y++, wrapped[i], C_HEAD);
    for (int i = 0; i < s->story.finding_count; i++) {
        char w2[8][256];
        char pref[256];
        snprintf(pref, sizeof(pref), "- %s", s->story.findings[i]);
        int n2 = wrap(pref, (w > 4 ? w - 4 : 60), w2, 8);
        for (int j = 0; j < n2; j++) text(2, y++, w2[j], C_WARN);
    }
}

static void draw_startup(int row, int sel)
{
    int y = row;
    int n = autostart_count();
    text(0, y++, "  Programs that start with the session (XDG autostart).", C_LABEL);
    if (n == 0) {
        text(0, y++, "", "");
        text(0, y++, "  No autostart entries on this machine.", C_LABEL);
        return;
    }
    char head[128];
    snprintf(head, sizeof(head), "  %-28s %-8s %s", "NAME", "STARTS", "EXEC");
    text(0, y++, head, C_LABEL);
    for (int i = 0; i < n; i++) {
        const AutostartEntry *e = autostart_entry(i);
        char line[512];
        snprintf(line, sizeof(line), "  %-28s %-8s %.*s",
                 e->name, e->enabled ? "yes" : "no",
                 (int)sizeof(line) - 60, e->exec);
        text(0, y, line, i == sel ? C_REV : "");
        y++;
    }
    char hint[256];
    snprintf(hint, sizeof(hint), "  %se%s toggles the selected entry",
             C_KEY, C_RESET);
    text(0, y, hint, C_DIM);
}

static void draw_services(int row, int sel)
{
    int y = row;
    int n = service_count();
    text(0, y++, "  Background services (systemd units on disk).", C_LABEL);
    if (n == 0) {
        text(0, y++, "", "");
        text(0, y++, "  No service units found.", C_LABEL);
        return;
    }
    char head[160];
    snprintf(head, sizeof(head), "  %-32s %-9s %s", "UNIT", "STATE", "DESCRIPTION");
    text(0, y++, head, C_LABEL);
    for (int i = 0; i < n; i++) {
        const ServiceEntry *e = service_entry(i);
        char line[512];
        snprintf(line, sizeof(line), "  %-32s %-9s %.*s",
                 e->name, e->state,
                 (int)sizeof(line) - 64, e->description);
        text(0, y, line, i == sel ? C_REV : "");
        y++;
    }
}

static void draw_history(int row)
{
    HistoryRow rows[HISTORY_ROWS];
    int n = history_load(rows, HISTORY_ROWS);
    int y = row;
    text(0, y++, "  What has been using the machine, accumulated over time.", C_LABEL);
    if (n == 0) {
        text(0, y++, "", "");
        text(0, y++, "  No history yet. It accumulates while wubulens runs.", C_LABEL);
        char p[600];
        snprintf(p, sizeof(p), "  Log: %s", history_path());
        text(0, y++, p, C_DIM);
        return;
    }
    char head[160];
    snprintf(head, sizeof(head), "  %-24s %10s %12s %8s",
             "PROGRAM", "GPU-HOURS", "PEAK MEM", "SAMPLES");
    text(0, y++, head, C_LABEL);
    for (int i = 0; i < n; i++) {
        char line[256];
        snprintf(line, sizeof(line), "  %-24.*s %10.2f %10.0f MB %8ld",
                 (int)sizeof(line) - 40, rows[i].name,
                 rows[i].gpu_seconds / 3600.0,
                 rows[i].peak_mem_mb, rows[i].samples);
        text(0, y++, line, "");
    }
    char p[600];
    snprintf(p, sizeof(p), "  Log: %s", history_path());
    text(0, y++, p, C_DIM);
}

static void draw_footer(int tab, int h, int confirm, char pending,
                        int last_rc, const AppUse *sel_app)
{
    char line[512];
    if (confirm && sel_app) {
        snprintf(line, sizeof(line),
                 " %s%s%s %s -- %sy%s to confirm, anything else cancels",
                 C_BAD, op_name((const char[]){pending, 0}), C_RESET,
                 sel_app->name, C_KEY, C_RESET);
    } else {
        snprintf(line, sizeof(line),
                 " %sTab%s next  %ss%s sort(%s)  %s/%s filter%-12s "
                 "%sx%s end  %sp%s/%sr%s stop/go  %sn%s nice  %sE%s efficiency  %sq%s quit",
                 C_KEY, C_RESET, C_KEY, C_RESET, sort_name(g_sort),
                 C_KEY, C_RESET, g_filtering ? "(typing)" : g_filter,
                 C_KEY, C_RESET, C_KEY, C_RESET, C_KEY, C_RESET,
                 C_KEY, C_RESET, C_KEY, C_RESET, C_KEY, C_RESET);
    }
    text(0, h - 1, line, confirm ? C_BAD : C_DIM);
    if (last_rc != 0) {
        snprintf(line, sizeof(line), " %s", op_error(last_rc));
        text(0, h - 2, line, last_rc == OP_OK ? C_GOOD : C_BAD);
    }
    (void)tab;
}

/* -- main loop ----------------------------------------------------------- */

int wubulens_tui(int demo)
{
    struct GpuBackend *b = demo ? gpu_open_fake() : gpu_open_nvml();
    if (!b) {
        fprintf(stderr, "wubulens: %s\n", gpu_last_open_error());
        return 1;
    }
    wubu_perf_init();

    raw_on();

    int tab = 0;
    int sel = 0;
    int confirm = 0;          /* waiting for y/n on an operation */
    char pending = 0;         /* which operation is pending */
    int last_rc = 0;          /* OpResult of the last operation */
    Snapshot s;
    memset(&s, 0, sizeof(s));

    for (;;) {
        snapshot_take(&s, b);
        /* the ledger: one line per app per sample, always */
        for (int i = 0; i < s.napps; i++)
            history_record(s.apps[i].name, s.apps[i].gpu_pct, s.apps[i].mem_mb, 1.0);

        int w, h;
        term_size(&w, &h);
        clear();
        draw_header(&s, w, tab);
        int row = 3;
        switch (tab) {
            case 0: draw_processes(&s, row, w, sel); break;
            case 1: draw_performance(&s, row, w); break;
            case 2: draw_gpu(&s, row, w); break;
            case 3: draw_startup(row, sel); break;
            case 4: draw_services(row, sel); break;
            case 5: draw_history(row); break;
        }
        draw_footer(tab, h, confirm, pending, last_rc,
                   (tab == 0 && sel < s.napps) ? &s.apps[sel] : NULL);
        fflush(stdout);

        /* wait for a key or about a second, whichever comes first */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {1, 0};
        int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (ready < 0 && errno == EINTR) {
            if (g_interrupted) break;
            continue;
        }
        if (ready <= 0) {
            if (g_interrupted) break;
            continue;
        }

        char key[8];
        ssize_t n = read(STDIN_FILENO, key, sizeof(key));
        if (n <= 0) continue;

        /* escape sequences: arrows and PgUp/PgDn come as ESC [ X */
        if (key[0] == 0x1b && n >= 3) {
            if (key[2] == 'C' || key[2] == 'A') {           /* right / up */
                if (key[2] == 'C') tab = (tab + 1) % TABS;
                else if (sel > 0) sel--;
            } else if (key[2] == 'D' || key[2] == 'B') {    /* left / down */
                if (key[2] == 'D') tab = (tab + TABS - 1) % TABS;
                else sel++;
            }
            continue;
        }

        if (g_filtering) {
            char k = (char)key[0];
            if (k == '\r' || k == '\n' || k == 0x1b) {
                g_filtering = 0;                 /* Enter keeps, Esc clears */
                if (k == 0x1b) g_filter[0] = '\0';
            } else if (k == 0x7f || k == 0x08) {
                size_t n = strlen(g_filter);
                if (n) g_filter[n - 1] = '\0';
            } else if (k >= 32 && k < 127) {
                size_t n = strlen(g_filter);
                if (n + 1 < sizeof(g_filter)) { g_filter[n] = k; g_filter[n + 1] = '\0'; }
            }
            continue;
        }

        if (confirm) {
            char k = (char)key[0];
            confirm = 0;
            if (k == 'y' || k == 'Y') {
                const AppUse *a = (tab == 0 && sel < s.napps) ? &s.apps[sel] : NULL;
                if (a) {
                    if (pending == 'x') {
                        for (int i = 0; i < a->npids; i++) {
                            last_rc = op_end_task(a->pids[i]);
                            if (last_rc != OP_OK) break;
                        }
                    } else if (pending == 'p') {
                        for (int i = 0; i < a->npids; i++) last_rc = op_suspend(a->pids[i]);
                    } else if (pending == 'r') {
                        for (int i = 0; i < a->npids; i++) last_rc = op_resume(a->pids[i]);
                    } else if (pending == 'n') {
                        for (int i = 0; i < a->npids; i++) last_rc = op_set_priority(a->pids[i], 10);
                    } else if (pending == 'E') {
                        for (int i = 0; i < a->npids; i++) last_rc = op_efficiency(a->pids[i]);
                    } else if (pending == 'F') {
                        for (int i = 0; i < a->npids; i++) last_rc = op_normal(a->pids[i]);
                    }
                }
            }
            continue;
        }

        switch (key[0]) {
            case 'q': case 'Q': case 0x03:
                raw_off();
                gpu_close(b);
                return 0;
            case '\t': case 'l': case 'L':
                tab = (tab + 1) % TABS; break;
            case 'h': case 'H':
                tab = (tab + TABS - 1) % TABS; break;
            case 'j': case 'J': sel++; break;
            case 'k': case 'K': if (sel > 0) sel--; break;
            case 's': case 'S':
                g_sort = (SortMode)((g_sort + 1) % SORT_COUNT);
                break;
            case '/':
                g_filtering = 1;
                break;
            case 'x': case 'X':
                if (tab == 0 && sel < s.napps) { pending = 'x'; confirm = 1; }
                break;
            case 'p': case 'P':
                if (tab == 0 && sel < s.napps) { pending = 'p'; confirm = 1; }
                break;
            case 'r': case 'R':
                if (tab == 0 && sel < s.napps) { pending = 'r'; confirm = 1; }
                break;
            case 'n': case 'N':
                if (tab == 0 && sel < s.napps) { pending = 'n'; confirm = 1; }
                break;
            case 'E':
                if (tab == 0 && sel < s.napps) { pending = 'E'; confirm = 1; }
                break;
            case 'F':
                if (tab == 0 && sel < s.napps) { pending = 'F'; confirm = 1; }
                break;
            case 'e':
                if (tab == 3) {
                    const AutostartEntry *e = autostart_entry(sel);
                    if (e) autostart_set_enabled(sel, !e->enabled);
                }
                break;
            case '?':
                /* the key list is the footer's second line */
                break;
            default: break;
        }
    }

    /* signalled out of the loop: restore before leaving */
    raw_off();
    gpu_close(b);
    return 130;
}
