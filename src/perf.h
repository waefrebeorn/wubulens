/*
 * perf.h -- real system resource sampling for the WuBu lens.
 *
 * Every number comes from /proc; nothing is estimated or fabricated.
 *
 *   /proc/stat         per-core and total CPU jiffies -> busy percentage
 *   /proc/meminfo      total / available / swap
 *   /proc/diskstats    whole-disk sectors -> read/write MB/s (delta based)
 *   /proc/net/dev      interface bytes -> rx/tx MB/s (delta based)
 *   /proc/loadavg      1/5/15 minute load
 *   /proc/uptime       uptime in seconds
 *
 * Rates are DELTAS between two samples: wubu_perf_init() primes the
 * counters, the first wubu_perf_sample() reports zero rates, later calls
 * report real ones. On the hosted body /proc is the host's own; on metal it
 * is the OS's. When a file is missing the field stays zero and
 * wubu_perf_available() says so.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_PERF_H
#define WUBULENS_PERF_H

#include <stdbool.h>

#define PERF_MAX_CORES 128

typedef struct {
    /* CPU */
    double  cpu_total_pct;              /* 0..100 */
    double  cpu_core_pct[PERF_MAX_CORES];
    int     cpu_cores;
    double  load_avg[3];
    /* Memory (MB) */
    unsigned long long mem_total_mb;
    unsigned long long mem_avail_mb;
    unsigned long long mem_used_mb;
    unsigned long long swap_total_mb;
    unsigned long long swap_used_mb;
    /* Disk (MB/s and cumulative MB) */
    double  disk_read_mbps;
    double  disk_write_mbps;
    unsigned long long disk_read_total_mb;
    unsigned long long disk_write_total_mb;
    /* Network (MB/s and cumulative MB) */
    double  net_rx_mbps;
    double  net_tx_mbps;
    unsigned long long net_rx_total_mb;
    unsigned long long net_tx_total_mb;
    /* Uptime (seconds) */
    double  uptime_secs;
} PerfSample;

void wubu_perf_init(void);
void wubu_perf_sample(PerfSample *out);
bool wubu_perf_available(void);
void wubu_perf_uptime_str(const PerfSample *s, char *buf, unsigned cap);

#endif /* WUBULENS_PERF_H */
