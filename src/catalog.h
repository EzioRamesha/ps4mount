/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Catalog of the PS4 pkgs on a drive, wherever they are, grouped per game. */
#pragma once

#include <limits.h>
#include <stdint.h>

#define CAT_MAX_DLC 64
#define CAT_MAX_SERVICES 7
#define CAT_MAX_DEPTH 16

enum { CAT_UNREAD, CAT_BAD, CAT_OK };

/* One .pkg file. Its details are read once and kept while its size and time stay the same. */
typedef struct {
    char path[PATH_MAX];
    uint64_t size;
    int64_t mtime;
    int state;                            /* CAT_UNREAD, CAT_BAD (not a fake PS4 pkg) or CAT_OK */
    char category[4];                     /* gd game, gp update, ac add-on */
    char title_id[10], content_id[37], app_ver[8];
    char services[CAT_MAX_SERVICES][20];  /* add-on content ID prefixes a game or update lists */
    int nservices;
    int seen;                             /* found by the current walk */
} cat_pkg_t;

/* A game with its update and add-ons: indexes in catalog_t.pkgs. */
typedef struct {
    char title_id[10];
    int app, patch; /* patch is -1 without an update */
    int dlc[CAT_MAX_DLC], ndlc;
} cat_game_t;

typedef struct {
    cat_pkg_t *pkgs; /* sorted by path */
    int npkgs, cap;
    cat_game_t *games;
    int ngames, gcap;
    int dupes;   /* game pkgs not used: an earlier copy of the same game is used */
    int orphans; /* add-ons whose game is not on the drive */
} catalog_t;

/* Returns 1 for a pkg the walk must leave out (path is absolute). */
typedef int (*cat_skip_fn)(const char *path, uint64_t size);

/* Finds every .pkg under root (any depth up to CAT_MAX_DEPTH), skipping hidden and system
 * folders, the old updates PS4Mount keeps in ps4/<TID>/replaced and the nignore folders of
 * ignore (paths below root, any case). Pkgs that are gone are dropped; new or changed ones
 * become CAT_UNREAD. Returns how many are unread. */
int catalog_walk(catalog_t *c, const char *root, cat_skip_fn skip, const char *const *ignore, int nignore);
/* Reads the details of the unread pkgs. Returns how many were read. */
int catalog_read(catalog_t *c);
/* Groups the readable pkgs into games: the first game pkg of a title (by path), its update
 * with the highest APP_VER, and its add-ons (same title ID, or listed by the game or update
 * in SERVICE_ID_ADDCONT_ADD_n, as add-ons of another region). */
void catalog_group(catalog_t *c);
const cat_game_t *catalog_game(const catalog_t *c, const char *title_id);
/* Saves what was read of each pkg, so a restart does not read them all again. 0 on success. */
int catalog_save(const catalog_t *c, const char *path);
/* Replaces c with a saved catalog (pkgs only: group it again). The next walk keeps the
 * pkgs whose size and time did not change. Returns how many were loaded, -1 without a file. */
int catalog_load(catalog_t *c, const char *path);
void catalog_free(catalog_t *c);
