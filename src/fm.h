/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Path and name rules of the panel's file manager. No system calls: tested on the host.
 * Paths from the panel are only ever accepted on a drive (/mnt/usbN or /mnt/extN). */
#pragma once

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
