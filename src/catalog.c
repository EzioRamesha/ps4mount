/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Catalog of the PS4 pkgs on a drive, wherever they are, grouped per game. */
#include "catalog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "pkg.h"

static int by_path(const void *a, const void *b) {
    return strcmp(((const cat_pkg_t *)a)->path, ((const cat_pkg_t *)b)->path);
}

static int has_pkg_ext(const char *name) {
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".pkg");
}

/* Folders never searched: hidden ones, the ones Windows keeps for itself, and the
 * old updates PS4Mount keeps in ps4/<TID>/replaced (rel is the path below the root). */
static int skip_dir(const char *name, const char *rel) {
    if (name[0] == '.' || !strcmp(name, "System Volume Information") || !strcasecmp(name, "$RECYCLE.BIN"))
        return 1;
    if (!strncmp(rel, "ps4/", 4)) {
        const char *s = strchr(rel + 4, '/');
        if (s && !strcmp(s, "/replaced"))
            return 1;
    }
    return 0;
}

/* Records a pkg found by the walk. The first `sorted` entries are the ones known before it. */
static int found(catalog_t *c, int sorted, const char *path, const struct stat *st) {
    cat_pkg_t key;
    snprintf(key.path, sizeof(key.path), "%s", path);
    cat_pkg_t *p = sorted ? bsearch(&key, c->pkgs, (size_t)sorted, sizeof(*c->pkgs), by_path) : NULL;
    if (!p) {
        if (c->npkgs == c->cap) {
            int cap = c->cap ? c->cap * 2 : 64;
            cat_pkg_t *n = realloc(c->pkgs, (size_t)cap * sizeof(*n));
            if (!n)
                return -1;
            c->pkgs = n;
            c->cap = cap;
        }
        p = &c->pkgs[c->npkgs++];
        memset(p, 0, sizeof(*p));
        snprintf(p->path, sizeof(p->path), "%s", path);
        p->state = CAT_UNREAD;
    } else if (p->size != (uint64_t)st->st_size || p->mtime != (int64_t)st->st_mtime) {
        char keep[PATH_MAX];
        snprintf(keep, sizeof(keep), "%s", p->path);
        memset(p, 0, sizeof(*p));
        snprintf(p->path, sizeof(p->path), "%s", keep);
        p->state = CAT_UNREAD;
    }
    p->size = (uint64_t)st->st_size;
    p->mtime = (int64_t)st->st_mtime;
    p->seen = 1;
    return 0;
}

static void walk(catalog_t *c, int sorted, const char *root, const char *dir, int depth, cat_skip_fn skip) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    size_t rootlen = strlen(root);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        int is_dir = e->d_type == DT_DIR, is_reg = e->d_type == DT_REG;
        if (e->d_type == DT_UNKNOWN) {
            if (lstat(path, &st))
                continue;
            is_dir = S_ISDIR(st.st_mode);
            is_reg = S_ISREG(st.st_mode);
        }
        if (is_dir) {
            if (depth < CAT_MAX_DEPTH && !skip_dir(e->d_name, path + rootlen + 1))
                walk(c, sorted, root, path, depth + 1, skip);
        } else if (is_reg && has_pkg_ext(e->d_name) && !stat(path, &st) && !(skip && skip(path, (uint64_t)st.st_size))) {
            if (found(c, sorted, path, &st))
                break;
        }
    }
    closedir(d);
}

int catalog_walk(catalog_t *c, const char *root, cat_skip_fn skip) {
    for (int i = 0; i < c->npkgs; i++)
        c->pkgs[i].seen = 0;
    walk(c, c->npkgs, root, root, 0, skip);
    int n = 0, unread = 0;
    for (int i = 0; i < c->npkgs; i++) {
        if (!c->pkgs[i].seen)
            continue;
        if (n != i)
            c->pkgs[n] = c->pkgs[i];
        unread += c->pkgs[n].state == CAT_UNREAD;
        n++;
    }
    c->npkgs = n;
    if (n > 1)
        qsort(c->pkgs, (size_t)n, sizeof(*c->pkgs), by_path);
    c->ngames = 0; /* the games point into pkgs: group again */
    return unread;
}

