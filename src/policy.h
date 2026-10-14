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
} panel_facts_t;

/* "internal", "duplicate", "excluded", "removed", "running", "ready", "failed",
 * "retrying" (failed, waiting to be tried again) or "pending". */
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

/* State files: one "<key>\t<value>" line per key (a key alone means value "1").
 * kv_get returns 0 and fills value when the key is there; kv_set with value NULL removes
 * the key. Keys and values have no tab or line break. 0 on success. */
int kv_get(const char *path, const char *key, char *value, size_t n);
int kv_set(const char *path, const char *key, const char *value);
/* Calls fn for each line; stops when it returns non-zero. -1 without a file. */
int kv_each(const char *path, int (*fn)(const char *key, const char *value, void *arg), void *arg);
