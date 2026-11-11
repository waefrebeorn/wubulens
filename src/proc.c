/*
 * proc.c -- /proc reading. See proc.h for the contract.
 *
 * C11, minimal includes, self-contained.
 */
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int is_numeric(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

static void basename_of(const char *path, char *out, unsigned cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    /* bounded: never truncate-surprise the caller */
    snprintf(out, cap, "%.*s", (int)(cap - 1), base);
}

static void read_link(const char *path, char *out, unsigned cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    ssize_t n = readlink(path, out, cap - 1);
    if (n < 0) return;
    out[n] = '\0';
}

/* /proc/<pid>/cmdline is NUL-separated; join the arguments with spaces. */
static void read_cmdline(unsigned int pid, char *out, unsigned cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/cmdline", pid);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char raw[1024];
    size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    if (n == 0) return;
    raw[n] = '\0';
    /* join on NULs, stopping at the last non-empty argument */
    size_t w = 0;
    size_t i = 0;
    while (i < n && w + 1 < cap) {
        size_t len = strnlen(raw + i, n - i);
        if (len == 0) { i++; continue; }
        if (w > 0) out[w++] = ' ';
        size_t copy = len;
        if (w + copy >= cap) copy = cap - 1 - w;
        memcpy(out + w, raw + i, copy);
        w += copy;
        out[w] = '\0';
        i += len + 1;
    }
}

static void read_rss(unsigned int pid, double *rss_mb)
{
    *rss_mb = 0.0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/statm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned long size_pages = 0, resident_pages = 0;
    if (fscanf(f, "%lu %lu", &size_pages, &resident_pages) == 2) {
        long pagesize = sysconf(_SC_PAGESIZE);
        if (pagesize <= 0) pagesize = 4096;
        *rss_mb = (double)resident_pages * (double)pagesize / (1024.0 * 1024.0);
    }
    fclose(f);
}

static int uid_from_status(unsigned int pid, char *user, unsigned cap)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    unsigned int uid = 0;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            if (sscanf(line + 4, "%u", &uid) == 1) found = 1;
            break;
        }
    }
    fclose(f);
    if (!found) return -1;
    struct passwd *pw = getpwuid(uid);
    if (pw) snprintf(user, cap, "%s", pw->pw_name);
    else snprintf(user, cap, "%u", uid);
    return 0;
}

const char *proc_user(unsigned int pid)
{
    static char user[64];
    user[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return "?";
    char line[256];
    unsigned int uid = 0;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            if (sscanf(line + 4, "%u", &uid) == 1) found = 1;
            break;
        }
    }
    fclose(f);
    if (!found) return "?";
    struct passwd *pw = getpwuid(uid);
    if (pw) snprintf(user, sizeof(user), "%s", pw->pw_name);
    else snprintf(user, sizeof(user), "%u", uid);
    return user;
}

bool proc_lookup(unsigned int pid, ProcInfo *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    char path[64];
    snprintf(path, sizeof(path), "/proc/%u", pid);
    struct stat st;
    if (stat(path, &st) != 0) { out->exists = false; return false; }

    out->pid = pid;
    out->exists = true;

    snprintf(path, sizeof(path), "/proc/%u/exe", pid);
    read_link(path, out->exe_path, sizeof(out->exe_path));
    basename_of(out->exe_path, out->exe, sizeof(out->exe));

    read_cmdline(pid, out->cmdline, sizeof(out->cmdline));

    char user[64];
    if (uid_from_status(pid, user, sizeof(user)) == 0)
        snprintf(out->user, sizeof(out->user), "%s", user);
    else
        snprintf(out->user, sizeof(out->user), "?");

    read_rss(pid, &out->rss_mb);
    return true;
}

int proc_count(void)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (is_numeric(e->d_name)) n++;
    closedir(d);
    return n;
}

int proc_list(ProcInfo *out, int cap)
{
    if (!out || cap <= 0) return 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < cap) {
        if (!is_numeric(e->d_name)) continue;
        unsigned int pid = (unsigned int)strtoul(e->d_name, NULL, 10);
        if (proc_lookup(pid, &out[n])) n++;
    }
    closedir(d);

    /* insertion sort by pid (readdir order is arbitrary) */
    for (int i = 1; i < n; i++) {
        ProcInfo key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].pid > key.pid) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }
    return n;
}