static int read_one(cat_pkg_t *p, pkg_t *pkg, sfo_t *sfo) {
    if (pkg_open(pkg, p->path))
        return -1;
    int rc = -1;
    size_t len = 0;
    uint8_t *d = NULL;
    if (strncmp(pkg->title_id, "CUSA", 4) || !pkg_fake_key(pkg))
        goto out;
    d = pkg_entry(pkg, 0x1000, &len);
    if (!d || sfo_parse(d, len, sfo))
        goto out;
    const sfo_item_t *cat = sfo_get(sfo, "CATEGORY");
    if (!cat || cat->is_int || (strcmp(cat->sval, "gd") && strcmp(cat->sval, "gp") && strcmp(cat->sval, "ac")))
        goto out;
    snprintf(p->category, sizeof(p->category), "%s", cat->sval);
    snprintf(p->title_id, sizeof(p->title_id), "%s", pkg->title_id);
    snprintf(p->content_id, sizeof(p->content_id), "%s", pkg->content_id);
    const sfo_item_t *av = sfo_get(sfo, "APP_VER");
    p->app_ver[0] = 0;
    if (av && !av->is_int)
        snprintf(p->app_ver, sizeof(p->app_ver), "%s", av->sval);
    p->nservices = 0;
    for (int i = 1; i <= CAT_MAX_SERVICES; i++) {
        char key[32];
        snprintf(key, sizeof(key), "SERVICE_ID_ADDCONT_ADD_%d", i);
        const sfo_item_t *it = sfo_get(sfo, key);
        if (it && !it->is_int && strlen(it->sval) >= 19)
            snprintf(p->services[p->nservices++], sizeof(p->services[0]), "%.19s", it->sval);
    }
    rc = 0;
out:
    free(d);
    pkg_close(pkg);
    return rc;
}

int catalog_read(catalog_t *c) {
    pkg_t *pkg = malloc(sizeof(*pkg));
    sfo_t *sfo = malloc(sizeof(*sfo));
    int n = 0;
    if (pkg && sfo)
        for (int i = 0; i < c->npkgs; i++) {
            cat_pkg_t *p = &c->pkgs[i];
            if (p->state != CAT_UNREAD)
                continue;
            p->state = read_one(p, pkg, sfo) ? CAT_BAD : CAT_OK;
            n++;
        }
    free(pkg);
    free(sfo);
    return n;
}

static cat_game_t *game_find(catalog_t *c, const char *title_id) {
    for (int i = 0; i < c->ngames; i++)
        if (!strcmp(c->games[i].title_id, title_id))
            return &c->games[i];
    return NULL;
}

static int is_kind(const cat_pkg_t *p, const char *kind) { return p->state == CAT_OK && !strcmp(p->category, kind); }

/* 1 when pkg lists the add-on's content ID prefix (UP0000-CUSA00000_00) as one of its services. */
static int serves(const cat_pkg_t *p, const cat_pkg_t *dlc) {
    for (int i = 0; i < p->nservices; i++)
        if (!strncmp(p->services[i], dlc->content_id, 19))
            return 1;
    return 0;
}

void catalog_group(catalog_t *c) {
    c->ngames = c->dupes = c->orphans = 0;
    for (int i = 0; i < c->npkgs; i++) {
        if (!is_kind(&c->pkgs[i], "gd"))
            continue;
        if (game_find(c, c->pkgs[i].title_id)) {
            c->dupes++;
            continue;
        }
        if (c->ngames == c->gcap) {
            int cap = c->gcap ? c->gcap * 2 : 32;
            cat_game_t *n = realloc(c->games, (size_t)cap * sizeof(*n));
            if (!n)
                break;
            c->games = n;
            c->gcap = cap;
        }
        cat_game_t *g = &c->games[c->ngames++];
        memset(g, 0, sizeof(*g));
        snprintf(g->title_id, sizeof(g->title_id), "%s", c->pkgs[i].title_id);
        g->app = i;
        g->patch = -1;
    }
    for (int i = 0; i < c->npkgs; i++) {
        const cat_pkg_t *p = &c->pkgs[i];
        if (!is_kind(p, "gp"))
            continue;
        cat_game_t *g = game_find(c, p->title_id);
        if (g && (g->patch < 0 || strcmp(p->app_ver, c->pkgs[g->patch].app_ver) > 0))
            g->patch = i;
    }
    for (int i = 0; i < c->npkgs; i++) {
        const cat_pkg_t *p = &c->pkgs[i];
        if (!is_kind(p, "ac"))
            continue;
        cat_game_t *g = game_find(c, p->title_id);
        for (int k = 0; !g && k < c->ngames; k++) {
            cat_game_t *o = &c->games[k];
            if (serves(&c->pkgs[o->app], p) || (o->patch >= 0 && serves(&c->pkgs[o->patch], p)))
                g = o;
        }
        if (!g)
            c->orphans++;
        else if (g->ndlc < CAT_MAX_DLC)
            g->dlc[g->ndlc++] = i;
    }
}

const cat_game_t *catalog_game(const catalog_t *c, const char *title_id) {
    for (int i = 0; i < c->ngames; i++)
        if (!strcmp(c->games[i].title_id, title_id))
            return &c->games[i];
    return NULL;
}

void catalog_free(catalog_t *c) {
    free(c->pkgs);
    free(c->games);
    memset(c, 0, sizeof(*c));
}
