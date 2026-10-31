/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What to do with each game, from facts the daemon gathers, and the small state files
 * the user's choices are kept in. No system calls: tested on the host. */
#pragma once

#include <stddef.h>

/* What the daemon knows about a game found on a drive. */
typedef struct {
    int internal;   /* a normal installation of the title exists */
    int removed;    /* deleted from the library on the console */
    int excluded;   /* the user chose never to add it */
    int elsewhere;  /* another copy on another drive is the one used */
    int halted;     /* adding games is paused (database corruption report) */
    int retry_wait; /* failed recently, or given up */
    int registered; /* in the system database */
    int installed;  /* PlayGo record, marker, internal folders and add-ons are as expected */
} game_facts_t;

enum { POLICY_SKIP = -1, POLICY_KEEP = 0, POLICY_ADD = 1 };
#define POLICY_WHY_PAUSED "adding games is paused"

/* POLICY_ADD: (re)register, POLICY_KEEP: registered, keep it mounted, POLICY_SKIP: leave it alone.
 * why gets a short reason for a skip ("" otherwise). */
int policy_decide(const game_facts_t *f, const char **why);

/* What the panel shows. */
typedef struct {
    int internal, elsewhere, removed, excluded, running, mounted, registered, given_up, retrying;
    int installing_addons; /* registered and mounted, but its add-ons are still being copied
                             * to internal storage */
} panel_facts_t;

/* "internal", "duplicate", "excluded", "removed", "running", "addons" (registered, its
 * add-ons are still copying), "ready", "failed", "retrying" (failed, waiting to be tried
 * again) or "pending". */
const char *policy_panel_state(const panel_facts_t *f);

/* A drive is low on space under SPACE_LOW_PERCENT of its size or SPACE_LOW_BYTES free. */
#define SPACE_LOW_PERCENT 5
#define SPACE_LOW_BYTES (2ULL << 30)
int policy_space_low(unsigned long long free_bytes, unsigned long long total_bytes);

/* Read speed (MB/s) of a drive against its best earlier result (0: none yet).
 * SPEED_DROP: under half of a best of at least SPEED_FAST_MBPS (cable or port gone bad).
 * SPEED_USB2: first result, under SPEED_USB2_MBPS (fine for a USB stick, low for an SSD).
 * SPEED_CACHED: SPEED_CACHE_MBPS or more, read from the system cache, not the drive. */
#define SPEED_FAST_MBPS 80
#define SPEED_USB2_MBPS 45
#define SPEED_CACHE_MBPS 2000
enum { SPEED_OK, SPEED_DROP, SPEED_USB2, SPEED_CACHED };
int policy_speed_verdict(double mbps, double best);

/* 1 when n_used has already reached max: no free slot for one more managed title. */
int policy_at_capacity(int n_used, int max);

/* How many scan rounds to let pass before the next one, once idle_in_a_row consecutive
 * scans found nothing new anywhere: 1 (no backoff) for 0 or 1, doubling after that, capped
 * at SCAN_BACKOFF_MAX so a newly plugged drive or added pkg is still found within
 * SCAN_BACKOFF_MAX * the configured interval -- as long as it is a newly plugged drive
 * (always walked immediately, see the !seen[] override in scan_all) or the drive's own
 * walk is due too. Combined with the per-drive backoff (policy_drive_walk_due), the
 * effective worst case for a pkg appearing on an already-connected, idle drive is bounded
 * by SCAN_BACKOFF_MAX * the configured interval, not SCAN_BACKOFF_MAX * DRIVE_IDLE_ROUNDS:
 * once the global backoff is fully elevated, every drive gets a full walk on each call
 * (policy_drive_walk_due_combined), so the two backoffs do not multiply. */
#define SCAN_BACKOFF_MAX 6
int policy_scan_backoff(int idle_in_a_row);

/* Whether to re-walk one drive's folders this scan. idle_in_a_row: consecutive scans with
 * nothing new found on this specific drive. speed_drop: its last read-speed test verdict
 * was SPEED_DROP (policy_speed_verdict) -- walked even less often once idle, since repeated
 * full walks add needless wear to an already struggling drive. Stays true for the first few
 * idle rounds either way (DRIVE_IDLE_ROUNDS, or half that for a dropped drive) so a drive
 * that just went idle is still fully responsive, then backs off to walking only every
 * DRIVE_IDLE_ROUNDS-th scan (every 2*DRIVE_IDLE_ROUNDS-th when speed-dropped). */
#define DRIVE_IDLE_ROUNDS 6
int policy_drive_walk_due(int idle_in_a_row, int speed_drop);

/* policy_drive_walk_due, folded together with the global scan backoff so the two do not
 * compound: idle_scans is the global g_idle_scans (consecutive scan_all() calls with
 * nothing new anywhere). Once idle_scans has reached SCAN_BACKOFF_MAX, policy_scan_backoff
 * has already saturated scan_all() itself to its slowest cadence, so every call that does
 * happen gives every eligible drive a real walk instead of this per-drive backoff skipping
 * some of them on top of that. */
int policy_drive_walk_due_combined(int idle_in_a_row, int speed_drop, int idle_scans);

/* State files: one "<key>\t<value>" line per key (a key alone means value "1").
 * kv_get returns 0 and fills value when the key is there; kv_set with value NULL removes
 * the key. Keys and values have no tab or line break. 0 on success. */
int kv_get(const char *path, const char *key, char *value, size_t n);
int kv_set(const char *path, const char *key, const char *value);
/* Calls fn for each line; stops when it returns non-zero. -1 without a file. */
int kv_each(const char *path, int (*fn)(const char *key, const char *value, void *arg), void *arg);
