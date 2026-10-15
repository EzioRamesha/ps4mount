/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Path and name rules of the panel's file manager. */
#include "fm.h"

#include <string.h>

int fm_drive_of(const char *path) {
    if (strncmp(path, "/mnt/usb", 8) == 0 && path[8] >= '0' && path[8] <= '7' &&
        (path[9] == 0 || path[9] == '/'))
        return path[8] - '0';
    if (strncmp(path, "/mnt/ext", 8) == 0 && path[8] >= '0' && path[8] <= '1' &&
        (path[9] == 0 || path[9] == '/'))
        return 8 + (path[8] - '0');
    return -1;
}

int fm_path_ok(const char *path) {
    size_t n = strlen(path);
    if (n < 9 || n > 1023 || path[0] != '/' || fm_drive_of(path) < 0 || path[n - 1] == '/')
        return fm_is_drive_root(path); /* a root may end with / and has no segments to check */
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)path[i] < 0x20)
            return 0;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len == 0)
            return 0; /* unreachable: the "while (*p == '/') p++" above already skipped any
                       * slash run before s, so a segment here is never empty; defensive only. */
        if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
            return 0;
        /* this is what rejects "//": do not simplify away. s[-1] and s[-2] both being '/'
         * means the slash run just skipped had length >= 2. */
        if (s > path && s[-1] == '/' && s - 1 > path && s[-2] == '/')
            return 0;
    }
    return 1;
}

int fm_is_drive_root(const char *path) {
    char t[16];
    size_t n = strlen(path);
    if (n == 10 && path[9] == '/')
        n = 9;
    if (n != 9 || n >= sizeof(t))
        return 0;
    memcpy(t, path, n);
    t[n] = 0;
    return fm_drive_of(t) >= 0;
}

int fm_name_ok(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > 240)
        return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".trash"))
        return 0; /* ".trash" is our own Trash bin's name (see TRASH_DIR_NAME in main.c):
                   * a folder made with that name would be indistinguishable from it and,
                   * at a drive root, become it. */
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x20 || name[i] == '/')
            return 0;
    return 1;
}

int fm_covers(const char *dir, const char *path) {
    size_t n = strlen(dir);
    return strncmp(dir, path, n) == 0 && (path[n] == 0 || path[n] == '/');
}
