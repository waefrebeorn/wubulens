/*
 * session.c -- read the machine's start-up surface. See session.h.
 *
 * C11, minimal includes, self-contained.
 */
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

/* Copy with an explicit bound -- no truncation surprise, no warning. */
static void copy_bounded(char *dst, unsigned cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t n = strcspn(src, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Parse one .desktop file. Returns 1 when it is a real autostart entry. */
static int parse_desktop(const char *path, AutostartEntry *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    memset(out, 0, sizeof(*out));
    copy_bounded(out->file, sizeof(out->file), path);
    out->enabled = 1;

    char line[1024];
    int in_entry = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        if (line[0] == '[') { in_entry = (strcmp(line, "[Desktop Entry]") == 0); continue; }
        if (!in_entry) continue;
        if (strncmp(line, "Name=", 5) == 0)
            copy_bounded(out->name, sizeof(out->name), line + 5);
        else if (strncmp(line, "Exec=", 5) == 0)
            copy_bounded(out->exec, sizeof(out->exec), line + 5);
        else if (strcmp(line, "Hidden=true") == 0)
            out->enabled = 0;
        else if (strcmp(line, "X-GNOME-Autostart-enabled=false") == 0)
            out->enabled = 0;
    }
    fclose(f);

    /* an entry with no Name is not a usable program row */
    return out->name[0] != '\0';
}

static int scan_autostart_dir(const char *dir, AutostartEntry *out, int cap, int *count)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *count < cap) {
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 8, ".desktop") != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (parse_desktop(path, &out[*count])) (*count)++;
    }
    closedir(d);
    return 1;
}

int autostart_count(void)
{
    static AutostartEntry entries[SESSION_MAX];
    static int count = -1;
    if (count >= 0) return count;

    count = 0;
    const char *home = getenv("HOME");
    const char *xcgh = getenv("XDG_CONFIG_HOME");
    char dir[512];

    if (xcgh && xcgh[0]) {
        snprintf(dir, sizeof(dir), "%s/autostart", xcgh);
    } else if (home && home[0]) {
        snprintf(dir, sizeof(dir), "%s/.config/autostart", home);
    } else {
        dir[0] = '\0';
    }
    if (dir[0]) scan_autostart_dir(dir, entries, SESSION_MAX, &count);
    scan_autostart_dir("/etc/xdg/autostart", entries, SESSION_MAX, &count);

    /* stash for autostart_entry() -- a file-scope cache is the simplest way
     * to keep the entries alive without an opaque handle */
    extern AutostartEntry g_autostart[SESSION_MAX];
    extern int g_autostart_count;
    memcpy(g_autostart, entries, sizeof(entries));
    g_autostart_count = count;
    return count;
}

/* the cache lives here so the accessor can hand back stable pointers */
AutostartEntry g_autostart[SESSION_MAX];
int g_autostart_count = 0;

const AutostartEntry *autostart_entry(int i)
{
    if (i < 0 || i >= autostart_count()) return NULL;
    return &g_autostart[i];
}

int autostart_set_enabled(int i, int enabled)
{
    const AutostartEntry *e = autostart_entry(i);
    if (!e) return -1;
    /* only a file under $HOME (or XDG_CONFIG_HOME) may be changed */
    const char *home = getenv("HOME");
    const char *xcgh = getenv("XDG_CONFIG_HOME");
    int user_file = 0;
    if (xcgh && strncmp(e->file, xcgh, strlen(xcgh)) == 0) user_file = 1;
    else if (home && strncmp(e->file, home, strlen(home)) == 0) user_file = 1;
    if (!user_file) return -1;

    FILE *f = fopen(e->file, "r");
    if (!f) return -1;
    char body[4096];
    size_t n = fread(body, 1, sizeof(body) - 1, f);
    body[n] = '\0';
    fclose(f);

    /* drop any existing enabled key, then add the new one */
    char out[4096];
    size_t w = 0;
    char *p = body;
    while (*p && w + 2 < sizeof(out)) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
        if (strncmp(p, "X-GNOME-Autostart-enabled=", 26) == 0 ||
            strncmp(p, "Hidden=", 7) == 0) {
            p += len;
            continue;
        }
        memcpy(out + w, p, len);
        w += len;
        p += len;
    }
    snprintf(out + w, sizeof(out) - w, "X-GNOME-Autostart-enabled=%s\n",
             enabled ? "true" : "false");

    FILE *g = fopen(e->file, "w");
    if (!g) return -1;
    fputs(out, g);
    fclose(g);
    return 0;
}

/* -- services ------------------------------------------------------------ */

static int unit_state(const char *path, const char *name, char *state, unsigned cap)
{
    /* enabled when a target .wants symlink points at the unit */
    const char *home = getenv("HOME");
    char link[600];
    if (home && strncmp(path, home, strlen(home)) == 0) {
        snprintf(link, sizeof(link), "%s/.config/systemd/user/default.target.wants/%s",
                 home, name);
        struct stat st;
        if (lstat(link, &st) == 0) { snprintf(state, cap, "enabled"); return 1; }
    }
    snprintf(state, cap, "disabled");
    return 0;
}

int service_count(void)
{
    static ServiceEntry entries[SESSION_MAX];
    static int count = -1;
    if (count >= 0) return count;
    count = 0;

    const char *home = getenv("HOME");
    const char *dirs[3];
    char user_dir[512];
    if (home) snprintf(user_dir, sizeof(user_dir), "%s/.config/systemd/user", home);
    dirs[0] = home ? user_dir : NULL;
    dirs[1] = "/etc/systemd/user";
    dirs[2] = "/etc/systemd/system";

    for (int d = 0; d < 3 && count < SESSION_MAX; d++) {
        if (!dirs[d]) continue;
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && count < SESSION_MAX) {
            size_t n = strlen(e->d_name);
            if (n < 9 || strcmp(e->d_name + n - 8, ".service") != 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
            ServiceEntry *se = &entries[count];
            memset(se, 0, sizeof(*se));
            snprintf(se->name, sizeof(se->name), "%.*s", 127, e->d_name);
            se->name[strlen(se->name) - 8] = '\0';   /* strip .service */
            copy_bounded(se->file, sizeof(se->file), path);
            unit_state(path, e->d_name, se->state, sizeof(se->state));

            FILE *f = fopen(path, "r");
            if (f) {
                char line[512];
                while (fgets(line, sizeof(line), f)) {
                    if (strncmp(line, "Description=", 12) == 0) {
                        copy_bounded(se->description, sizeof(se->description), line + 12);
                        se->description[strcspn(se->description, "\r\n")] = '\0';
                        break;
                    }
                }
                fclose(f);
            }
            count++;
        }
        closedir(dir);
    }

    extern ServiceEntry g_services[SESSION_MAX];
    extern int g_service_count;
    memcpy(g_services, entries, sizeof(entries));
    g_service_count = count;
    return count;
}

ServiceEntry g_services[SESSION_MAX];
int g_service_count = 0;

const ServiceEntry *service_entry(int i)
{
    if (i < 0 || i >= service_count()) return NULL;
    return &g_services[i];
}
