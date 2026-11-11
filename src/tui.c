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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>

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

#define TABS 5
static const char *k_tab_names[TABS] = {
    "Processes", "Performance", "GPU", "Startup", "Services"
};

/* -- terminal ------------------------------------------------------------ */

static struct termios g_saved;
static int g_raw = 0;

static void raw_on(void)
{
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return;
    struct termios t = g_saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_raw = 1;
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
    char head[128];
    snprintf(head, sizeof(head), "  %-22s %6s  %10s  %s",
             "PROGRAM", "GPU%", "MEMORY", "KIND");
    text(0, y++, head, C_LABEL);

    int visible = y + 12;
    (void)visible;
    for (int i = 0; i < s->napps; i++) {
        const AppUse *a = &s->apps[i];
        char pct[16];
        if (a->has_pct) snprintf(pct, sizeof(pct), "%.0f", a->gpu_pct);
        else            snprintf(pct, sizeof(pct), "-");
        char line[256];
        snprintf(line, sizeof(line), "  %-22s %6s  %8.0f MB  %s",
                 a->name, pct, a->mem_mb, a->kind);
        text(0, y, line, i == sel ? C_REV : "");
        y++;
    }
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

static void draw_footer(int tab, int h)
{
    char line[256];
    snprintf(line, sizeof(line),
             " %sTab%s next   %s?%s keys   %sq%s quit",
             C_KEY, C_RESET, C_KEY, C_RESET, C_KEY, C_RESET);
    text(0, h - 1, line, C_DIM);
    char pos[64];
    snprintf(pos, sizeof(pos), " %s ", k_tab_names[tab]);
    text(0, h - 2, "", "");
    (void)pos;
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
    Snapshot s;
    memset(&s, 0, sizeof(s));

    for (;;) {
        snapshot_take(&s, b);

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
        }
        draw_footer(tab, h);
        fflush(stdout);

        /* wait for a key or about a second, whichever comes first */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {1, 0};
        int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0) continue;

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
            case 'e': case 'E':
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
}
