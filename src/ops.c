/*
 * ops.c -- real process operations. See ops.h.
 *
 * C11, minimal includes, self-contained.
 */
#include "ops.h"

#include <signal.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <time.h>

OpResult op_end_task(unsigned int pid)
{
    if (pid == 0) return OP_ERR_NOTFOUND;

    /* Ask politely first; escalate only if it is still alive. */
    if (kill((pid_t)pid, SIGTERM) != 0) {
        if (errno == ESRCH) return OP_ERR_NOTFOUND;
        if (errno == EPERM) return OP_ERR_PERM;
        return OP_ERR_FAIL;
    }
    for (int i = 0; i < 50; i++) {           /* about five seconds */
        struct timespec ts = {0, 100000000L};   /* 100 ms */
        nanosleep(&ts, NULL);
        if (kill((pid_t)pid, 0) != 0) {
            if (errno == ESRCH) return OP_OK;   /* gone */
        }
    }
    if (kill((pid_t)pid, SIGKILL) != 0) {
        if (errno == ESRCH) return OP_OK;
        if (errno == EPERM) return OP_ERR_PERM;
        return OP_ERR_FAIL;
    }
    return OP_OK;
}

OpResult op_suspend(unsigned int pid)
{
    if (pid == 0) return OP_ERR_NOTFOUND;
    if (kill((pid_t)pid, SIGSTOP) != 0) {
        if (errno == ESRCH) return OP_ERR_NOTFOUND;
        if (errno == EPERM) return OP_ERR_PERM;
        return OP_ERR_FAIL;
    }
    return OP_OK;
}

OpResult op_resume(unsigned int pid)
{
    if (pid == 0) return OP_ERR_NOTFOUND;
    if (kill((pid_t)pid, SIGCONT) != 0) {
        if (errno == ESRCH) return OP_ERR_NOTFOUND;
        if (errno == EPERM) return OP_ERR_PERM;
        return OP_ERR_FAIL;
    }
    return OP_OK;
}

OpResult op_set_priority(unsigned int pid, int nice)
{
    if (pid == 0) return OP_ERR_NOTFOUND;
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    if (setpriority(PRIO_PROCESS, (id_t)pid, nice) != 0) {
        if (errno == ESRCH) return OP_ERR_NOTFOUND;
        if (errno == EPERM || errno == EACCES) return OP_ERR_PERM;
        return OP_ERR_FAIL;
    }
    return OP_OK;
}

OpResult op_efficiency(unsigned int pid)
{
    return op_set_priority(pid, 19);
}

OpResult op_normal(unsigned int pid)
{
    return op_set_priority(pid, 0);
}

const char *op_error(OpResult r)
{
    switch (r) {
        case OP_OK:          return "done";
        case OP_ERR_NOTFOUND: return "no such process";
        case OP_ERR_PERM:    return "not permitted (own the process or run as root)";
        default:             return "the operation failed";
    }
}

const char *op_name(const char *key)
{
    if (!key) return "operation";
    if (strcmp(key, "x") == 0) return "end task";
    if (strcmp(key, "p") == 0) return "suspend";
    if (strcmp(key, "r") == 0) return "resume";
    if (strcmp(key, "n") == 0) return "priority";
    return "operation";
}
