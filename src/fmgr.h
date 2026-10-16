/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Background copy/move engine for the panel's file manager. Console-only: it uses pthreads
 * and makes real filesystem syscalls on real user data (multi-gigabyte game files), so there
 * is no host test for it (see fm.[ch] for the pure, host-tested path/name rules this module
 * builds on). One task runs at a time, daemon-wide; the main loop starts it, polls
 * fmgr_status() for the panel, and can ask it to stop with fmgr_cancel(). */
#pragma once

#include <stddef.h>

/* What a background file operation is doing. One task runs at a time, daemon-wide. */
enum { FMGR_IDLE = 0, FMGR_RUNNING, FMGR_DONE, FMGR_FAILED, FMGR_CANCELLED };
/* What to do when the destination already has an item with that name. */
enum { FMGR_ASK = 0, FMGR_OVERWRITE, FMGR_SKIP, FMGR_KEEP_BOTH };

typedef struct {
    int state;                 /* FMGR_* above */
    int copy;                  /* 1 copy, 0 move */
    int conflict;              /* FMGR_* policy chosen for this task */
    char what[256];            /* the item being worked on right now, for the panel */
    int done_items, total_items;
    unsigned long long done_bytes, total_bytes;
    unsigned long long bytes_per_sec;
    long eta_seconds;
    char msg[256];             /* why it failed, or what it finished */
} fmgr_status_t;

#define FMGR_MAX_SRCS 256
/* Starts a copy (copy=1) or move (copy=0) of n srcs into dst. dst_free is the destination
 * drive's free bytes, measured by the caller (fmgr.c deliberately knows nothing about
 * drives or mounts). Returns 0 when the task started, -1 when one is already running or
 * the request is refused (msg says why). */
int fmgr_start(char (*srcs)[1024], int n, const char *dst, int copy, int conflict,
               unsigned long long dst_free, char *msg, size_t cap);
/* Copies the current status for the panel. Safe to call from the main loop at any time. */
void fmgr_status(fmgr_status_t *out);
/* Asks the running task to stop. It stops at the next file boundary. */
void fmgr_cancel(void);
/* 1 while a task is running: the daemon refuses other file changes meanwhile. */
int fmgr_busy(void);
