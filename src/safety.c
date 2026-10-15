/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What keeps PS4Mount from harming the console. */
#include "safety.h"

#include <stdio.h>
#include <string.h>

int safety_db_corrupt(const char *line, char *db, size_t n) {
    if (n)
        db[0] = 0;
    /* the system spells it "Occured"; both spellings are accepted */
    const char *p = strstr(line, "DbCorruptionOccured");
    if (!p)
        p = strstr(line, "DbCorruptionOccurred");
    if (!p)
        return 0;
    const char *s = strchr(p, '/');
    if (s && n) {
        size_t l = strcspn(s, " \t:,\r\n");
        if (l > 3 && !strncmp(s + l - 3, ".db", 3))
            snprintf(db, n, "%.*s", (int)l, s);
    }
    return 1;
}

static retry_t *find(const retry_table_t *t, const char *tid) {
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->items[i].tid, tid))
            return &t->items[i];
    return NULL;
}

int retry_waiting(const retry_table_t *t, const char *tid, time_t now) {
    const retry_t *r = find(t, tid);
    return r && (r->tries >= RETRY_MAX_TRIES || now < r->until);
}

int retry_given_up(const retry_table_t *t, const char *tid) {
    const retry_t *r = find(t, tid);
    return r && r->tries >= RETRY_MAX_TRIES;
}

int retry_failed(retry_table_t *t, const char *tid, time_t now) {
    retry_t *r = find(t, tid);
    for (int i = 0; !r && i < t->n; i++)
        if (!t->items[i].tid[0]) {
            r = &t->items[i];
            snprintf(r->tid, sizeof(r->tid), "%s", tid);
        }
    if (!r)
        return 0;
    r->tries++;
    r->until = now + RETRY_SECONDS;
    return r->tries;
}

void retry_clear(retry_table_t *t, const char *tid) {
    retry_t *r = find(t, tid);
    if (r)
        memset(r, 0, sizeof(*r));
}

void retry_clear_given_up(retry_table_t *t) {
    for (int i = 0; i < t->n; i++)
        if (t->items[i].tries >= RETRY_MAX_TRIES)
            memset(&t->items[i], 0, sizeof(t->items[i]));
}
