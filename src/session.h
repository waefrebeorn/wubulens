/*
 * session.h -- the machine's own start-up surface.
 *
 * Autostart: the XDG .desktop files in $XDG_CONFIG_HOME/autostart and
 * /etc/xdg/autostart. Services: the systemd user units on disk, with the
 * enabled state read from the target .wants symlinks (no subprocess).
 *
 * Everything is read from the filesystem; nothing is invented. Entries that
 * cannot be parsed are skipped, not guessed.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_SESSION_H
#define WUBULENS_SESSION_H

#include <stdbool.h>

#define SESSION_MAX 128

typedef struct {
    char name[128];
    char exec[256];
    bool enabled;        /* false when Hidden or ...-enabled=false */
    char file[256];      /* the file it came from */
} AutostartEntry;

typedef struct {
    char name[128];
    char description[256];
    char state[32];      /* "enabled", "disabled", "static", "masked" */
    char file[256];
} ServiceEntry;

int autostart_count(void);
const AutostartEntry *autostart_entry(int i);

/* Enable or disable one entry, by writing its .desktop file. Returns 0 on
 * success. Only a user file can be changed; a system file is refused. */
int autostart_set_enabled(int i, int enabled);

int service_count(void);
const ServiceEntry *service_entry(int i);

#endif /* WUBULENS_SESSION_H */
