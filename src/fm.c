/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Path and name rules of the panel's file manager. */
#include "fm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Case-insensitive strcmp without relying on strcasecmp (not every host has it). */
static int fm_ci_cmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (!ca)
            return 0;
        a++;
        b++;
    }
}

/* qsort has no room for extra context in its comparator, so the key and direction that
 * fm_sort is asked for are stashed here while the sort runs (fm_sort is not reentrant). */
static int g_fm_sort_key;
static int g_fm_sort_desc;

/* Orders two rows: folders always first, then by g_fm_sort_key/g_fm_sort_desc, then by
 * name and finally by address so that equal rows never reshuffle between renders. */
static int fm_row_cmp(const void *pa, const void *pb) {
    const fm_row_t *a = (const fm_row_t *)pa;
    const fm_row_t *b = (const fm_row_t *)pb;
    if (a->dir != b->dir)
        return b->dir - a->dir; /* folders (dir = 1) sort before files, no matter desc */

    int c;
    switch (g_fm_sort_key) {
    case FM_SORT_SIZE:
        c = (a->size > b->size) - (a->size < b->size);
        break;
    case FM_SORT_TIME:
        c = (a->mtime > b->mtime) - (a->mtime < b->mtime);
        break;
    default:
        c = fm_ci_cmp(a->name, b->name);
        break;
    }
    if (g_fm_sort_desc)
        c = -c;
    if (c)
        return c;

    c = fm_ci_cmp(a->name, b->name); /* deterministic tie-break, not affected by desc */
    if (c)
        return c;
    return (a > b) - (a < b); /* last resort: still equal, so order by address */
}

void fm_sort(fm_row_t *rows, int n, int key, int desc) {
    g_fm_sort_key = key;
    g_fm_sort_desc = desc;
    qsort(rows, (size_t)n, sizeof(*rows), fm_row_cmp);
}

/* Finds an existing " (N)" already sitting right before base_end (left over from an
 * earlier "keep both" copy) and returns where it starts, so a new attempt replaces it
 * instead of piling up as "x (2) (3)". Returns base_end unchanged when there is none. */
static size_t fm_strip_keep_both_suffix(const char *name, size_t base_end) {
    if (base_end < 4 || name[base_end - 1] != ')')
        return base_end;
    size_t digits_end = base_end - 1; /* index of ')' */
    size_t k = digits_end;
    while (k > 0 && isdigit((unsigned char)name[k - 1]))
        k--;
    if (k == digits_end || k < 2 || name[k - 1] != '(' || name[k - 2] != ' ')
        return base_end; /* no digits, or no " (" right before them */
    return k - 2;
}

/* Builds the "keep both" name for a copy: finds the last '.' that is not the first
 * character (so "a.tar.gz" splits before ".gz", and ".config" never splits), drops any
 * " (N)" a previous attempt already left right before that point, and inserts the new
 * " (N)" there, or at the end when there is no dot to split at. */
int fm_keep_both_name(const char *name, int attempt, char *out, size_t cap) {
    size_t n = strlen(name);
    size_t ext = n; /* start of the extension (the dot itself); n when there is none */
    for (size_t i = n; i > 1; i--) {
        if (name[i - 1] == '.') {
            ext = i - 1;
            break;
        }
    }
    size_t base_end = fm_strip_keep_both_suffix(name, ext); /* where the new suffix goes */

    char suffix[32];
    int sn = snprintf(suffix, sizeof(suffix), " (%d)", attempt);
    if (sn < 0 || (size_t)sn >= sizeof(suffix))
        return -1;

    size_t ext_len = n - ext;
    size_t total = base_end + (size_t)sn + ext_len;
    if (total + 1 > cap)
        return -1;

    memcpy(out, name, base_end);
    memcpy(out + base_end, suffix, (size_t)sn);
    memcpy(out + base_end + (size_t)sn, name + ext, ext_len);
    out[total] = 0;
    return 0;
}
