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
/* Lets games be added again after a database corruption report paused it. 0 on success. */
int app_resume(char *msg, size_t cap);
void app_stop(void);
/* Closes the open game and moves the pkgs of the games in tids (comma separated title ids)
 * or of all games (NULL) back to where they came from. */
int app_restore(const char *tids, char *msg, size_t cap);
/* Forgets the pkgs moved back, so the scan picks them up again. */
int app_restored_forget(char *msg, size_t cap);
void app_config_json(buf_t *out);
/* body: form-encoded settings; 0 when saved, msg says why not. */
int app_config_set(const char *body, char *msg, size_t cap);
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

/* Wrong PINs in a row before every /api/ request is refused for PIN_LOCK_SECONDS. */
#define PIN_MAX_FAILS 5
#define PIN_LOCK_SECONDS 60

int web_listen(void);
/* Serves one connection on the listening socket. */
void web_serve(int listen_fd);
/* Answers the request on a connected socket and closes it. */
void web_handle(int fd);
