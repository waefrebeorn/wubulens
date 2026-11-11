/*
 * classify.h -- what a program is.
 *
 * A kind (ai, game, browser, desktop, dev, system, other) and a group key
 * (the app identity several processes share: Chromium's helpers, Steam's
 * launcher and game, a container's processes). Rule based, from the
 * executable name and the command line only -- no guessing from numbers.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_CLASSIFY_H
#define WUBULENS_CLASSIFY_H

#include <stdbool.h>

typedef enum {
    KIND_AI = 0, KIND_GAME, KIND_VIDEO, KIND_CREATIVE, KIND_BROWSER,
    KIND_DESKTOP, KIND_DEV, KIND_SYSTEM, KIND_OTHER, KIND_COUNT
} AppKind;

/* The kind of a program from its executable name and command line. */
AppKind classify_kind(const char *exe, const char *cmdline);

const char *classify_kind_name(AppKind k);

/* A grouping key: processes that belong to one app share it. Written into
 * key (cap bytes). Falls back to the executable name. */
void classify_group_key(const char *exe, const char *cmdline,
                        char *key, unsigned cap);

/* True when a kind should be shown with its plain description. */
const char *classify_description(AppKind k);

#endif /* WUBULENS_CLASSIFY_H */
