/*
 * classify.c -- program identity rules. See classify.h.
 *
 * The rules are ordered: the first match wins. They are deliberately
 * conservative -- when nothing matches, the program is "other" and the lens
 * says so rather than inventing a category.
 *
 * C11, minimal includes, self-contained.
 */
#include "classify.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

typedef struct { const char *needle; AppKind kind; } Rule;

/* Substring rules on the executable name (lowercase compare). */
static const Rule k_exe_rules[] = {
    /* language-model servers and clients */
    { "llama",        KIND_AI },
    { "vllm",         KIND_AI },
    { "sglang",       KIND_AI },
    { "comfyui",      KIND_AI },
    { "diffuser",     KIND_AI },
    { "whisper",      KIND_AI },
    { "ollama",       KIND_AI },
    { "lmstudio",     KIND_AI },
    { "tensorrt",     KIND_AI },
    /* browsers */
    { "chromium",     KIND_BROWSER },
    { "chrome",       KIND_BROWSER },
    { "firefox",      KIND_BROWSER },
    { "webkit",       KIND_BROWSER },
    /* desktop shell */
    { "kwin",         KIND_DESKTOP },
    { "plasmashell",  KIND_DESKTOP },
    { "gnome-shell",  KIND_DESKTOP },
    { "xfwm",         KIND_DESKTOP },
    { "mutter",       KIND_DESKTOP },
    { "wayland",      KIND_DESKTOP },
    { "Xorg",         KIND_DESKTOP },
    /* development */
    { "code",         KIND_DEV },
    { "rustc",        KIND_DEV },
    { "gcc",          KIND_DEV },
    { "clang",        KIND_DEV },
    { "python",       KIND_DEV },
    { "node",         KIND_DEV },
    /* video */
    { "obs",          KIND_VIDEO },
    { "ffmpeg",       KIND_VIDEO },
    { "vlc",          KIND_VIDEO },
    { "mpv",          KIND_VIDEO },
    /* creative */
    { "blender",      KIND_CREATIVE },
    { "gimp",         KIND_CREATIVE },
    { "krita",        KIND_CREATIVE },
    /* games */
    { "steam",        KIND_GAME },
    { "wineserver",   KIND_GAME },
};

/* Substring rules on the command line (a game binary often has no marker in
 * its name, but its path does). */
static const Rule k_cmd_rules[] = {
    { "steamapps",        KIND_GAME },
    { "proton",           KIND_GAME },
    { ".exe",             KIND_GAME },
    { "llama-server",     KIND_AI },
    { "vllm serve",       KIND_AI },
    { "comfyui",          KIND_AI },
    { "blender",          KIND_CREATIVE },
    { "obs",              KIND_VIDEO },
    { "ffmpeg",           KIND_VIDEO },
    { "chromium",         KIND_BROWSER },
};

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    size_t hl = strlen(hay);
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t j = 0;
        while (j < nl && lc(hay[i + j]) == lc(needle[j])) j++;
        if (j == nl) return 1;
    }
    return 0;
}

AppKind classify_kind(const char *exe, const char *cmdline)
{
    if (exe && exe[0]) {
        for (unsigned i = 0; i < sizeof(k_exe_rules) / sizeof(k_exe_rules[0]); i++)
            if (contains_ci(exe, k_exe_rules[i].needle))
                return k_exe_rules[i].kind;
    }
    if (cmdline && cmdline[0]) {
        for (unsigned i = 0; i < sizeof(k_cmd_rules) / sizeof(k_cmd_rules[0]); i++)
            if (contains_ci(cmdline, k_cmd_rules[i].needle))
                return k_cmd_rules[i].kind;
    }
    return KIND_OTHER;
}

const char *classify_kind_name(AppKind k)
{
    switch (k) {
        case KIND_AI:       return "ai";
        case KIND_GAME:     return "game";
        case KIND_VIDEO:    return "video";
        case KIND_CREATIVE: return "creative";
        case KIND_BROWSER:  return "browser";
        case KIND_DESKTOP:  return "desktop";
        case KIND_DEV:      return "dev";
        case KIND_SYSTEM:   return "system";
        default:            return "other";
    }
}

const char *classify_description(AppKind k)
{
    switch (k) {
        case KIND_AI:       return "Serves or runs a language model; memory stays "
                                 "reserved while it waits for requests.";
        case KIND_GAME:     return "A game; it may hold memory between frames and "
                                 "spike without warning.";
        case KIND_VIDEO:    return "Recording, streaming or transcoding; it can "
                                 "use the video engines while shaders are idle.";
        case KIND_CREATIVE: return "Image or 3D work; large canvases and caches "
                                 "stay resident.";
        case KIND_BROWSER:  return "A browser; many helper processes share one "
                                 "app, and video decode runs on the GPU.";
        case KIND_DESKTOP:  return "The desktop shell; it composites every frame "
                                 "you see.";
        case KIND_DEV:      return "A development tool or runtime; builds and "
                                 "runs can load the GPU.";
        case KIND_SYSTEM:   return "Part of the operating system.";
        default:            return "";
    }
}

const char *classify_description_by_name(const char *kind_name)
{
    if (!kind_name) return "";
    for (int k = 0; k < KIND_COUNT; k++) {
        if (strcmp(kind_name, classify_kind_name((AppKind)k)) == 0)
            return classify_description((AppKind)k);
    }
    return "";
}

void classify_group_key(const char *exe, const char *cmdline,
                        char *key, unsigned cap)
{
    if (!key || cap == 0) return;
    key[0] = '\0';

    /* Multi-process apps collapse to one row. */
    if (exe && contains_ci(exe, "chromium")) { snprintf(key, cap, "Chromium"); return; }
    if (exe && contains_ci(exe, "chrome"))   { snprintf(key, cap, "Chrome"); return; }
    if (exe && contains_ci(exe, "firefox"))  { snprintf(key, cap, "Firefox"); return; }
    if (cmdline && contains_ci(cmdline, "steam"))
        { snprintf(key, cap, "Steam"); return; }

    /* otherwise the executable name is the app; when /proc/<pid>/exe could
     * not be read (a process owned by another user) fall back to the first
     * word of the command line -- the program path -- not the whole line */
    if (exe && exe[0]) { snprintf(key, cap, "%s", exe); return; }
    if (cmdline && cmdline[0]) {
        unsigned i = 0;
        while (cmdline[i] == ' ') i++;
        unsigned start = i;
        while (cmdline[i] && cmdline[i] != ' ') i++;
        unsigned len = i - start;
        if (len >= cap) len = cap - 1;
        memcpy(key, cmdline + start, len);
        key[len] = '\0';
        /* strip the directory: "Xorg", not "/usr/lib/xorg/Xorg" */
        char *slash = strrchr(key, '/');
        if (slash && slash[1]) {
            memmove(key, slash + 1, strlen(slash + 1) + 1);
        }
        return;
    }
    snprintf(key, cap, "unknown");
}
