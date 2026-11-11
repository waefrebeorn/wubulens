/*
 * test_ops.c -- prove the operations act on a REAL process.
 *
 * Forks a child, then asserts the kernel's own view changes:
 *   suspend  -> /proc/<pid>/stat state 'T'
 *   resume   -> state leaves 'T'
 *   priority -> /proc/<pid>/stat nice matches
 *   end task -> the process is gone
 *
 * C11, self-contained.
 */
#include "ops.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>

static int failures;
static int checks;
#define CHECK(c, m) do { \
    checks++; \
    if (!(c)) { printf("  FAIL: %s\n", m); failures++; } \
} while (0)

/* The process state letter from /proc/<pid>/stat, or '?' when gone. */
static char proc_state(unsigned int pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return '?';
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    /* stat is "pid (comm) state ..." -- comm may contain spaces, so find the
     * last ')' and take the next non-space character */
    char *rparen = strrchr(buf, ')');
    if (!rparen) return '?';
    char *p = rparen + 1;
    while (*p == ' ') p++;
    return *p;
}

static int proc_nice(unsigned int pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -999;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    char *rparen = strrchr(buf, ')');
    if (!rparen) return -999;
    /* fields after comm: state ppid pgrp session tty_nr tpgid flags minflt
     * cminflt majflt cmajflt utime stime cutime cstime priority nice ... */
    char *p = rparen + 1;
    int field = 0;
    int nice = -999;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        field++;
        if (field == 17) { nice = atoi(p); break; }   /* nice is field 17 */
        while (*p && *p != ' ') p++;
    }
    return nice;
}

int main(void)
{
    printf("\n=== wubulens ops ===\n");

    pid_t child = fork();
    if (child < 0) { printf("FAIL: fork failed\n"); return 1; }
    if (child == 0) {
        /* the child just exists and sleeps */
        for (;;) pause();
        _exit(0);
    }
    unsigned int pid = (unsigned int)child;
    /* give the kernel a moment to settle the child's state */
    for (int i = 0; i < 20 && proc_state(pid) == '?'; i++) {
        struct timespec ts = {0, 50000000L};
        nanosleep(&ts, NULL);
    }
    CHECK(proc_state(pid) != '?', "the child exists");

    /* suspend */
    CHECK(op_suspend(pid) == OP_OK, "suspend succeeds");
    {
        struct timespec ts = {0, 100000000L};
        nanosleep(&ts, NULL);
        CHECK(proc_state(pid) == 'T', "the kernel reports the child stopped");
    }

    /* resume */
    CHECK(op_resume(pid) == OP_OK, "resume succeeds");
    {
        struct timespec ts = {0, 100000000L};
        nanosleep(&ts, NULL);
        CHECK(proc_state(pid) != 'T', "the kernel reports the child running");
    }

    /* priority */
    CHECK(op_set_priority(pid, 10) == OP_OK, "setpriority succeeds");
    CHECK(proc_nice(pid) == 10, "the child's nice value is 10");

    /* end task: SIGTERM, and the process is gone */
    CHECK(op_end_task(pid) == OP_OK, "end task succeeds");
    {
        struct timespec ts = {0, 100000000L};
        nanosleep(&ts, NULL);
        /* terminated: either reaped (gone) or a zombie awaiting our waitpid */
        {
            char st = proc_state(pid);
            CHECK(st == '?' || st == 'Z', "the child is terminated");
        }
    }
    waitpid(child, NULL, WNOHANG);

    /* a pid that does not exist is reported, not crashed on */
    CHECK(op_suspend(999999u) == OP_ERR_NOTFOUND, "a dead pid is not found");
    CHECK(op_resume(999999u) == OP_ERR_NOTFOUND, "a dead pid is not found (resume)");
    CHECK(op_end_task(999999u) == OP_ERR_NOTFOUND, "a dead pid is not found (end)");
    CHECK(op_set_priority(999999u, 5) == OP_ERR_NOTFOUND, "a dead pid is not found (nice)");

    /* the error text is human, not a raw errno */
    CHECK(strcmp(op_error(OP_ERR_PERM), "done") != 0, "a failure is not called done");

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
