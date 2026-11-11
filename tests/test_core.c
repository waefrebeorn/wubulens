/*
 * test_core.c -- prove the data layer against the REAL machine.
 *
 * No mocks: this runs on the box, reads the real /proc and the real card
 * through NVML, and asserts the numbers are plausible. A GPU-less machine is
 * still a valid run: the NVML half then asserts the backend reports why.
 *
 * C11, self-contained.
 */
#include "perf.h"
#include "proc.h"
#include "gpu.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;
static int checks;
#define CHECK(c, m) do { \
    checks++; \
    if (!(c)) { printf("  FAIL: %s\n", m); failures++; } \
} while (0)

int main(void)
{
    printf("\n=== wubulens core ===\n");

    /* -- /proc resources -- */
    wubu_perf_init();
    PerfSample s1;
    wubu_perf_sample(&s1);
    sleep(1);
    PerfSample s;
    wubu_perf_sample(&s);

    CHECK(wubu_perf_available(), "/proc was readable");
    CHECK(s.cpu_cores > 0, "core count is positive");
    CHECK(s.cpu_total_pct >= 0.0 && s.cpu_total_pct <= 100.0,
          "total CPU% is in range");
    CHECK(s.mem_total_mb > 0, "memory total is positive");
    CHECK(s.mem_avail_mb <= s.mem_total_mb, "available <= total memory");
    CHECK(s.load_avg[0] >= 0.0, "load average is readable");
    CHECK(s.uptime_secs > 0.0, "uptime is positive");
    CHECK(s.disk_read_total_mb > 0, "disk reads are countable");
    CHECK(s.net_rx_total_mb > 0, "network receives are countable");

    char up[64];
    wubu_perf_uptime_str(&s, up, sizeof(up));
    CHECK(up[0] != '\0', "uptime renders as text");

    /* -- /proc processes -- */
    CHECK(proc_count() > 10, "the process list is populated");

    ProcInfo self;
    CHECK(proc_lookup((unsigned)getpid(), &self), "this process resolves");
    CHECK(self.exists, "this process exists");
    CHECK(self.exe[0] != '\0', "this process has an executable name");
    CHECK(self.cmdline[0] != '\0', "this process has a command line");
    CHECK(self.user[0] != '\0' && strcmp(self.user, "?") != 0,
          "this process has an owner");
    CHECK(self.rss_mb > 0.0, "this process has resident memory");

    int n = proc_list(NULL, 0);
    CHECK(n == 0, "proc_list(NULL,0) is safe and returns 0");
    ProcInfo list[64];
    int got = proc_list(list, 64);
    CHECK(got > 0, "proc_list fills the array");
    int sorted = 1;
    for (int i = 1; i < got; i++)
        if (list[i].pid < list[i - 1].pid) sorted = 0;
    CHECK(sorted, "proc_list is ordered by pid");
    CHECK(proc_user((unsigned)getpid())[0] != '\0', "proc_user resolves");

    /* -- GPU -- */
    struct GpuBackend *b = gpu_open_nvml();
    if (!b) {
        printf("  (NVML unavailable: %s)\n", gpu_last_open_error());
        CHECK(gpu_last_open_error()[0] != '\0',
              "a missing GPU is explained, not silent");
    } else {
        CHECK(gpu_kind(b) == GPU_KIND_NVML, "the NVML backend is live");
        CHECK(gpu_count(b) >= 1, "at least one GPU is present");
        CHECK(gpu_sample(b), "sampling succeeds");

        const GpuMetrics *m = gpu_metrics(b, 0);
        CHECK(m != NULL, "GPU metrics are available");
        if (m) {
            CHECK(m->name[0] != '\0', "the GPU has a name");
            CHECK(m->busy_pct <= 100, "busy% is in range");
            CHECK(m->mem_total_mb > 0, "VRAM total is positive");
            CHECK(m->mem_used_mb <= m->mem_total_mb, "VRAM used <= total");
            CHECK(m->temp_c > 0 && m->temp_c < 120, "temperature is sane");
            CHECK(m->power_mw > 0, "power draw is positive");
        }

        GpuProc procs[64];
        int np = gpu_procs(b, 0, procs, 64);
        if (m && m->has_per_process) {
            CHECK(np >= 0, "per-process figures are enumerable");
        } else {
            printf("  (this card reports no per-process data: %s)\n",
                   m && m->has_per_process ? "yes" : "no");
        }
        CHECK(gpu_metrics(b, 999) == NULL, "out-of-range GPU is NULL");
        CHECK(gpu_procs(b, 0, procs, 0) == 0, "zero-capacity procs is safe");

        gpu_close(b);
    }

    /* the fake backend must work with no GPU at all */
    struct GpuBackend *f = gpu_open_fake();
    CHECK(f != NULL, "the fake backend opens");
    if (f) {
        CHECK(gpu_kind(f) == GPU_KIND_FAKE, "the fake backend identifies itself");
        CHECK(gpu_count(f) == 1, "the fake card is one GPU");
        CHECK(gpu_sample(f), "the fake card samples");
        const GpuMetrics *m = gpu_metrics(f, 0);
        CHECK(m != NULL && m->mem_total_mb > 0, "the fake card reports memory");
        GpuProc fp[8];
        CHECK(gpu_procs(f, 0, fp, 8) == 2, "the fake card has processes");
        gpu_close(f);
    }

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
