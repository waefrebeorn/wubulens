/*
 * perf.c -- /proc resource sampling. See perf.h for the contract.
 *
 * C11, minimal includes, self-contained.
 */
#include "perf.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Delta state: previous counter readings. */
struct PerfState {
    int primed;
    /* CPU jiffies: [0] = total, [1..] = per core. index 0..PERF_MAX_CORES */
    unsigned long long prev_cpu[PERF_MAX_CORES + 1][8];
    int prev_cores;
    /* Disk sectors */
    unsigned long long prev_disk_read_sectors;
    unsigned long long prev_disk_write_sectors;
    /* Net bytes */
    unsigned long long prev_net_rx_bytes;
    unsigned long long prev_net_tx_bytes;
    double prev_secs;
    int available;
};

static struct PerfState g_perf;

/* Parse one /proc/stat line after the "cpu" / "cpuN" tag into 8 jiffy
 * buckets. Returns the number of fields parsed. */
static int parse_cpu_line(const char *p, unsigned long long *out)
{
    int n = 0;
    while (n < 8) {
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9') break;
        out[n] = strtoull(p, (char **)&p, 10);
        n++;
    }
    return n;
}

/* Total and per-core CPU jiffies from /proc/stat. Fills total[8] and the
 * per-core rows; returns the core count. */
static int read_proc_stat(unsigned long long total[8],
                          unsigned long long cores[][8], int max_cores)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;

    char line[512];
    int ncores = 0;
    memset(total, 0, 8 * sizeof(unsigned long long));

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;   /* the cpu lines are first */
        if (line[3] == ' ') {
            parse_cpu_line(line + 4, total);
        } else if (line[3] >= '0' && line[3] <= '9') {
            if (ncores < max_cores)
                parse_cpu_line(line + 3 + strspn(line + 3, "0123456789"), cores[ncores]);
            ncores++;
        }
    }
    fclose(f);
    return ncores;
}

/* Busy percentage from two jiffy rows (8 buckets each). */
static double busy_pct(const unsigned long long prev[8],
                       const unsigned long long cur[8])
{
    unsigned long long prev_idle = prev[3] + (prev[4] ? prev[4] : 0);
    unsigned long long cur_idle  = cur[3] + (cur[4] ? cur[4] : 0);
    unsigned long long prev_tot = 0, cur_tot = 0;
    for (int i = 0; i < 8; i++) { prev_tot += prev[i]; cur_tot += cur[i]; }
    unsigned long long dt = cur_tot - prev_tot;
    unsigned long long di = cur_idle - prev_idle;
    if (dt == 0) return 0.0;
    double busy = (double)(dt - di) * 100.0 / (double)dt;
    if (busy < 0.0) busy = 0.0;
    if (busy > 100.0) busy = 100.0;
    return busy;
}

static void read_meminfo(PerfSample *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    char key[64];
    unsigned long long kb;
    unsigned long long memtotal = 0, memavail = 0, swaptotal = 0, swapfree = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %llu", key, &kb) != 2) continue;
        if (strcmp(key, "MemTotal:") == 0)     memtotal = kb;
        else if (strcmp(key, "MemAvailable:") == 0) memavail = kb;
        else if (strcmp(key, "SwapTotal:") == 0)     swaptotal = kb;
        else if (strcmp(key, "SwapFree:") == 0)      swapfree = kb;
    }
    fclose(f);

    s->mem_total_mb   = memtotal / 1024;
    s->mem_avail_mb   = memavail / 1024;
    s->mem_used_mb    = (memtotal > memavail) ? (memtotal - memavail) / 1024 : 0;
    s->swap_total_mb  = swaptotal / 1024;
    s->swap_used_mb   = (swaptotal > swapfree) ? (swaptotal - swapfree) / 1024 : 0;
}

