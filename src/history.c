/*
 * history.c -- the cumulative ledger. See history.h.
 *
 * C11, minimal includes, self-contained.
 */
#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static const char *history_dir(void)
{
    static char dir[512];
    if (dir[0]) return dir;
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(dir, sizeof(dir), "%s/wubulens", xdg);
    else if (home && home[0]) snprintf(dir, sizeof(dir), "%s/.local/share/wubulens", home);
    else snprintf(dir, sizeof(dir), "/tmp/wubulens");
    return dir;
}

const char *history_path(void)
{
    static char path[600];
    if (!path[0]) snprintf(path, sizeof(path), "%s/history.log", history_dir());
    return path;
}

void history_record(const char *name, double gpu_pct, double mem_mb,
                    double sample_secs)
{
    if (!name || name[0] == '\0') return;
    const char *dir = history_dir();
    mkdir(dir, 0755);

    FILE *f = fopen(history_path(), "a");
    if (!f) return;
    /* one line: unixtime name gpu_pct mem_mb sample_secs */
    fprintf(f, "%ld %s %.2f %.1f %.2f\n",
            (long)time(NULL), name, gpu_pct, mem_mb, sample_secs);
    fclose(f);
}

static int cmp_rows(const void *a, const void *b)
{
    const HistoryRow *x = (const HistoryRow *)a;
    const HistoryRow *y = (const HistoryRow *)b;
    if (x->gpu_seconds > y->gpu_seconds) return -1;
    if (x->gpu_seconds < y->gpu_seconds) return 1;
    return 0;
}

int history_load(HistoryRow *out, int cap)
{
    if (!out || cap <= 0) return 0;
    FILE *f = fopen(history_path(), "r");
    if (!f) return 0;

    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        long t;
        char name[64];
        double gpu_pct, mem_mb, secs;
        if (sscanf(line, "%ld %63s %lf %lf %lf", &t, name, &gpu_pct, &mem_mb, &secs) != 5)
            continue;                       /* skip a corrupt line, never guess */

        HistoryRow *row = NULL;
        for (int i = 0; i < n; i++) {
            if (strcmp(out[i].name, name) == 0) { row = &out[i]; break; }
        }
        if (!row && n < cap) {
            row = &out[n++];
            memset(row, 0, sizeof(*row));
            snprintf(row->name, sizeof(row->name), "%.*s", 63, name);
        }
        if (!row) continue;
        row->gpu_seconds += gpu_pct * secs;      /* percent-seconds */
        if (mem_mb > row->peak_mem_mb) row->peak_mem_mb = mem_mb;
        row->samples++;
    }
    fclose(f);

    if (n > 1) qsort(out, (size_t)n, sizeof(HistoryRow), cmp_rows);
    return n;
}
