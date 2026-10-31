/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Web panel interface. */
#pragma once

#include "build.h"

#ifndef WEB_PORT
#define WEB_PORT 8770
#endif

/* Implemented by main.c for the web panel. */
void app_games_json(buf_t *out);
int app_add(const char *tid, char *msg, size_t cap);
int app_remove(const char *tid, char *msg, size_t cap);
/* on: removes the game from the library and never adds it again; off: allows it again. 0 on success. */
int app_exclude(const char *tid, int on, char *msg, size_t cap);
/* Scans every drive now, or only drive ("usb0" ... "ext1") when not NULL. 0 when started. */
int app_rescan(const char *drive, char *msg, size_t cap);
/* Starts a read speed test of a connected drive ("usb0"...). 0 when started. */
int app_speedtest(const char *drive, char *msg, size_t cap);
/* Lists one folder of a drive as JSON rows joined with the catalog. 0 on success. */
int app_fm_list(const char *path, buf_t *out, char *msg, size_t cap);
/* Makes folder name inside dir. 0 on success. */
int app_fm_mkdir(const char *dir, const char *name, char *msg, size_t cap);
/* Renames the file or folder at path to name (same folder). 0 on success. */
int app_fm_rename(const char *path, const char *name, char *msg, size_t cap);
/* Deletes the file, or the folder and everything in it. 0 on success. */
int app_fm_delete(const char *path, char *msg, size_t cap);
/* Moves the file or folder at src into the folder dst on the same drive. 0 on success. */
int app_fm_move(const char *src, const char *dst, char *msg, size_t cap);
/* {"state":"idle|running|done|failed|cancelled","what":"...","done_items":N,"total_items":N,
 *  "done_bytes":N,"total_bytes":N,"speed":N,"eta":N,"msg":"..."} */
void app_fm_task_json(buf_t *out);
/* Starts a background copy or move of one or more items into dst. srcs is one or more paths
 * separated by '\n'. copy: 1 copy, 0 move. conflict: FMGR_ASK=0, FMGR_OVERWRITE=1, FMGR_SKIP=2,
 * FMGR_KEEP_BOTH=3 (see fmgr.h). 0 when the task started. */
int app_fm_copy(const char *srcs, const char *dst, int copy, int conflict, char *msg, size_t cap);
/* Asks the running background copy/move task to stop. 0 on success. */
int app_fm_cancel(char *msg, size_t cap);
/* Lets games be added again after a database corruption report paused it. 0 on success. */
int app_resume(char *msg, size_t cap);
/* Unmounts every game and stops PS4Mount (from the main loop). -1 with the reason in msg
 * when a game is open. */
int app_stop(char *msg, size_t cap);
/* 1 with the name of the open game (the panel says to close it before stopping), else 0. */
int app_stop_open(char *name, size_t cap);
/* Closes the open game and moves the pkgs of the games in tids (comma separated title ids)
 * or of all games (NULL) back to where they came from. */
int app_restore(const char *tids, char *msg, size_t cap);
/* Forgets the pkgs moved back, so the scan picks them up again. */
int app_restored_forget(char *msg, size_t cap);
void app_config_json(buf_t *out);
/* body: form-encoded settings; 0 when saved, msg says why not. */
int app_config_set(const char *body, char *msg, size_t cap);
/* Updates and add-ons of one game, for the panel's dialogs. 0 on success. */
int app_game_json(const char *tid, buf_t *out, char *msg, size_t cap);
/* Puts the update rel ("replaced/<name>") in use; the game is added again. */
int app_choose_update(const char *tid, const char *rel, char *msg, size_t cap);
/* Keeps on only the add-ons in on (content IDs, comma separated); the game is added again. */
int app_choose_dlc(const char *tid, const char *on, char *msg, size_t cap);
/* Starts a ready game on the console (sceSystemServiceLaunchApp; LncUtil lookups do not work
 * on firmware 11.20). 1 when another game is open and force is 0 (msg: its name); with force
 * that game is closed first, without saving, before this one is started. 0 on success. */
int app_launch(const char *tid, int force, char *msg, size_t cap);
/* Fills an icon0.png for tid; 0 on success. */
int app_icon(const char *tid, buf_t *out);
/* {"problems":[{drive, kind, title_id, path}]}: unusable pkgs, duplicates, add-ons and updates without game. */
void app_problems_json(buf_t *out);
/* Update used for tid: an APP_VER, "none", or "" for the highest. 0 on success. */
int app_set_update(const char *tid, const char *ver, char *msg, size_t cap);
/* Turns an add-on (content ID) on or off for its game. 0 on success. */
int app_addon(const char *content_id, int on, char *msg, size_t cap);
/* Plain text report for bug reports: version, firmware, settings, drives, failures, log tail. */
void app_diag(buf_t *out);
/* Panel PIN (4 to 8 digits), "" when the panel is open to everyone on the network. */
const char *app_pin(void);
/* The console's own language as a panel language code ("en"/"es"/"pt"/"fr"), "" when it is
 * not one of them. */
const char *app_lang(void);

/* Wrong PINs in a row before every /api/ request is refused for PIN_LOCK_SECONDS. */
#define PIN_MAX_FAILS 5
#define PIN_LOCK_SECONDS 60

int web_listen(void);
/* Serves one connection on the listening socket. */
void web_serve(int listen_fd);
/* Answers the request on a connected socket and closes it. */
void web_handle(int fd);