/* Whole-disk devices only (a partition is not a disk). */
static int is_whole_disk(const char *name)
{
    /* sdX, vdX, xvdX, nvmeXnY, mmcblkX -- but not sdX1, nvmeXnYpZ, mmcblkXpY */
    if (strncmp(name, "sd", 2) == 0 || strncmp(name, "vd", 2) == 0 ||
        strncmp(name, "xvd", 3) == 0)
        return name[strlen(name) - 1] < '0' || name[strlen(name) - 1] > '9';
    if (strncmp(name, "nvme", 4) == 0) {
        const char *p = strchr(name, 'p');
        return p == NULL;                 /* nvme0n1 yes, nvme0n1p1 no */
    }
    if (strncmp(name, "mmcblk", 6) == 0) {
        const char *p = strchr(name, 'p');
        return p == NULL;
    }
    return 0;
}

static void read_diskstats(unsigned long long *read_sectors,
                           unsigned long long *write_sectors)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;
    char line[256];
    *read_sectors = 0;
    *write_sectors = 0;
    while (fgets(line, sizeof(line), f)) {
        int maj, min;
        char name[64];
        unsigned long long rd_done, rd_merged, rd_sectors;
        unsigned long long wr_done, wr_merged, wr_sectors;
        if (sscanf(line, "%d %d %63s %llu %llu %llu %*u %llu %llu %llu",
                   &maj, &min, name, &rd_done, &rd_merged, &rd_sectors,
                   &wr_done, &wr_merged, &wr_sectors) != 9)
            continue;
        if (!is_whole_disk(name)) continue;
        *read_sectors += rd_sectors;
        *write_sectors += wr_sectors;
    }
    fclose(f);
}

static void read_netdev(unsigned long long *rx_bytes,
                        unsigned long long *tx_bytes)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512];
    *rx_bytes = 0;
    *tx_bytes = 0;
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (lineno <= 2) continue;          /* the two header lines */
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char name[64];
        int n = (int)(colon - line);
        while (n > 0 && line[n - 1] == ' ') n--;
        if (n <= 0 || n >= 63) continue;
        memcpy(name, line, (size_t)n);
        name[n] = '\0';
        if (strcmp(name, "lo") == 0) continue;
        unsigned long long vals[16];
        memset(vals, 0, sizeof(vals));
        int got = 0;
        char *p = colon + 1;
        while (got < 16) {
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') break;
            vals[got++] = strtoull(p, &p, 10);
        }
        if (got >= 8) {
            *rx_bytes += vals[0];
            *tx_bytes += vals[8];
        }
    }
    fclose(f);
}

static void read_loadavg(PerfSample *s)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;
    double a = 0, b = 0, c = 0;
    if (fscanf(f, "%lf %lf %lf", &a, &b, &c) == 3) {
        s->load_avg[0] = a; s->load_avg[1] = b; s->load_avg[2] = c;
    }
    fclose(f);
}

static void read_uptime(PerfSample *s)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return;
    double up = 0;
    if (fscanf(f, "%lf", &up) == 1) s->uptime_secs = up;
    fclose(f);
}

void wubu_perf_init(void)
{
    memset(&g_perf, 0, sizeof(g_perf));
    /* Prime the counters so the first sample has a baseline. */
    unsigned long long total[8];
    unsigned long long cores[PERF_MAX_CORES][8];
    g_perf.prev_cores = read_proc_stat(total, cores, PERF_MAX_CORES);
    if (g_perf.prev_cores >= 0) {
        memcpy(g_perf.prev_cpu[0], total, sizeof(total));
        for (int i = 0; i < g_perf.prev_cores && i < PERF_MAX_CORES; i++)
            memcpy(g_perf.prev_cpu[i + 1], cores[i], sizeof(total));
        g_perf.available = 1;
    }
    read_diskstats(&g_perf.prev_disk_read_sectors, &g_perf.prev_disk_write_sectors);
    read_netdev(&g_perf.prev_net_rx_bytes, &g_perf.prev_net_tx_bytes);
    g_perf.prev_secs = 1.0;             /* the first sample reports zero rates */
    g_perf.primed = 1;
}

