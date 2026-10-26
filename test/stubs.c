/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The app_* calls of the web panel, recorded instead of done, for the host tests. */
#include <stdio.h>
#include <string.h>

#include "stubs.h"
#include "web.h"

char g_stub_call[64], g_stub_arg[4096];
int g_stub_calls;

static int record(const char *call, const char *arg, char *msg, size_t cap) {
    snprintf(g_stub_call, sizeof(g_stub_call), "%s", call);
    snprintf(g_stub_arg, sizeof(g_stub_arg), "%s", arg ? arg : "(null)");
    g_stub_calls++;
    snprintf(msg, cap, "done \"%s\"", call);
    return 0;
}

void app_games_json(buf_t *out) { buf_printf(out, "{\"version\":\"test\",\"games\":[]}"); }
int app_add(const char *tid, char *msg, size_t cap) { return record("add", tid, msg, cap); }
int app_remove(const char *tid, char *msg, size_t cap) { return record("remove", tid, msg, cap); }
int app_exclude(const char *tid, int on, char *msg, size_t cap) {
    return record(on ? "exclude" : "include", tid, msg, cap);
}
int app_rescan(const char *drive, char *msg, size_t cap) { return record("rescan", drive, msg, cap); }
int app_speedtest(const char *drive, char *msg, size_t cap) { return record("speedtest", drive, msg, cap); }
int app_fm_list(const char *path, buf_t *out, char *msg, size_t cap) {
    char m[8];
    (void)msg;
    (void)cap;
    record("fm_list", path, m, sizeof(m));
    buf_printf(out, "{\"path\":\"%s\",\"entries\":[]}", path);
    return 0;
}
int app_fm_mkdir(const char *dir, const char *name, char *msg, size_t cap) {
    char arg[2048];
    snprintf(arg, sizeof(arg), "%s + %s", dir, name);
    return record("fm_mkdir", arg, msg, cap);
}
int app_fm_rename(const char *path, const char *name, char *msg, size_t cap) {
    char arg[2048];
    snprintf(arg, sizeof(arg), "%s -> %s", path, name);
    return record("fm_rename", arg, msg, cap);
}
int app_fm_delete(const char *path, char *msg, size_t cap) { return record("fm_delete", path, msg, cap); }
int app_fm_move(const char *src, const char *dst, char *msg, size_t cap) {
    char arg[4096];
    snprintf(arg, sizeof(arg), "%s -> %s", src, dst);
    return record("fm_move", arg, msg, cap);
}
void app_fm_task_json(buf_t *out) { buf_printf(out, "{\"state\":\"idle\"}"); }
int app_fm_copy(const char *srcs, const char *dst, int copy, int conflict, char *msg, size_t cap) {
    char arg[4096];
    snprintf(arg, sizeof(arg), "%s -> %s copy=%d conflict=%d", srcs, dst, copy, conflict);
    return record("fm_copy", arg, msg, cap);
}
int app_fm_cancel(char *msg, size_t cap) { return record("fm_cancel", NULL, msg, cap); }
int app_resume(char *msg, size_t cap) { return record("resume", NULL, msg, cap); }
void app_stop(void) {
    char m[8];
    record("stop", NULL, m, sizeof(m));
}
int app_restore(const char *tids, char *msg, size_t cap) { return record("restore", tids, msg, cap); }
int app_restored_forget(char *msg, size_t cap) { return record("forget", NULL, msg, cap); }
void app_config_json(buf_t *out) { buf_printf(out, "{\"batch\":true}"); }
int app_config_set(const char *body, char *msg, size_t cap) { return record("config", body, msg, cap); }
int app_icon(const char *tid, buf_t *out) {
    char m[8];
    record("icon", tid, m, sizeof(m));
    buf_put(out, "PNG", 3);
    return 0;
}
void app_problems_json(buf_t *out) { buf_printf(out, "{\"problems\":[]}"); }
int app_set_update(const char *tid, const char *ver, char *msg, size_t cap) {
    char arg[64];
    snprintf(arg, sizeof(arg), "%s=[%s]", tid, ver);
    return record("update", arg, msg, cap);
}
int app_addon(const char *content_id, int on, char *msg, size_t cap) {
    char arg[64];
    snprintf(arg, sizeof(arg), "%s=%d", content_id, on);
    return record("addon", arg, msg, cap);
}
void app_diag(buf_t *out) { buf_printf(out, "diag\n"); }
const char *app_pin(void) { return ""; }
const char *app_lang(void) { return ""; }
int app_game_json(const char *tid, buf_t *out, char *msg, size_t cap) {
    record("game", tid, msg, cap);
    buf_printf(out, "{\"title_id\":\"%s\",\"updates\":[],\"dlcs\":[]}", tid);
    return 0;
}
int app_choose_update(const char *tid, const char *rel, char *msg, size_t cap) {
    char arg[256];
    snprintf(arg, sizeof(arg), "%s %s", tid, rel);
    return record("update", arg, msg, cap);
}
int app_choose_dlc(const char *tid, const char *on, char *msg, size_t cap) {
    char arg[4096];
    snprintf(arg, sizeof(arg), "%s %s", tid, on);
    return record("dlc", arg, msg, cap);
}
int app_launch(const char *tid, int force, char *msg, size_t cap) {
    char arg[32];
    snprintf(arg, sizeof(arg), "%s %d", tid, force);
    record("launch", arg, msg, cap);
    if (!strcmp(tid, "CUSA00002") && !force) {
        snprintf(msg, cap, "Uncharted \"4\"");
        return 1;
    }
    return 0;
}
