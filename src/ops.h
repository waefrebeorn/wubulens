/*
 * ops.h -- the operations a task manager can actually perform.
 *
 * Every one of these acts on a real pid through the kernel:
 *
 *   end task    SIGTERM, then SIGKILL if it is still there
 *   suspend     SIGSTOP (freeze without killing)
 *   resume      SIGCONT
 *   priority    setpriority() -- a nice value, -20 (favour) .. 19 (yield)
 *
 * Nothing here is simulated. Failures come back as an OpResult the caller can
 * show, and a permission failure is reported as such, never retried blindly.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_OPS_H
#define WUBULENS_OPS_H

typedef enum {
    OP_OK = 0,
    OP_ERR_NOTFOUND = -1,
    OP_ERR_PERM = -2,
    OP_ERR_FAIL = -3
} OpResult;

OpResult op_end_task(unsigned int pid);
OpResult op_suspend(unsigned int pid);
OpResult op_resume(unsigned int pid);
OpResult op_set_priority(unsigned int pid, int nice);

const char *op_error(OpResult r);

/* A short human label for an operation, for the footer and confirmations. */
const char *op_name(const char *key);

#endif /* WUBULENS_OPS_H */