bool wubu_perf_available(void) { return g_perf.available != 0; }

void wubu_perf_sample(PerfSample *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!g_perf.primed) wubu_perf_init();

    /* CPU */
    unsigned long long total[8];
    unsigned long long cores[PERF_MAX_CORES][8];
    int ncores = read_proc_stat(total, cores, PERF_MAX_CORES);
    if (ncores >= 0) {
        out->cpu_total_pct = busy_pct(g_perf.prev_cpu[0], total);
        out->cpu_cores = ncores;
        for (int i = 0; i < ncores && i < PERF_MAX_CORES; i++) {
            out->cpu_core_pct[i] = busy_pct(g_perf.prev_cpu[i + 1], cores[i]);
            memcpy(g_perf.prev_cpu[i + 1], cores[i], sizeof(total));
        }
        memcpy(g_perf.prev_cpu[0], total, sizeof(total));
        g_perf.prev_cores = ncores;
    }

    /* Memory, load, uptime: instantaneous */
    read_meminfo(out);
    read_loadavg(out);
    read_uptime(out);

    /* Disk and network: delta rates over the elapsed wall time */
    unsigned long long rd_sec = 0, wr_sec = 0;
    read_diskstats(&rd_sec, &wr_sec);
    unsigned long long rx = 0, tx = 0;
    read_netdev(&rx, &tx);

    double secs = 1.0;
    if (out->uptime_secs > g_perf.prev_secs)
        secs = out->uptime_secs - g_perf.prev_secs;
    if (secs <= 0.0) secs = 1.0;

    /* sectors are 512 bytes; bytes -> MB is /(1024*1024) */
    double rd_delta_mb = (double)(rd_sec - g_perf.prev_disk_read_sectors) * 512.0 / (1024.0 * 1024.0);
    double wr_delta_mb = (double)(wr_sec - g_perf.prev_disk_write_sectors) * 512.0 / (1024.0 * 1024.0);
    out->disk_read_mbps  = rd_delta_mb / secs;
    out->disk_write_mbps = wr_delta_mb / secs;
    out->disk_read_total_mb  = rd_sec * 512ULL / (1024ULL * 1024ULL);
    out->disk_write_total_mb = wr_sec * 512ULL / (1024ULL * 1024ULL);

    double rx_delta_mb = (double)(rx - g_perf.prev_net_rx_bytes) / (1024.0 * 1024.0);
    double tx_delta_mb = (double)(tx - g_perf.prev_net_tx_bytes) / (1024.0 * 1024.0);
    out->net_rx_mbps = rx_delta_mb / secs;
    out->net_tx_mbps = tx_delta_mb / secs;
    out->net_rx_total_mb = rx / (1024ULL * 1024ULL);
    out->net_tx_total_mb = tx / (1024ULL * 1024ULL);

    g_perf.prev_disk_read_sectors = rd_sec;
    g_perf.prev_disk_write_sectors = wr_sec;
    g_perf.prev_net_rx_bytes = rx;
    g_perf.prev_net_tx_bytes = tx;
    g_perf.prev_secs = out->uptime_secs;
}

void wubu_perf_uptime_str(const PerfSample *s, char *buf, unsigned cap)
{
    if (!buf || cap == 0) return;
    if (!s) { snprintf(buf, cap, "unknown"); return; }
    unsigned long long t = (unsigned long long)s->uptime_secs;
    unsigned long long days = t / 86400;
    unsigned long long hours = (t % 86400) / 3600;
    unsigned long long mins = (t % 3600) / 60;
    if (days > 0)
        snprintf(buf, cap, "%llud %lluh %llum", days, hours, mins);
    else if (hours > 0)
        snprintf(buf, cap, "%lluh %llum", hours, mins);
    else
        snprintf(buf, cap, "%llum", mins);
}
