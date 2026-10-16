/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Path and name rules of the panel's file manager. No system calls: tested on the host.
 * Paths from the panel are only ever accepted on a drive (/mnt/usbN or /mnt/extN). */
#pragma once

#include <stddef.h>

/* Drive index (0..7 usb, 8..9 ext) of a path on a drive, -1 when it is not on one. */
int fm_drive_of(const char *path);
/* 1 when path is absolute, on a drive, free of "..", empty segments and control chars. */
int fm_path_ok(const char *path);
/* 1 when path is exactly a drive root (with or without a trailing slash). */
int fm_is_drive_root(const char *path);
/* 1 when name can be used for a new file or folder (one segment, printable, <= 240 bytes). */
int fm_name_ok(const char *name);
/* 1 when path equals dir or lies under dir/ (used to refuse moving a folder into itself). */
int fm_covers(const char *dir, const char *path);

/* How a listing is ordered. Folders always come before files whatever the key is. */
enum { FM_SORT_NAME = 0, FM_SORT_SIZE = 1, FM_SORT_TIME = 2 };

/* One row of a listing, as far as sorting cares about it. */
typedef struct {
    const char *name;
    int dir;
    unsigned long long size;
    long long mtime;
} fm_row_t;

/* Orders a listing: folders first, then by key (name is case-insensitive; size and time
 * are largest/newest first when desc is 1). Returns nothing; sorts in place. */
void fm_sort(fm_row_t *rows, int n, int key, int desc);

/* Builds the name a "keep both" copy should get: "x.pkg" -> "x (2).pkg", "x (2).pkg" ->
 * "x (3).pkg", a name with no dot gets the number before the end. out must hold at least
 * FM_NAME_MAX bytes. Returns 0, or -1 when no free name fits the buffer. */
#define FM_NAME_MAX 256
int fm_keep_both_name(const char *name, int attempt, char *out, size_t cap);
