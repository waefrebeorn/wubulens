/*
 * wubulens -- the WuBu lens: a plain-language GPU + task manager, in C11.
 *
 * Every number comes from the machine: the card through NVML (loaded at run
 * time, so no development package is needed to build), the system through
 * /proc, the programs through their own /proc entries. Nothing is estimated.
 *
 *   wubulens               the interface (a plain report when piped)
 *   wubulens snapshot      the report
 *   wubulens --json        the machine-readable form
 *   wubulens --demo        a scripted card (no hardware needed)
 *   wubulens --help
 *
 * Layout: one concern per file, opaque structs, minimal includes, C11.
 */

#include "gpu.h"
#include "perf.h"
#include "proc.h"
#include "narrate.h"
#include "snapshot.h"
#include "tui.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *a0)
{
    printf("wubulens -- what is using my GPU right now, and why?\n\n"
           "  %s                the interface (a plain report when piped)\n"
           "  %s snapshot       the report\n"
           "  %s snapshot --json\n"
           "  %s --demo         a scripted card (no hardware needed)\n"
           "  %s --help\n",
           a0, a0, a0, a0, a0);
}

int main(int argc, char **argv)
{
    int json = 0, demo = 0, want_report = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "snapshot") == 0) {
            want_report = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else if (strcmp(argv[i], "--demo") == 0) {
            demo = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "wubulens: unknown argument '%s' (--help)\n", argv[i]);
            return 1;
        }
    }

    wubu_perf_init();

    /* The interface only makes sense on a terminal; piped output is the
     * report, which is what scripts and agents want. */
    if (!want_report && isatty(STDOUT_FILENO) && isatty(STDIN_FILENO))
        return wubulens_tui(demo);

    struct GpuBackend *b = demo ? gpu_open_fake() : gpu_open_nvml();
    if (!b) {
        fprintf(stderr, "wubulens: %s\n"
                        "  (try --demo to see the picture without a GPU)\n",
                demo ? "the demo backend failed to open" : gpu_last_open_error());
        return 1;
    }

    Snapshot s;
    snapshot_take(&s, b);
    gpu_close(b);

    char buf[8192];
    snapshot_render(&s, buf, sizeof(buf), json);
    fputs(buf, stdout);
    return s.have_gpu ? 0 : 1;
}
