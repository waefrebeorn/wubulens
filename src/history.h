/*
 * history.h -- cumulative per-app resource history.
 *
 * Every sample appends one line per app to a log under
 * ~/.local/share/wubulens/. The History view aggregates it, so it answers
 * "what has been using my GPU" rather than only "what is using it now" --
 * the difference between a snapshot and a task manager.
 *
 * The log is append-only and text, so it can be read with any tool. A line
 * that cannot be parsed is skipped, never guessed at.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_HISTORY_H
#define WUBULENS_HISTORY_H

#define HISTORY_ROWS 64

typedef struct {
    char     name[64];
    double   gpu_seconds;    /* accumulated GPU-percent seconds */
    double   peak_mem_mb;
    long     samples;
} HistoryRow;

/* Append one sample for one app. name is the app's group key. */
void history_record(const char *name, double gpu_pct, double mem_mb,
                    double sample_secs);

/* Aggregate the log into rows, busiest first. Returns the row count. */
int history_load(HistoryRow *out, int cap);

/* The log file's path (for the footer / debugging). */
const char *history_path(void);

#endif /* WUBULENS_HISTORY_H */
