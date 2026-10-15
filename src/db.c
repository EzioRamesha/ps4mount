/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Read-only lookups in the system databases. */
/* The system databases are written by the system alone: ShellCore keeps them open
 * with its own cache, and rows written from outside make it flag them as corrupted. */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sqlite3.h"

#ifndef APP_DB
#define APP_DB "/system_data/priv/mms/app.db"
#endif

/* Titles registered in app.db, read in one query at most once a second. The file is
 * opened without locks (nolock=1): a shared lock held here, even for a moment, can make
 * a write of ShellCore fail, and ShellCore then flags the database as corrupted. */
static char (*g_titles)[10];
static int g_ntitles, g_cap;
static time_t g_read_at;
static int g_read_ok;

static void refresh(void) {
    if (g_read_ok && time(NULL) == g_read_at)
        return;
    g_read_at = time(NULL);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2("file:" APP_DB "?mode=ro&nolock=1", &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return; /* keep the last list */
    }
    sqlite3_stmt *st;
    int n = 0, ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT titleId FROM tbl_contentinfo", -1, &st, NULL) == SQLITE_OK) {
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(st, 0);
            if (!t)
                continue;
            if (n == g_cap) {
                int cap = g_cap ? g_cap * 2 : 256;
                char (*grown)[10] = realloc(g_titles, sizeof(*grown) * (size_t)cap);
                if (!grown)
                    break;
                g_titles = grown;
                g_cap = cap;
            }
            snprintf(g_titles[n++], 10, "%s", t);
        }
        ok = rc == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (ok) { /* a read that met a write in progress keeps the last list */
        g_ntitles = n;
        g_read_ok = 1;
    }
}

#ifndef ADDCONT_DB
#define ADDCONT_DB "/system_data/priv/mms/addcont.db"
#endif

/* Rows of table t that contain needle in a text column. */
static int table_rows(sqlite3 *db, const char *t, const char *needle) {
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\"", t);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    int rows = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int cols = sqlite3_column_count(st), hit = 0;
        for (int c = 0; c < cols && !hit; c++) {
            const char *v = sqlite3_column_type(st, c) == SQLITE_TEXT ? (const char *)sqlite3_column_text(st, c) : NULL;
            hit = v && strstr(v, needle);
        }
        rows += hit;
    }
    sqlite3_finalize(st);
    return rows;
}

int db_addcont_rows(const char *needle, char *tables, size_t n) {
    if (n)
        tables[0] = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2("file:" ADDCONT_DB "?mode=ro&nolock=1", &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *st;
    int total = 0;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        if (!t || strchr(t, '"'))
            continue;
        int rows = table_rows(db, t, needle);
        total += rows;
        size_t l = n ? strlen(tables) : 0;
        if (rows && n && l + strlen(t) + 2 < n)
            snprintf(tables + l, n - l, "%s%s", l ? "," : "", t);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return total;
}

int db_is_registered(const char *title_id) {
    refresh();
    for (int i = 0; i < g_ntitles; i++)
        if (!strcmp(g_titles[i], title_id))
            return 1;
    return 0;
}
