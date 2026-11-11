/*
 * proc.h -- what a process is: executable, command line, owner, memory.
 *
 * All of it read from /proc/<pid>/{exe,cmdline,status,statm}. When a field
 * cannot be read (a kernel thread has no exe, a process exits mid-read) it is
 * empty or zero -- never invented.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_PROC_H
#define WUBULENS_PROC_H

#include <stdbool.h>

typedef struct {
    unsigned int pid;
    char     exe[256];      /* basename of /proc/<pid>/exe, "" if none */
    char     exe_path[512]; /* full path of the executable */
    char     cmdline[512];  /* NUL-separated args joined with spaces */
    char     user[64];      /* owner name, "?" if unknown */
    double   rss_mb;        /* resident set from statm */
    bool     exists;        /* false when the pid vanished mid-read */
} ProcInfo;

/* Number of numeric /proc entries (an upper bound on live pids). */
int proc_count(void);

/* Fill out[] with up to cap processes, in pid order. Returns the count. */
int proc_list(ProcInfo *out, int cap);

/* One process. Returns true on success. */
bool proc_lookup(unsigned int pid, ProcInfo *out);

/* Owner name for a pid ("?" when unknown). */
const char *proc_user(unsigned int pid);

#endif /* WUBULENS_PROC_H */
