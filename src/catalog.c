/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Catalog of the PS4 pkgs on a drive, wherever they are, grouped per game. */
#include "catalog.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

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

typedef struct {
    catalog_t *c;
    int sorted; /* the first entries of c, known before this walk */
    const char *root;
    size_t rootlen;
    cat_skip_fn skip;
    const char *const *ignore;
    int nignore;
} walk_t;

static int ignored(const walk_t *w, const char *rel) {
    for (int i = 0; i < w->nignore; i++)
        if (!strcasecmp(w->ignore[i], rel))
            return 1;
    return 0;
}

static void walk(const walk_t *w, const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d)
        return;
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
        const char *rel = path + w->rootlen + 1;
        if (is_dir) {
            if (depth < CAT_MAX_DEPTH && !skip_dir(e->d_name, rel) && !ignored(w, rel))
                walk(w, path, depth + 1);
        } else if (is_reg && has_pkg_ext(e->d_name) && !stat(path, &st) &&
                   !(w->skip && w->skip(path, (uint64_t)st.st_size))) {
            if (found(w->c, w->sorted, path, &st))
                break;
        }
    }
    closedir(d);
}

int catalog_walk(catalog_t *c, const char *root, cat_skip_fn skip, const char *const *ignore, int nignore) {
    for (int i = 0; i < c->npkgs; i++)
        c->pkgs[i].seen = 0;
    walk_t w = {c, c->npkgs, root, strlen(root), skip, ignore, nignore};
    walk(&w, root, 0);
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

#define PKG_MAGIC 0x7f434e54u
#define PKG_SIZE_AT 0x430

static uint64_t be64_at(const uint8_t *b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | b[i];
    return v;
}

/* 1 when the file is a PS4 pkg shorter than its header says (or too short to have a header);
 * full gets the expected size, 0 when unknown. */
static int is_short(const char *path, uint64_t size, uint64_t *full) {
    *full = 0;
    if (size == 0)
        return 1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t h[PKG_SIZE_AT + 8];
    ssize_t n = pread(fd, h, sizeof(h), 0);
    close(fd);
    static const uint8_t magic[4] = {PKG_MAGIC >> 24, (PKG_MAGIC >> 16) & 0xff, (PKG_MAGIC >> 8) & 0xff,
                                     PKG_MAGIC & 0xff};
    if (n < 0 || memcmp(h, magic, (size_t)n < sizeof(magic) ? (size_t)n : sizeof(magic)))
        return 0;
    if ((size_t)n < sizeof(magic))
        return 1;
    if ((size_t)n < sizeof(h))
        return 1;
    *full = be64_at(h + PKG_SIZE_AT);
    return *full > size;
}

static int read_one(cat_pkg_t *p, pkg_t *pkg, sfo_t *sfo) {
    if (is_short(p->path, p->size, &p->full_size))
        return 1;
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
            int rc = read_one(p, pkg, sfo);
            p->state = rc > 0 ? CAT_SHORT : rc ? CAT_BAD : CAT_OK;
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

/* The update of g to use: the chosen version when it is there, else the highest. */
static int pick_update(const catalog_t *c, const cat_game_t *g, cat_choose_fn choose) {
    const char *want = choose ? choose(g->title_id) : NULL;
    if (want && !strcmp(want, CAT_NO_UPDATE))
        return -1;
    int best = -1;
    for (int k = 0; k < g->nupdates; k++) {
        const cat_pkg_t *u = &c->pkgs[g->updates[k]];
        if (want && !strcmp(u->app_ver, want))
            return g->updates[k];
        if (best < 0 || strcmp(u->app_ver, c->pkgs[best].app_ver) > 0)
            best = g->updates[k];
    }
    return best;
}

void catalog_group(catalog_t *c, cat_choose_fn choose) {
    c->ngames = c->dupes = c->orphans = 0;
    for (int i = 0; i < c->npkgs; i++)
        c->pkgs[i].role = CAT_ROLE_NONE;
    for (int i = 0; i < c->npkgs; i++) {
        if (!is_kind(&c->pkgs[i], "gd"))
            continue;
        if (game_find(c, c->pkgs[i].title_id)) {
            c->pkgs[i].role = CAT_ROLE_DUPE;
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
        c->pkgs[i].role = CAT_ROLE_APP;
    }
    for (int i = 0; i < c->npkgs; i++) {
        cat_pkg_t *p = &c->pkgs[i];
        if (!is_kind(p, "gp"))
            continue;
        cat_game_t *g = game_find(c, p->title_id);
        p->role = g ? CAT_ROLE_SPARE : CAT_ROLE_NOGAME;
        if (g && g->nupdates < CAT_MAX_UPDATES)
            g->updates[g->nupdates++] = i;
    }
    for (int k = 0; k < c->ngames; k++) {
        cat_game_t *g = &c->games[k];
        g->patch = pick_update(c, g, choose);
        if (g->patch >= 0)
            c->pkgs[g->patch].role = CAT_ROLE_PATCH;
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
        if (!g) {
            c->orphans++;
            c->pkgs[i].role = CAT_ROLE_ORPHAN;
        } else if (g->ndlc < CAT_MAX_DLC) {
            g->dlc[g->ndlc++] = i;
            c->pkgs[i].role = CAT_ROLE_DLC;
        }
    }
}

const cat_game_t *catalog_game(const catalog_t *c, const char *title_id) {
    for (int i = 0; i < c->ngames; i++)
        if (!strcmp(c->games[i].title_id, title_id))
            return &c->games[i];
    return NULL;
}

/* One line per read pkg, fields separated by tabs, the path last:
 * state size mtime category title_id content_id app_ver nservices services... path */
int catalog_save(const catalog_t *c, const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    for (int i = 0; i < c->npkgs; i++) {
        const cat_pkg_t *p = &c->pkgs[i];
        if (p->state == CAT_UNREAD || p->state == CAT_SHORT || strpbrk(p->path, "\t\r\n"))
            continue;
        fprintf(f, "%d\t%llu\t%lld\t%s\t%s\t%s\t%s\t%d", p->state, (unsigned long long)p->size,
                (long long)p->mtime, p->category[0] ? p->category : "-", p->title_id[0] ? p->title_id : "-",
                p->content_id[0] ? p->content_id : "-", p->app_ver[0] ? p->app_ver : "-", p->nservices);
        for (int k = 0; k < p->nservices; k++)
            fprintf(f, "\t%s", p->services[k]);
        fprintf(f, "\t%s\n", p->path);
    }
    int rc = ferror(f) ? -1 : 0;
    if (fclose(f))
        rc = -1;
    if (!rc && rename(tmp, path))
        rc = -1;
    if (rc)
        unlink(tmp);
    return rc;
}

static void field(char *out, size_t n, const char *s) { snprintf(out, n, "%s", strcmp(s, "-") ? s : ""); }

/* Parses one saved line into p. 0 when it is complete. */
static int parse_line(char *line, cat_pkg_t *p) {
    char *f[8 + CAT_MAX_SERVICES + 1];
    int nf = 0;
    for (char *s = line; nf < (int)(sizeof(f) / sizeof(f[0]));) {
        f[nf++] = s;
        char *t = strchr(s, '\t');
        if (!t)
            break;
        *t = 0;
        s = t + 1;
    }
    if (nf < 9)
        return -1;
    int ns = atoi(f[7]);
    if (ns < 0 || ns > CAT_MAX_SERVICES || nf != 9 + ns)
        return -1;
    memset(p, 0, sizeof(*p));
    p->state = atoi(f[0]);
    if (p->state != CAT_OK && p->state != CAT_BAD)
        return -1;
    p->size = strtoull(f[1], NULL, 10);
    p->mtime = strtoll(f[2], NULL, 10);
    field(p->category, sizeof(p->category), f[3]);
    field(p->title_id, sizeof(p->title_id), f[4]);
    field(p->content_id, sizeof(p->content_id), f[5]);
    field(p->app_ver, sizeof(p->app_ver), f[6]);
    for (int k = 0; k < ns; k++)
        snprintf(p->services[k], sizeof(p->services[k]), "%s", f[8 + k]);
    p->nservices = ns;
    if (f[8 + ns][0] != '/')
        return -1;
    snprintf(p->path, sizeof(p->path), "%s", f[8 + ns]);
    return 0;
}

int catalog_load(catalog_t *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    catalog_free(c);
    char *line = malloc(PATH_MAX + 512);
    cat_pkg_t p;
    while (line && fgets(line, PATH_MAX + 512, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (parse_line(line, &p))
            continue;
        if (c->npkgs == c->cap) {
            int cap = c->cap ? c->cap * 2 : 64;
            cat_pkg_t *n = realloc(c->pkgs, (size_t)cap * sizeof(*n));
            if (!n)
                break;
            c->pkgs = n;
            c->cap = cap;
        }
        c->pkgs[c->npkgs++] = p;
    }
    free(line);
    fclose(f);
    if (c->npkgs > 1)
        qsort(c->pkgs, (size_t)c->npkgs, sizeof(*c->pkgs), by_path);
    /* a path saved twice: keep one */
    int n = 0;
    for (int i = 0; i < c->npkgs; i++)
        if (!n || strcmp(c->pkgs[n - 1].path, c->pkgs[i].path))
            c->pkgs[n++] = c->pkgs[i];
    c->npkgs = n;
    return n;
}

void catalog_free(catalog_t *c) {
    free(c->pkgs);
    free(c->games);
    memset(c, 0, sizeof(*c));
}
