/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What to do with each game, and the state files of the user's choices. */
#include "policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int policy_decide(const game_facts_t *f, const char **why) {
    *why = "";
    if (f->internal)
        *why = "installed on the console";
    else if (f->removed)
        *why = "removed from the library";
    else if (f->excluded)
        *why = "set to never be added";
    else if (f->elsewhere)
        *why = "served from another drive";
    if (**why)
        return POLICY_SKIP;
    if (f->registered && f->installed)
        return POLICY_KEEP;
    if (f->halted)
        *why = POLICY_WHY_PAUSED;
    else if (f->retry_wait)
        *why = "failed recently";
    return **why ? POLICY_SKIP : POLICY_ADD;
}

const char *policy_panel_state(const panel_facts_t *f) {
    if (f->internal)
        return "internal";
    if (f->elsewhere)
        return "duplicate";
    if (f->excluded)
        return "excluded";
    if (f->removed)
        return "removed";
    if (f->running)
        return "running";
    if (f->mounted && f->registered)
        return "ready";
    return f->given_up ? "failed" : "pending";
}

#define KV_LINE 1024

/* Splits a line into key and value ("1" for a key alone). 0 for a usable line. */
static int kv_split(char *line, char **key, char **value) {
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0])
        return -1;
    char *tab = strchr(line, '\t');
    *key = line;
    *value = "1";
    if (tab) {
        *tab = 0;
        *value = tab + 1;
    }
    return 0;
}

int kv_each(const char *path, int (*fn)(const char *key, const char *value, void *arg), void *arg) {
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[KV_LINE], *k, *v;
    while (fgets(line, sizeof(line), f))
        if (!kv_split(line, &k, &v) && fn(k, v, arg))
            break;
    fclose(f);
    return 0;
}

typedef struct {
    const char *key;
    char *value;
    size_t n;
    int found;
} kv_find_t;

static int kv_find(const char *key, const char *value, void *arg) {
    kv_find_t *q = arg;
    if (strcmp(key, q->key))
        return 0;
    snprintf(q->value, q->n, "%s", value);
    q->found = 1;
    return 0; /* the last line for a key wins */
}

int kv_get(const char *path, const char *key, char *value, size_t n) {
    kv_find_t q = {key, value, n, 0};
    if (kv_each(path, kv_find, &q) || !q.found)
        return -1;
    return 0;
}

int kv_set(const char *path, const char *key, const char *value) {
    if (!key[0] || strpbrk(key, "\t\r\n") || (value && strpbrk(value, "\t\r\n")))
        return -1;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out)
        return -1;
    FILE *in = fopen(path, "r");
    char line[KV_LINE], *k, *v;
    while (in && fgets(line, sizeof(line), in))
        if (!kv_split(line, &k, &v) && strcmp(k, key))
            fprintf(out, "%s\t%s\n", k, v);
    if (in)
        fclose(in);
    if (value)
        fprintf(out, "%s\t%s\n", key, value);
    int rc = ferror(out) ? -1 : 0;
    if (fclose(out))
        rc = -1;
    if (!rc && rename(tmp, path))
        rc = -1;
    if (rc)
        unlink(tmp);
    return rc;
}
