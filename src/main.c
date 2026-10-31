/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount daemon: scans USB drives, registers PS4 fpkgs without installing them, follows klog and serves the web panel. */
/* ps4mount: run PS4 fpkgs from external storage without installing them.
 *
 * Every scan:
 *   1. Every folder of /mnt/usbN and /mnt/extN is searched for PS4 pkgs, which stay
 *      where they are; each game is grouped with its update and add-ons. (With the
 *      organize setting on, pkgs are first moved to <root>/ps4/<TITLE_ID>/app.pkg.)
 *   2. For each game the files a real install leaves
 *      behind are generated from the pkg into an internal folder next to a symlink
 *      to the pkg, that folder is mounted over /user/app/<TITLE_ID> (nullfs) and
 *      the title is added to app.db/appinfo.db.
 *   3. Mounts whose storage disappeared are released.
 * Titles installed the normal way are never touched. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "accopy.h"
#include "build.h"
#include "catalog.h"
#include "crypto.h"
#include "db.h"
#include "fm.h"
#include "fmgr.h"
#include "fw.h"
#include "logfile.h"
#include "logts.h"
#include "origin.h"
#include "pkg.h"
#include "policy.h"
#include "safety.h"
#include "speed.h"
#include "web.h"
#include "icon_data.h"

#define VERSION "PS4Mount 0.7.2-test.1"
#define LOCK_PORT 9032
#define SCAN_SECONDS 10
#define STATE_DIR "/data/ps4mount"
/* Internal folders mounted over /user/app/<TID> and /user/patch/<TID>: the files an
 * install leaves there plus app.pkg/patch.pkg as symlinks to the pkgs on the drive.
 * /user is bfs, which has no symlinks; /system_data is ufs. A delete from the
 * console only ever removes these links, never the pkgs. */
#define HOLD_DIR "/system_data/ps4mount"
#define MAX_TITLES 1024
#define MAX_DLC 64
#define DLC_LIMIT 29 /* add-ons a running game can use (see the README) */
#define BATCH_QUIET 10 /* seconds without writes to the system databases between groups */
#define MAX_DLC_TIDS 8

#define MAX_SCAN_PATHS 8
#define CONFIG_PATH STATE_DIR "/config.txt"
#define RESTORED_LIST STATE_DIR "/restored.txt" /* pkgs moved back: not picked up again */
/* Add-ons turned off in the panel (content IDs): left out when their game is added. Shared
 * by the quick toggle (app_addon), the per-game page (app_choose_dlc) and an add-on deleted
 * from the system storage screen. */
#define ADDONS_OFF_FILE STATE_DIR "/addons-off.txt"

/* Settings, kept in CONFIG_PATH and edited in the panel. */
typedef struct {
    int batch;      /* new games are added in groups */
    int batch_size; /* games per group */
    int dlc;        /* add-ons are moved and installed */
    int dlc_copy;   /* add-ons are copied to internal storage by the system, not mounted */
    char paths[MAX_SCAN_PATHS][128]; /* organize: folders whose pkgs are moved, relative to each drive; "" is the root */
    int npaths;
    int organize; /* pkgs are moved into <root>/ps4/<TID>/; off: they are used where they are */
    char ignore[MAX_SCAN_PATHS][128]; /* folders the scan never enters, relative to each drive */
    int nignore;
    int interval;      /* seconds between scans */
    char pin[9];       /* panel PIN, "" for none */
    int cross;         /* updates and add-ons may be on another drive than their game */
    int restore_batch; /* games removed from the library per group when moving pkgs back */
} config_t;

#define MIN_INTERVAL 10
#define MAX_INTERVAL 300
#define MAX_RESTORE_BATCH 10

static config_t g_cfg = {1, 5, 1, 1, {""}, 1, 0, {""}, 0, SCAN_SECONDS, "", 0, 5};

/* Set when the system log reports a corrupted system database: no game is added until
 * the user resumes in the panel. Kept in HALT_FILE across restarts. */
#define HALT_FILE STATE_DIR "/halted.txt"
static char g_halt[256];

/* --- drives and the pkgs found on them --- */

#define NUM_DRIVES 10 /* /mnt/usb0-7, /mnt/ext0-1 */

static catalog_t g_cat[NUM_DRIVES];
static unsigned char g_cat_loaded[NUM_DRIVES]; /* saved catalog read since the drive appeared */

static void drive_root(int i, char *out, size_t n) {
    if (i < 8)
        snprintf(out, n, "/mnt/usb%d", i);
    else
        snprintf(out, n, "/mnt/ext%d", i - 8);
}

static catalog_t *drive_catalog(const char *root) {
    char r[32];
    for (int i = 0; i < NUM_DRIVES; i++) {
        drive_root(i, r, sizeof(r));
        if (!strcmp(r, root))
            return &g_cat[i];
    }
    return NULL;
}

/* Where a game is, as recorded in its marker: <root>/ps4/<TID> for a game PS4Mount
 * organized (as earlier versions recorded every game), the game pkg itself otherwise. */
static void game_location(const char *root, const char *tid, const char *app_path, char *out, size_t n) {
    char lib[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4/%s/app.pkg", root, tid);
    if (!strcmp(app_path, lib))
        snprintf(out, n, "%s/ps4/%s", root, tid);
    else
        snprintf(out, n, "%s", app_path);
}

typedef struct { char useless[45]; char message[3075]; } notify_request_t;
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(uint32_t *user_id);
/* Starting a game uses sceSystemServiceLaunchApp, the call other PS5 homebrew launchers
 * use (ps5-payload-dev/websrv). The context layout is theirs too; LncUtil-based lookups
 * do not work on firmware 11.20, this call does. */
typedef struct {
    uint32_t structsize, user_id, app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} launch_ctx_t;
int sceSystemServiceLaunchApp(const char *title_id, char **argv, launch_ctx_t *ctx);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
/* SCE_SYSTEM_SERVICE_PARAM_ID_LANG = 1; value is Sony's own SCE_SYSTEM_PARAM_LANG_* enum.
 * Used by app_lang() to default the panel's language to the console's own. */
int sceSystemServiceParamGetInt(int param_id, int *value);

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    printf("[notify] %s\n", req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* --- small fs helpers --- */

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    mkdir(tmp, mode);
}

static int write_file(const char *path, const void *data, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}

/* Reads up to cap-1 bytes of path into out, 0-terminated; returns the byte count (0 on
 * failure, which callers treat as "nothing to parse" rather than an error). */
static size_t read_file(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0)
        n = 0;
    out[n] = 0;
    return (size_t)n;
}

#define DOING_FILE STATE_DIR "/doing.txt"
/* Big enough for MAX_TITLES tids of up to 9 chars + a comma each, plus the action line. */
#define DOING_MAX (MAX_TITLES * 11 + 32)

/* Title IDs whose last launch attempt never returned: one tid per key, the usual kv_*
 * file. Cleared by the ordinary act of pressing Play again, same as EXCLUDED_FILE is
 * cleared by un-excluding a game. */
#define LAUNCH_HELD_FILE STATE_DIR "/launch-held.txt"

/* The action currently marked, "" when nothing is. Kept in memory so a nested web_serve
 * on the same thread (pump_events, during install_all's wait loop) cannot let an
 * unrelated operation (e.g. a panel launch) clobber or clear a marker it does not own. */
static char g_doing[16];

/* Marks action (e.g. "adding", "launching") as in progress for tids (comma-separated,
 * no spaces). If a different action is already marked, this operation does not own the
 * marker: the file is left alone and 0 is returned. Otherwise the file is (re)written,
 * g_doing is set to action, and 1 is returned; the caller must pass the same action to
 * doing_clear() once its bracketed operation returns. */
static int doing_mark(const char *action, const char *tids) {
    if (g_doing[0] && strcmp(g_doing, action))
        return 0;
    char body[DOING_MAX];
    snprintf(body, sizeof(body), "%s\n%s\n", action, tids);
    size_t n = strlen(body); /* snprintf's return value can exceed sizeof(body); strlen cannot */
    write_file(DOING_FILE, body, n, 0600);
    snprintf(g_doing, sizeof(g_doing), "%s", action);
    return 1;
}

/* Clears the marker, but only if action is the one this process actually marked (the
 * return of doing_mark was 1 for it) or nothing is marked yet in this process at all: a
 * caller that never owned the marker, because another operation already held it, must
 * not erase that operation's marker. g_doing being unset covers startup recovery, which
 * clears a marker left by a previous run before this process has marked anything. */
static void doing_clear(const char *action) {
    if (g_doing[0] && strcmp(g_doing, action))
        return;
    unlink(DOING_FILE);
    g_doing[0] = 0;
}

/* Reads the marker left by a previous run that did not clear it. action/tids are sized
 * for the same things doing_mark takes. Returns 0 (nothing was in progress) or 1 (action
 * and tids are filled in; the caller must still doing_clear() once it has acted on this). */
static int doing_find(char *action, size_t asz, char *tids, size_t tsz) {
    char buf[DOING_MAX];
    size_t n = read_file(DOING_FILE, buf, sizeof(buf));
    if (!n)
        return 0;
    char *nl = strchr(buf, '\n');
    if (!nl)
        return 0;
    *nl = 0;
    snprintf(action, asz, "%s", buf);
    char *tids_start = nl + 1;
    char *nl2 = strchr(tids_start, '\n');
    if (nl2)
        *nl2 = 0;
    snprintf(tids, tsz, "%s", tids_start);
    return 1;
}

static int mounted_from(const char *mountpoint, char *from, size_t n) {
    struct statfs sf;
    if (statfs(mountpoint, &sf) || strcmp(sf.f_mntonname, mountpoint) || strcmp(sf.f_fstypename, "nullfs"))
        return 0;
    if (from)
        snprintf(from, n, "%s", sf.f_mntfromname);
    return 1;
}

/* Mounts on path from the mount table. Unlike statfs(path) this also sees a layer
 * whose source drive was disconnected (statfs on it fails). */
static int layers_on(const char *path) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT), count = 0;
    if (n <= 0)
        return 0;
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 0;
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].f_mntonname, path))
            count++;
    free(list);
    return count;
}

/* Mounts of the game's package images (/mnt/sandbox/pfsmnt/<TID>-app0 and others). The
 * system releases them a while after the game process ended. */
static int pfs_mounts(const char *tid) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT), count = 0;
    if (n <= 0)
        return 0;
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 0;
    char tag[32];
    snprintf(tag, sizeof(tag), "/pfsmnt/%s-", tid);
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    for (int i = 0; i < n; i++)
        count += strstr(list[i].f_mntonname, tag) != NULL;
    free(list);
    return count;
}

/* A layer is on path but cannot be read: its drive was disconnected (and maybe
 * connected again, which gives it a new mount). */
static int dead_layer(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) && layers_on(path) > 0;
}

static int is_mountpoint(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) == 0 && !strcmp(sf.f_mntonname, path);
}

/* Free and total bytes of the drive at root. 0 on success. */
static int drive_space(const char *root, unsigned long long *free_bytes, unsigned long long *total_bytes) {
    struct statfs sf;
    *free_bytes = *total_bytes = 0;
    if (statfs(root, &sf))
        return -1;
    *free_bytes = (unsigned long long)sf.f_bavail * sf.f_bsize;
    *total_bytes = (unsigned long long)sf.f_blocks * sf.f_bsize;
    return 0;
}

/* --- appmeta entries copied verbatim from the pkg --- */

typedef struct {
    uint32_t id;
    const char *name;
} meta_entry_t;

static const meta_entry_t k_user_meta[] = {
    {0x1200, "icon0.png"}, {0x1220, "pic0.png"},  {0x1006, "pic1.png"},          {0x1240, "snd0.at9"},
    {0x1280, "icon0.dds"}, {0x12a0, "pic0.dds"},  {0x1004, "pronunciation.xml"}, {0x100b, "shareparam.json"},
    {0, NULL},
};

static int write_entry(pkg_t *pkg, uint32_t id, const char *path) {
    size_t len;
    uint8_t *data = pkg_entry(pkg, id, &len);
    if (!data)
        return 1; /* absent entry is fine */
    int rc = write_file(path, data, len, 0777);
    free(data);
    return rc;
}

static int write_buf(const char *path, const buf_t *b) {
    if (b->failed) {
        printf("[write] %s: out of memory while building it, not written\n", path);
        return -1;
    }
    return write_file(path, b->data, b->len, 0777);
}

/* --- registration --- */

/* An add-on pkg, anywhere on the drive of its game. */
typedef struct {
    char path[PATH_MAX];
    pkg_t pkg;
    sfo_t sfo;
    int data; /* 1: has data (mounted by the game), 0: only unlocks content */
} dlc_src_t;

/* Content IDs appear one per line in list (itself wrapped in leading/trailing newlines);
 * used both to read the add-ons the panel submitted for a game and to test membership. */
static int dlc_listed(const char *list, const char *cid) {
    char line[40];
    snprintf(line, sizeof(line), "\n%.36s\n", cid);
    return strstr(list, line) != NULL;
}

/* One game: its pkg plus an optional update and add-ons. */
typedef struct {
    char dir[PATH_MAX]; /* where the game is: see game_location */
    char app_path[PATH_MAX], patch_path[PATH_MAX]; /* patch_path is "" without an update */
    pkg_t app, patch;
    int has_patch;
    sfo_t app_sfo, patch_sfo;
    char name[128], ver[8], marker[PATH_MAX + 160];
    char update_issue[256]; /* why patch.pkg was ignored, empty when used or absent */
    dlc_src_t *dlc; /* add-ons in use, then the ones turned off, up to ndlc_all */
    int ndlc, ndlc_all, dlc_cap;
    char dlc_tids[MAX_DLC_TIDS][10]; /* add-on title IDs: /user/addcont/<TID> folders */
    int ndlc_tids;
} title_src_t;

static int read_sfo(pkg_t *pkg, sfo_t *sfo) {
    size_t len;
    uint8_t *d = pkg_entry(pkg, 0x1000, &len);
    int rc = d ? sfo_parse(d, len, sfo) : -1;
    free(d);
    return rc;
}

static void src_close(title_src_t *t) {
    pkg_close(&t->app);
    if (t->has_patch)
        pkg_close(&t->patch);
    for (int i = 0; i < t->ndlc_all; i++)
        pkg_close(&t->dlc[i].pkg);
    free(t->dlc);
    t->dlc = NULL;
    t->ndlc = t->ndlc_all = t->dlc_cap = 0;
}

static int dlc_cmp(const void *a, const void *b) {
    return strcmp(((const dlc_src_t *)a)->pkg.content_id, ((const dlc_src_t *)b)->pkg.content_id);
}

static int addon_off(const char *content_id);

/* The add-ons the catalog grouped with the game: the ones in use first, then the ones
 * turned off in the panel (/data/ps4mount/addons-off.txt, shared with app_addon and the
 * per-game page's app_choose_dlc), up to ndlc_all, so the per-game page can list and
 * re-enable them. A second copy of an add-on, and one deleted on the console, is left out. */
static void dlc_open(title_src_t *t, const catalog_t *c, const cat_game_t *g) {
    const cat_pkg_t *list[CAT_MAX_DLC];
    int nlist = catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC);
    for (int k = 0; k < nlist && t->ndlc_all < MAX_DLC; k++) {
        const cat_pkg_t *p = list[k];
        int dup = 0;
        for (int i = 0; i < t->ndlc_all; i++)
            dup |= !strcmp(t->dlc[i].pkg.content_id, p->content_id);
        char rm[96];
        snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", p->content_id);
        if (dup || exists(rm)) /* removed: deleted on the console */
            continue;
        if (t->ndlc_all == t->dlc_cap) { /* about 100 KB per add-on: grown as they are found */
            int cap = t->dlc_cap ? t->dlc_cap * 2 : 4;
            if (cap > MAX_DLC)
                cap = MAX_DLC;
            dlc_src_t *grown = realloc(t->dlc, sizeof(*grown) * (size_t)cap);
            if (!grown)
                break;
            t->dlc = grown;
            t->dlc_cap = cap;
        }
        dlc_src_t *a = &t->dlc[t->ndlc_all];
        snprintf(a->path, sizeof(a->path), "%s", p->path);
        /* the catalog checked the category and the fake key when it first read the pkg;
         * checking the key again on every scan would cost an RSA operation per add-on */
        if (pkg_open(&a->pkg, a->path) || read_sfo(&a->pkg, &a->sfo)) {
            if (a->pkg.fd > 0)
                pkg_close(&a->pkg);
            continue;
        }
        a->data = a->pkg.header[0x77] == 0x1b; /* content type: 0x1b with data, 0x1c license only */
        t->ndlc_all++;
    }
    if (!t->ndlc_all)
        return;
    qsort(t->dlc, (size_t)t->ndlc_all, sizeof(*t->dlc), dlc_cmp);
    /* the add-ons turned off go after the ones in use, in the same order within each group */
    dlc_src_t *sorted = malloc(sizeof(*sorted) * (size_t)t->ndlc_all);
    if (!sorted)
        return;
    int on = 0, k = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < t->ndlc_all; i++) {
            int is_off = addon_off(t->dlc[i].pkg.content_id) != 0;
            if (is_off == pass)
                sorted[k++] = t->dlc[i];
            on += !pass && !is_off;
        }
    memcpy(t->dlc, sorted, sizeof(*sorted) * (size_t)t->ndlc_all);
    free(sorted);
    t->ndlc = on;
    for (int i = 0; i < t->ndlc; i++) {
        int known = 0;
        for (int j = 0; j < t->ndlc_tids; j++)
            known |= !strcmp(t->dlc_tids[j], t->dlc[i].pkg.title_id);
        if (!known && t->ndlc_tids < MAX_DLC_TIDS)
            snprintf(t->dlc_tids[t->ndlc_tids++], 10, "%s", t->dlc[i].pkg.title_id);
    }
}

static int first_time(const char *path);

/* Opens the pkgs of game tid as the catalog of the drive at root grouped them. */
static int src_open(title_src_t *t, const char *root, const char *tid) {
    memset(t, 0, sizeof(*t));
    const catalog_t *cat = drive_catalog(root);
    const cat_game_t *g = cat ? catalog_game(cat, tid) : NULL;
    if (!g)
        return -1;
    snprintf(t->app_path, sizeof(t->app_path), "%s", cat->pkgs[g->app].path);
    const cat_pkg_t *patch = catalog_patch(g_cat, cat, g);
    if (patch)
        snprintf(t->patch_path, sizeof(t->patch_path), "%s", patch->path);
    game_location(root, tid, t->app_path, t->dir, sizeof(t->dir));
    if (pkg_open(&t->app, t->app_path) || read_sfo(&t->app, &t->app_sfo)) {
        if (first_time(t->app_path))
            printf("[scan] %s: unreadable app pkg\n", t->app_path);
        pkg_close(&t->app);
        return -1;
    }
    if (strcmp(t->app.title_id, tid)) {
        printf("[scan] %s: the game pkg is for %s\n", t->app_path, t->app.title_id);
        pkg_close(&t->app);
        return -1;
    }
    /* a damaged param.sfo has powered the console off and damaged its storage: never registered */
    char why[64];
    if (sfo_check(&t->app_sfo, why, sizeof(why))) {
        printf("[scan] %s: not added, its param.sfo is damaged (%s)\n", t->app_path, why);
        if (first_time(t->app_path)) {
            const sfo_item_t *title = sfo_get(&t->app_sfo, "TITLE");
            notify("%s: not added, the game package is damaged (%s)", title ? title->sval : tid, why);
        }
        pkg_close(&t->app);
        return -1;
    }
    if (exists(t->patch_path)) {
        const sfo_item_t *gv = sfo_get(&t->app_sfo, "VERSION");
        if (pkg_open(&t->patch, t->patch_path) || read_sfo(&t->patch, &t->patch_sfo)) {
            snprintf(t->update_issue, sizeof(t->update_issue), "Update ignored: the update pkg could not be read");
            if (t->patch.fd > 0)
                pkg_close(&t->patch);
        } else {
            /* An update only fits the title it was made for. (Master VERSION may differ:
             * installed games run with base 01.02 + update 01.00.) */
            const sfo_item_t *uv = sfo_get(&t->patch_sfo, "VERSION");
            const char *gver = gv ? gv->sval : "?", *uver = uv ? uv->sval : "?";
            if (strcmp(t->patch.title_id, tid)) {
                snprintf(t->update_issue, sizeof(t->update_issue),
                         "Update ignored: it is for %s version %s, the game is %s version %s", t->patch.title_id, uver,
                         tid, gver);
                pkg_close(&t->patch);
            } else if (sfo_check(&t->patch_sfo, why, sizeof(why))) {
                snprintf(t->update_issue, sizeof(t->update_issue),
                         "Update ignored: the update package is damaged (%s)", why);
                pkg_close(&t->patch);
            } else {
                t->has_patch = 1;
            }
        }
        if (t->update_issue[0] && first_time(t->patch_path)) {
            const sfo_item_t *title = sfo_get(&t->app_sfo, "TITLE");
            notify("%s: %s", title ? title->sval : tid, t->update_issue);
        }
    }
    const sfo_t *eff = t->has_patch ? &t->patch_sfo : &t->app_sfo;
    const sfo_item_t *it = sfo_get(eff, "TITLE");
    snprintf(t->name, sizeof(t->name), "%s", it ? it->sval : tid);
    it = sfo_get(eff, "APP_VER");
    snprintf(t->ver, sizeof(t->ver), "%s", it && !it->is_int ? it->sval : "01.00");
    char digest[65] = "";
    if (t->has_patch)
        for (int i = 0; i < 32; i++)
            snprintf(digest + i * 2, 3, "%02x", t->patch.digest[i]);
    snprintf(t->marker, sizeof(t->marker), "%s\n%s\n", t->dir, t->has_patch ? digest : "nopatch");
    if (g_cfg.dlc)
        dlc_open(t, cat, g);
    if (t->ndlc) {
        /* one line for all add-ons, so markers of titles without add-ons stay as before */
        uint8_t h[32];
        buf_t all = {0};
        for (int i = 0; i < t->ndlc; i++)
            buf_put(&all, t->dlc[i].pkg.digest, 32);
        sha256(all.data, all.len, h);
        buf_free(&all);
        size_t n = strlen(t->marker);
        snprintf(t->marker + n, sizeof(t->marker) - n, "dlc %d ", t->ndlc);
        for (int i = 0; i < 32; i++) {
            n = strlen(t->marker);
            snprintf(t->marker + n, sizeof(t->marker) - n, "%02x", h[i]);
        }
        n = strlen(t->marker);
        snprintf(t->marker + n, sizeof(t->marker) - n, "\n");
    }
    return 0;
}

static void write_meta(pkg_t *pkg, const char *meta_dir, int *rc) {
    char q[PATH_MAX];
    for (const meta_entry_t *m = k_user_meta; m->name; m++) {
        snprintf(q, sizeof(q), "%s/%s", meta_dir, m->name);
        *rc |= write_entry(pkg, m->id, q) < 0;
    }
    for (uint32_t id = 0x1260; id <= 0x127f; id++) {
        size_t len;
        uint8_t *d = pkg_entry(pkg, id, &len);
        if (!d)
            continue;
        snprintf(q, sizeof(q), "%s/changeinfo", meta_dir);
        mkdirs(q, 0777);
        if (id == 0x1260)
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo.xml", meta_dir);
        else
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo_%02u.xml", meta_dir, id - 0x1261);
        *rc |= write_file(q, d, len, 0777);
        free(d);
    }
}

static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode))
            rm_tree(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static void hold_path(char *out, size_t n, const char *kind, const char *tid) {
    snprintf(out, n, HOLD_DIR "/%s/%s", kind, tid);
}

/* Fresh internal folder for kind ("app"/"patch") with name -> pkg_path. */
static int hold_prepare(const char *kind, const char *tid, const char *name, const char *pkg_path) {
    char dir[PATH_MAX], link[PATH_MAX];
    hold_path(dir, sizeof(dir), kind, tid);
    mkdirs(dir, 0777);
    snprintf(link, sizeof(link), "%s/%s", dir, name);
    unlink(link);
    if (symlink(pkg_path, link)) {
        printf("[reg] symlink %s: %s\n", link, strerror(errno));
        return -1;
    }
    return 0;
}

/* The link of kind exists and reaches its pkg. */
static int hold_ready(const char *kind, const char *tid, const char *name) {
    char p[PATH_MAX];
    hold_path(p, sizeof(p), kind, tid);
    snprintf(p + strlen(p), sizeof(p) - strlen(p), "/%s", name);
    return exists(p);
}

static uint8_t *read_all(const char *path, size_t *len) {
    struct stat st;
    *len = 0;
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > (1 << 20))
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    uint8_t *d = malloc((size_t)st.st_size);
    if (d && read(fd, d, (size_t)st.st_size) != st.st_size) {
        free(d);
        d = NULL;
    }
    close(fd);
    if (d)
        *len = (size_t)st.st_size;
    return d;
}

/* Adds or replaces the license slot of pkg in the rif/idx of its content ID prefix,
 * keeping the slots of the game and of other add-ons. */
static int place_license(pkg_t *pkg) {
    size_t lic_len, rl, il;
    uint8_t *lic = pkg_entry(pkg, 0x400, &lic_len);
    if (!lic || lic_len != 0x400) {
        free(lic);
        printf("[reg] %s: no license.dat\n", pkg->content_id);
        return -1;
    }
    char rp[96], ip[96];
    snprintf(rp, sizeof(rp), "/user/license/fake%.19s.rif", pkg->content_id);
    snprintf(ip, sizeof(ip), "/user/license/fake%.19s.idx", pkg->content_id);
    uint8_t *old_rif = read_all(rp, &rl), *old_idx = read_all(ip, &il);
    buf_t rif = {0}, idx = {0};
    build_license(&rif, &idx, old_rif, rl, old_idx, il, pkg->content_id, lic);
    int rc = write_buf(rp, &rif) | write_buf(ip, &idx);
    free(lic);
    free(old_rif);
    free(old_idx);
    buf_free(&rif);
    buf_free(&idx);
    return rc ? -1 : 0;
}

/* Internal folder of an add-on: HOLD_DIR/addcont/<ADD-ON TID>/<LABEL>, mounted (by
 * title ID) over /user/addcont/<ADD-ON TID>. It holds ac.pkg as a symlink to the pkg on
 * the drive plus ac.json/ac.pbm. Also writes the PlayGo records, the license slot and,
 * for add-ons with data, the icon. */
static int place_dlc(dlc_src_t *a) {
    pkg_t *pkg = &a->pkg;
    const sfo_item_t *v = sfo_get(&a->sfo, "VERSION");
    const char *ver = v && !v->is_int ? v->sval : "01.00", *label = pkg->content_id + 20;
    char dir[PATH_MAX], p[PATH_MAX], q[PATH_MAX];
    snprintf(dir, sizeof(dir), HOLD_DIR "/addcont/%s/%s", pkg->title_id, label);
    mkdirs(dir, 0777);
    snprintf(q, sizeof(q), "%s/ac.pkg", dir);
    unlink(q);
    int rc = 0;
    if (symlink(a->path, q)) {
        printf("[reg] symlink %s: %s\n", q, strerror(errno));
        rc = -1;
    }
    buf_t json = {0}, pbm = {0}, pdb = {0};
    build_app_json(&json, pkg, a->path);
    build_app_pbm(&pbm, pkg, ver, a->data ? PBM_ADDCONT : PBM_LICENSE);
    build_ac_pdb(&pdb, pkg, a->path, &json);
    snprintf(q, sizeof(q), "%s/ac.json", dir);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/ac.pbm", dir);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/ac.pbm.backup", dir);
    rc |= write_buf(q, &pbm);
    snprintf(p, sizeof(p), "/user/playgo/addcont/%s/00", pkg->content_id);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/d0.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/d1.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/f0.pdb", p);
    rc |= write_file(q, "", 0, 0777);
    rc |= place_license(pkg);
    if (a->data) {
        snprintf(p, sizeof(p), "/user/appmeta/addcont/%s/%s", pkg->title_id, label);
        mkdirs(p, 0777);
        snprintf(q, sizeof(q), "%s/icon0.png", p);
        rc |= write_entry(pkg, 0x1200, q) < 0;
    }
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&pdb);
    if (rc)
        printf("[reg] add-on %s: could not write its files\n", pkg->content_id);
    return rc ? -1 : 0;
}

/* An add-on turned off: its internal folder and records go, so the game no longer sees it. */
static void drop_dlc(const pkg_t *pkg) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), HOLD_DIR "/addcont/%s/%s", pkg->title_id, pkg->content_id + 20);
    if (!exists(p))
        return;
    rm_tree(p);
    snprintf(p, sizeof(p), "/user/playgo/addcont/%s", pkg->content_id);
    rm_tree(p);
    snprintf(p, sizeof(p), "/user/appmeta/addcont/%s/%s", pkg->title_id, pkg->content_id + 20);
    rm_tree(p);
    printf("[reg] add-on %s turned off\n", pkg->content_id);
}

/* The ac.pkg link of every add-on exists and reaches its pkg. */
static int dlc_ready(const title_src_t *t) {
    char p[PATH_MAX];
    if (g_cfg.dlc_copy)
        return 1; /* copy mode keeps no links */
    for (int i = 0; i < t->ndlc; i++) {
        snprintf(p, sizeof(p), HOLD_DIR "/addcont/%s/%s/ac.pkg", t->dlc[i].pkg.title_id,
                 t->dlc[i].pkg.content_id + 20);
        if (!exists(p))
            return 0;
    }
    return 1;
}

/* --- add-ons copied to internal storage (copy mode) --- */

/* sceAppInstUtilInstallByPackage arguments: the system copies the pkg at uri into
 * /user/addcont/<ADD-ON TID>/<LABEL>/ and writes the add-on's addcont.db row itself. */
typedef struct {
    const char *uri, *ex_uri, *playgo_scenario_id, *content_id, *content_name, *icon_url;
    uint64_t unknown_30;
    uint64_t reserved[7];
} ac_meta_t;
typedef struct {
    char content_id[0x30];
    int content_type, content_platform;
} ac_pkg_t;
typedef struct {
    char languages[30][8];
    char scenario_ids[64][3];
    char content_ids[64][0x30];
    unsigned char unknown[6480];
} ac_playgo_t;
int sceAppInstUtilInstallByPackage(ac_meta_t *, ac_pkg_t *, ac_playgo_t *);
int sceAppInstUtilAppUnInstallAddcont(const char *title_id, const char *addcont_name);

#define AC_POLL_SECONDS 2
#define AC_NOT_FOUND 0x80A30004 /* AppInstUtil: no such add-on */

/* One batch of copies at a time: all add-ons to install, of every game, are handed to the
 * system together after a scan, which copies them one by one in that order. The calls run in their own
 * thread; the main loop follows each copy in ac_poll (klog copy lines, then the file size). */
#define AC_RETURN_S 60 /* a call still running this long after the copy reached full size is not waited for */
#define AC_MAX_JOBS MAX_DLC

typedef struct {
    int data, started, ended, copy_start, copy_end; /* copy_*: klog lines seen */
    int no_progress; /* ac.pkg was fully allocated at once: written space shows no progress */
    volatile int called, rc;
    time_t full_at; /* when the copy reached full size */
    char cid[40], game[10], pkg_path[PATH_MAX], name[128], title[128]; /* title: the game's name */
    char copy_session[24]; /* transferSessionId of copy_start: an end line only counts for the
                             * same session, never a stale one from the system uninstalling a
                             * prior partial copy before overwriting it */
    uint64_t size;
    acwait_t wait;
} ac_job_t;

static ac_job_t g_acj[AC_MAX_JOBS];
static char g_ac_left[MAX_TITLES][10]; /* games with add-ons past the full batch: not ready after it */
static int g_ac_nleft;
static int g_acn, g_ac_ok, g_ac_bad;
static volatile int g_ac_gen; /* a new batch changes it: late returns of older calls are ignored */
static time_t g_ac_next, g_ac_since;
static ac_playgo_t g_ac_playgo;

static int g_ac_submitted; /* the batch was handed to the system; before that it is still being built */

static int ac_busy(void) { return g_acn > 0; }

static ac_job_t *ac_job_of(const char *cid) {
    for (int i = 0; i < g_acn; i++)
        if (!g_acj[i].ended && !strcmp(g_acj[i].cid, cid))
            return &g_acj[i];
    return NULL;
}

static void ac_copy_path(char *out, size_t n, const char *cid) {
    snprintf(out, n, "/user/addcont/%.9s/%s/ac.pkg", cid + 7, cid + 20);
}

/* <CID>.acpend: written with .accopy when a copy starts, removed when it succeeded. Left
 * behind, it means a failed or partial copy that ac_reconcile still has to clean up. */
static void ac_pend_path(char *out, size_t n, const char *cid) { snprintf(out, n, STATE_DIR "/%s.acpend", cid); }

static void *ac_install_thread(void *arg) {
    int gen = (int)(intptr_t)arg;
    for (int i = 0; i < g_acn && g_ac_gen == gen; i++) {
        ac_job_t *j = &g_acj[i];
        ac_meta_t meta;
        ac_pkg_t info;
        char name[32];
        memset(&meta, 0, sizeof(meta));
        memset(&info, 0, sizeof(info));
        memset(&g_ac_playgo, 0, sizeof(g_ac_playgo));
        snprintf(name, sizeof(name), "%s (Add-on)", j->game);
        meta.uri = j->pkg_path;
        meta.ex_uri = meta.playgo_scenario_id = meta.content_id = meta.icon_url = "";
        meta.content_name = name;
        int rc = sceAppInstUtilInstallByPackage(&meta, &info, &g_ac_playgo);
        printf("[accopy] %s: InstallByPackage=0x%08x\n", meta.uri, rc);
        if (g_ac_gen != gen)
            break; /* given up on: a newer batch owns the jobs */
        j->rc = rc;
        j->called = 1;
    }
    return NULL;
}

/* Adds one add-on to the batch being built. 0 when added; 1 when the batch is full (it goes
 * in the next one, not a failure); -1 with why set. The free space
 * must hold every add-on of the batch. */
static int ac_queue(const dlc_src_t *a, const char *game_tid, char *why, size_t cap) {
    struct statfs sf;
    if (g_acn == AC_MAX_JOBS) {
        return 1;
    }
    if (statfs("/user", &sf)) {
        snprintf(why, cap, "free space unknown (%s)", strerror(errno));
        return -1;
    }
    uint64_t queued = 0;
    for (int i = 0; i < g_acn; i++)
        queued += g_acj[i].size;
    if (!accopy_space_ok((uint64_t)sf.f_bavail * sf.f_bsize, queued + a->pkg.pkg_size)) {
        snprintf(why, cap, "not enough free space");
        return -1;
    }
    ac_job_t *j = &g_acj[g_acn];
    memset(j, 0, sizeof(*j));
    j->data = a->data;
    snprintf(j->cid, sizeof(j->cid), "%s", a->pkg.content_id);
    snprintf(j->game, sizeof(j->game), "%s", game_tid);
    snprintf(j->pkg_path, sizeof(j->pkg_path), "%s", a->path);
    const sfo_item_t *it = sfo_get(&a->sfo, "TITLE");
    snprintf(j->name, sizeof(j->name), "%s", it && !it->is_int ? it->sval : a->pkg.content_id);
    j->size = a->pkg.pkg_size;
    char pend[96];
    ac_pend_path(pend, sizeof(pend), j->cid);
    if (accopy_mark(STATE_DIR, j->cid, a->path) || write_file(pend, "", 0, 0666)) {
        snprintf(why, cap, "state files not written");
        return -1;
    }
    g_acn++;
    printf("[accopy] %s: queued %s (%llu bytes)\n", j->cid, a->path, (unsigned long long)j->size);
    return 0;
}

/* Hands the batch to the system. */
static void ac_submit(void) {
    if (!g_acn || g_ac_submitted)
        return;
    g_ac_submitted = 1;
    int gen = ++g_ac_gen;
    g_ac_ok = g_ac_bad = 0;
    g_ac_since = time(NULL);
    g_ac_next = g_ac_since + AC_POLL_SECONDS;
    for (int i = 0; i < g_acn; i++)
        acwait_start(&g_acj[i].wait, g_ac_since);
    printf("[accopy] installing %d add-on(s)\n", g_acn);
    pthread_t th;
    if (pthread_create(&th, NULL, ac_install_thread, (void *)(intptr_t)gen)) {
        for (int i = 0; i < g_acn; i++) {
            g_acj[i].called = 1;
            g_acj[i].rc = -1; /* failed at the next poll; the partial-copy cleanup undoes the marks */
        }
        return;
    }
    pthread_detach(th);
}

/* klog: the system's copy of an add-on started or ended ("...transferSessionId = 0..0-<CID>").
 * Re-installing over a stale partial copy makes the system uninstall it first, which emits
 * its own "end" line for that old session: taking it for this job's end would mark the job
 * done before its real copy (started by a later, differently-numbered session) even begins. */
static void ac_copy_event(const char *line) {
    const char *p = strstr(line, "transferSessionId = ");
    if (!p || !ac_busy())
        return;
    p += 20;
    const char *dash = strchr(p, '-');
    if (!dash)
        return;
    char session[24];
    size_t slen = (size_t)(dash - p);
    if (slen >= sizeof(session))
        slen = sizeof(session) - 1;
    memcpy(session, p, slen);
    session[slen] = 0;
    char cid[40];
    snprintf(cid, sizeof(cid), "%.36s", dash + 1);
    ac_job_t *j = ac_job_of(cid);
    if (!j)
        return;
    if (strstr(line, "SendDownloadCopyEnd")) {
        if (!j->copy_session[0] || strcmp(j->copy_session, session))
            return; /* end of a different, stale session: not this job's copy */
        j->copy_end = 1;
        g_ac_next = 0; /* followed at once, not at the next poll */
    } else {
        snprintf(j->copy_session, sizeof(j->copy_session), "%s", session);
        j->copy_start = 1;
        if (!j->started) {
            j->started = 1;
            acwait_start(&j->wait, time(NULL));
        }
    }
}

/* Uninstalls an add-on PS4Mount installed: by label, then by content ID. "Not found"
 * counts as done (the system may have removed it already). */
static int ac_uninstall(const char *cid) {
    char dtid[10];
    snprintf(dtid, sizeof(dtid), "%.9s", cid + 7);
    int rc = sceAppInstUtilAppUnInstallAddcont(dtid, cid + 20);
    printf("[accopy] %s: uninstall by label 0x%08x\n", cid, rc);
    if (rc) {
        int rc2 = sceAppInstUtilAppUnInstallAddcont(dtid, cid);
        printf("[accopy] %s: uninstall by content id 0x%08x\n", cid, rc2);
        if (!rc2 || (rc == (int)AC_NOT_FOUND && rc2 == (int)AC_NOT_FOUND))
            rc = 0;
    }
    return rc;
}

/* Uninstalls the add-ons PS4Mount copied for game (every game when NULL): their .accopy
 * file holds the pkg path, <root>/ps4/<GAME>/addcont/... */
static void ac_uninstall_copies(const char *game) {
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strlen(e->d_name) != 36 + 7 || strcmp(e->d_name + 36, ".accopy"))
            continue;
        char cid[40], path[96], pkg[PATH_MAX] = "", tag[16];
        snprintf(cid, sizeof(cid), "%.36s", e->d_name);
        snprintf(path, sizeof(path), STATE_DIR "/%s", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, pkg, sizeof(pkg) - 1);
            pkg[n > 0 ? n : 0] = 0;
            close(fd);
        }
        snprintf(tag, sizeof(tag), "/ps4/%s/", game ? game : "");
        if (game && !strstr(pkg, tag))
            continue;
        char copy[PATH_MAX];
        ac_copy_path(copy, sizeof(copy), cid);
        if (ac_uninstall(cid) || exists(copy))
            continue;
        accopy_unmark(STATE_DIR, cid);
        acfail_clear(STATE_DIR, cid);
        snprintf(path, sizeof(path), STATE_DIR "/%s.acpg", cid);
        unlink(path);
        ac_pend_path(path, sizeof(path), cid);
        unlink(path);
        printf("[accopy] %s: uninstalled\n", cid);
    }
    closedir(d);
}

/* Copy status of an add-on for the panel: "installed", "installing", "problem" (why set),
 * "store", "missing" (not copied) or "" (mount mode). */
static const char *ac_status(const dlc_src_t *a, int on, char *why, size_t cap) {
    const char *cid = a->pkg.content_id;
    char p[PATH_MAX];
    if (!g_cfg.dlc_copy)
        return "";
    if (ac_job_of(cid))
        return "installing";
    int marked = accopy_marked(STATE_DIR, cid);
    ac_pend_path(p, sizeof(p), cid);
    int pending = exists(p);
    ac_copy_path(p, sizeof(p), cid);
    if (!pending && marked && (!a->data || exists(p)))
        return "installed";
    if (!pending && a->data && exists(p))
        return "store";
    if (on && acfail_get(STATE_DIR, cid, why, cap) >= ACFAIL_MAX)
        return "problem";
    why[0] = 0;
    return "missing";
}

/* Copy mode: an add-on turned on is not in yet (copying or still to copy). A problem add-on
 * is never tried again, so it does not hold the game. */
static int ac_game_waiting(const title_src_t *t) {
    char why[64];
    for (int i = 0; i < t->ndlc; i++) {
        const char *st = ac_status(&t->dlc[i], 1, why, sizeof(why));
        if (!strcmp(st, "installing") || !strcmp(st, "missing"))
            return 1;
    }
    return 0;
}

/* Work that waits for the copy in progress: back to mount mode, games removed. */
static int g_ac_revert;
static char g_ac_drop[MAX_TITLES][10];
static int g_ac_ndrop;

static void ac_drop_game(const char *tid) {
    if (g_ac_ndrop < MAX_TITLES)
        snprintf(g_ac_drop[g_ac_ndrop++], 10, "%s", tid);
}

static void ac_tasks(int deleting) {
    if (ac_busy() || deleting)
        return;
    if (g_ac_revert && !g_cfg.dlc_copy) {
        printf("[accopy] mount mode: removing the copies\n");
        ac_uninstall_copies(NULL);
    }
    g_ac_revert = 0;
    for (int i = 0; i < g_ac_ndrop; i++)
        ac_uninstall_copies(g_ac_drop[i]);
    g_ac_ndrop = 0;
}

/* STATE_DIR/<CID>.acpg: the system install made /user/playgo/addcont/<CID>, so losing it
 * later (a registration of the game removes it) means the copy must be installed again. */
static void ac_playgo_note(const char *cid) {
    char p[PATH_MAX], q[96];
    snprintf(p, sizeof(p), "/user/playgo/addcont/%s", cid);
    snprintf(q, sizeof(q), STATE_DIR "/%s.acpg", cid);
    if (exists(p))
        write_file(q, "", 0, 0666);
    else
        unlink(q);
}

static int ac_playgo_ok(const char *cid) {
    char p[PATH_MAX], q[96];
    snprintf(p, sizeof(p), "/user/playgo/addcont/%s", cid);
    snprintf(q, sizeof(q), STATE_DIR "/%s.acpg", cid);
    return !exists(q) || exists(p);
}

/* One job of the batch: 1 done, -1 failed (why set), 0 still running. A copy only counts
 * as running from its klog start line, or once every job before it ended (the system copies
 * in order), so add-ons waiting in the queue are not taken for stalled. */
/* The copy is over but its result is not known from the call: done only if the system
 * wrote its files, after giving the call AC_RETURN_S to return. 1, -1 or 0 (waiting). */
static int ac_files_written(ac_job_t *j, time_t now, char *why, size_t cap) {
    if (j->called)
        return 1;
    if (!j->full_at)
        j->full_at = now;
    if (now - j->full_at < AC_RETURN_S)
        return 0;
    char dir[PATH_MAX], q[PATH_MAX + 16];
    snprintf(dir, sizeof(dir), "/user/addcont/%.9s/%s", j->cid + 7, j->cid + 20);
    snprintf(q, sizeof(q), "%s/ac.json", dir);
    int ok = exists(q);
    snprintf(q, sizeof(q), "%s/ac.pbm", dir);
    ok = ok && exists(q);
    printf("[accopy] %s: copy over, call not returned after %d s, ac.json/ac.pbm %s\n", j->cid, AC_RETURN_S,
           ok ? "present" : "missing");
    if (ok)
        return 1;
    snprintf(why, cap, "install did not finish");
    return -1;
}

static int ac_job_step(ac_job_t *j, int head, int next_started, time_t now, char *why, size_t cap) {
    char p[PATH_MAX];
    if (j->called && j->rc) {
        snprintf(why, cap, "install call 0x%08x", j->rc);
        return -1;
    }
    if (!exists(j->pkg_path)) {
        snprintf(why, cap, "drive disconnected");
        return -1;
    }
    if (!j->data) {
        /* unlock only: nothing is copied, the call's result is the answer */
        if (j->called)
            return 1;
        if (acwait_step(&j->wait, 0, 1, now) == ACW_TIMEOUT) {
            snprintf(why, cap, "install call never returned");
            return -1;
        }
        return 0;
    }
    if (!j->started) {
        if (!head)
            return 0;
        j->started = 1;
        acwait_start(&j->wait, now);
    }
    /* the system creates ac.pkg at full size first: progress is the space really written,
     * and only the klog end line says the copy is done */
    struct stat st;
    ac_copy_path(p, sizeof(p), j->cid);
    uint64_t cur = stat(p, &st) ? 0 : (uint64_t)st.st_blocks * 512;
    int r = acwait_step(&j->wait, cur < j->size ? cur : j->size - 1, j->size, now);
    if (j->copy_end)
        return j->called ? 1 : ac_files_written(j, now, why, cap);
    if (next_started)
        return ac_files_written(j, now, why, cap); /* the system moved on: its end line was missed */
    if (r == ACW_TIMEOUT) {
        snprintf(why, cap, "copy timed out");
        return -1;
    }
    if (cur >= j->size - j->size / 64 && cur && !j->copy_end && j->wait.last == cur && now == j->wait.grew)
        j->no_progress |= now - j->wait.start < 2 * AC_POLL_SECONDS + 1;
    if (r == ACW_STALL && !j->no_progress) {
        snprintf(why, cap, "copy stopped");
        return -1;
    }
    return 0;
}

/* Called from the main loop: follows the batch, at most every AC_POLL_SECONDS. A failed copy
 * releases nothing for the others: each job ends on its own. */
static void ac_poll(void) {
    if (!g_ac_submitted || time(NULL) < g_ac_next)
        return;
    time_t now = time(NULL);
    g_ac_next = now + AC_POLL_SECONDS;
    int head = 1, left = 0;
    for (int i = 0; i < g_acn; i++) {
        ac_job_t *j = &g_acj[i];
        if (j->ended)
            continue;
        char why[64] = "";
        int next_started = 0;
        for (int k = i + 1; k < g_acn; k++)
            next_started |= g_acj[k].data && g_acj[k].started && g_acj[k].copy_start;
        int r = ac_job_step(j, head, next_started, now, why, sizeof(why));
        if (j->data && !r)
            head = 0;
        if (!r) {
            left++;
            continue;
        }
        j->ended = 1;
        if (r > 0) {
            j->ended = 2;
            char pend[96];
            ac_pend_path(pend, sizeof(pend), j->cid);
            unlink(pend);
            acfail_clear(STATE_DIR, j->cid);
            ac_playgo_note(j->cid);
            printf("[accopy] %s: copied\n", j->cid);
            g_ac_ok++;
        } else {
            /* .accopy and .acpend stay: ac_reconcile removes the partial copy */
            int n = acfail_add(STATE_DIR, j->cid, why);
            printf("[accopy] %s: failed (%s), %d of %d\n", j->cid, why, n, ACFAIL_MAX);
            notify("%s: add-on %s not installed (%s)", j->game, j->name, why);
            g_ac_bad++;
        }
    }
    if (left)
        return;
    printf("[accopy] batch done in %ld s, %d installed, %d failed\n", (long)(now - g_ac_since), g_ac_ok, g_ac_bad);
    if (g_ac_ok)
        notify("%d add-on%s installed", g_ac_ok, g_ac_ok > 1 ? "s" : "");
    /* a game is ready to play once all of its add-ons in the batch are in */
    for (int i = 0; i < g_acn; i++) {
        int first = 1, all = 1;
        for (int k = 0; k < g_acn; k++)
            if (!strcmp(g_acj[k].game, g_acj[i].game)) {
                first &= k >= i;
                all &= g_acj[k].ended == 2;
            }
        for (int k = 0; k < g_ac_nleft; k++)
            all &= strcmp(g_ac_left[k], g_acj[i].game) != 0;
        if (first && all)
            notify("%s ready to play", g_acj[i].title[0] ? g_acj[i].title : g_acj[i].game);
    }
    g_ac_gen++; /* a call that has not returned yet is ignored */
    g_acn = 0;
    g_ac_nleft = 0;
    g_ac_submitted = 0;
}

static int place_files(title_src_t *t) {
    pkg_t *app = &t->app, *pat = t->has_patch ? &t->patch : NULL;
    const char *tid = app->title_id, *cid = app->content_id;
    const sfo_item_t *av = sfo_get(&t->app_sfo, "APP_VER");
    const char *app_ver = av && !av->is_int ? av->sval : "01.00";
    char p[PATH_MAX], q[PATH_MAX], hold[PATH_MAX];
    int rc = 0;

    snprintf(p, sizeof(p), "/user/appmeta/%s", tid);
    mkdirs(p, 0777);
    write_meta(app, p, &rc);
    if (pat)
        write_meta(pat, p, &rc); /* the update's artwork wins, as with a real install */

    snprintf(p, sizeof(p), "/system_data/priv/appmeta/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/param.sfo", p);
    rc |= write_entry(pat ? pat : app, 0x1000, q) != 0;
    snprintf(q, sizeof(q), "%s/nptitle.dat", p);
    rc |= write_entry(app, 0x402, q) < 0;
    snprintf(q, sizeof(q), "%s/npbind.dat", p);
    rc |= write_entry(app, 0x403, q) < 0;

    if (place_license(app))
        return -1;
    if (!g_cfg.dlc_copy) { /* copy mode: the system installs them (ac_reconcile) */
        for (int i = 0; i < t->ndlc; i++)
            rc |= place_dlc(&t->dlc[i]);
        for (int i = t->ndlc; i < t->ndlc_all; i++)
            drop_dlc(&t->dlc[i].pkg);
    }

    size_t pl = 0, ppl = 0;
    uint8_t *pg = pkg_entry(app, 0x1001, &pl), *ppg = pat ? pkg_entry(pat, 0x1001, &ppl) : NULL;
    buf_t json = {0}, pbm = {0}, xml = {0}, pjson = {0}, ppbm_patch = {0}, pxml = {0}, ppbm = {0}, pdb = {0}, patch_pdb = {0};
    build_app_json(&json, app, t->app_path);
    build_app_pbm(&pbm, app, app_ver, PBM_APP);
    patch_ref_t pr = {0}, *prp = NULL;
    if (pat) {
        build_app_json(&pjson, pat, t->patch_path);
        build_app_pbm(&ppbm_patch, pat, t->ver, PBM_PATCH);
        pr = (patch_ref_t){pat, ppg, ppl, t->ver, t->patch_path, &pjson};
        prp = &pr;
    }
    if (build_app_xml(&xml, pg, pl) || build_pfs_pbm(&ppbm, app, pg, pl, prp) ||
        build_pdb(&pdb, app, t->ver, t->app_path, &json, pg, pl, prp) ||
        (pat && (build_app_xml(&pxml, ppg, ppl) || build_patch_pdb(&patch_pdb, &pr)))) {
        printf("[reg] %s: playgo-chunk.dat unusable\n", tid);
        rc = -1;
        goto out;
    }
    rc |= hold_prepare("app", tid, "app.pkg", t->app_path);
    hold_path(hold, sizeof(hold), "app", tid);
    snprintf(q, sizeof(q), "%s/app.json", hold);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/app.pbm", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.pbm.backup", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.xml", hold);
    rc |= write_buf(q, &xml);
    snprintf(p, sizeof(p), "/system_data/playgo/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/pfs.pbm", p);
    rc |= write_buf(q, &ppbm);
    snprintf(q, sizeof(q), "%s/pfs.pbm.backup", p);
    rc |= write_buf(q, &ppbm);
    snprintf(p, sizeof(p), "/user/playgo/app/%s/00", cid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/d0.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/d1.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(p, sizeof(p), "/user/playgo/patch/%s", cid);
    if (pat) {
        rc |= hold_prepare("patch", tid, "patch.pkg", t->patch_path);
        hold_path(hold, sizeof(hold), "patch", tid);
        snprintf(q, sizeof(q), "%s/patch.json", hold);
        rc |= write_buf(q, &pjson);
        snprintf(q, sizeof(q), "%s/patch.pbm", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.pbm.backup", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.xml", hold);
        rc |= write_buf(q, &pxml);
        snprintf(p, sizeof(p), "/user/playgo/patch/%s/00/00.00", cid);
        mkdirs(p, 0777);
        snprintf(q, sizeof(q), "%s/d0.pdb", p);
        rc |= write_buf(q, &patch_pdb);
        snprintf(q, sizeof(q), "%s/d1.pdb", p);
        rc |= write_buf(q, &patch_pdb);
    } else {
        rm_tree(p);
        hold_path(hold, sizeof(hold), "patch", tid);
        rm_tree(hold);
    }
out:
    free(pg);
    free(ppg);
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&xml);
    buf_free(&pjson);
    buf_free(&ppbm_patch);
    buf_free(&pxml);
    buf_free(&ppbm);
    buf_free(&pdb);
    buf_free(&patch_pdb);
    return rc ? -1 : 0;
}

#define IOV(s) {(void *)(s), strlen(s) + 1}

/* Titles this daemon serves. */
typedef struct {
    int used, has_patch, running, ndlc, ndlc_tids;
    char dlc_tids[MAX_DLC_TIDS][10];
    time_t running_since; /* launch seen in klog; the sandbox mount appears shortly after */
    char tid[10], cid[37], name[128], dir[PATH_MAX];
} managed_t;

static managed_t g_managed[MAX_TITLES];
static int g_managed_full; /* games manage() could not give a slot this scan cycle */
static int g_manual_scan; /* "Scan for games" in the panel */
static int g_scan_changed; /* a drive had unread pkgs the most recent scan_all() call */
static int g_idle_scans;   /* consecutive scan_all() calls with nothing new anywhere */
static int g_drive_changed[NUM_DRIVES]; /* this drive had unread pkgs the most recent scan */
static int g_drive_idle[NUM_DRIVES];    /* consecutive scans with nothing new on this drive */
static int g_scan_only = -1; /* index of the only drive to walk in the next scan, -1 for all */
static int g_deleting; /* a delete screen is open: game folders are unmounted */
static time_t g_deleting_since;
static time_t g_deleting_close_at; /* set when the uninstall finished */

static managed_t *managed_by(const char *tid, const char *cid) {
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && ((tid && !strcmp(g_managed[i].tid, tid)) || (cid && !strcmp(g_managed[i].cid, cid))))
            return &g_managed[i];
    return NULL;
}

/* Top mount on target: 0 none, 1 our folder, -1 something else (like a USB
 * folder mounted by an older PS4Mount). */
static int top_layer(const char *target, const char *dir) {
    struct statfs sf;
    if (statfs(target, &sf) || strcmp(sf.f_mntonname, target))
        return 0;
    return strcmp(sf.f_fstypename, "nullfs") || strcmp(sf.f_mntfromname, dir) ? -1 : 1;
}

static int nullfs_mount(const char *dir, const char *target) {
    struct iovec iov[] = {IOV("fstype"), IOV("nullfs"), IOV("from"), IOV(dir), IOV("fspath"), IOV(target)};
    if (nmount(iov, 6, 0)) {
        printf("[mount] %s -> %s: %s\n", dir, target, strerror(errno));
        return -1;
    }
    return 0;
}

/* A running PS4 title keeps /mnt/sandbox/pfsmnt/<TID>-app0 mounted. Unlike the klog
 * events this survives a restart of the daemon, so it is the reference for "running". */
/* Sandbox mounts of running titles, read from the mount table at most once a second
 * (the table holds over a thousand entries; reading it per title would be wasteful). */
static char (*g_sandboxes)[10];
static int g_nsandboxes;
static time_t g_sandboxes_at;

static void refresh_sandboxes(void) {
    if (g_sandboxes && time(NULL) == g_sandboxes_at)
        return;
    g_sandboxes_at = time(NULL);
    g_nsandboxes = 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return;
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 16));
    char (*ids)[10] = realloc(g_sandboxes, sizeof(*ids) * (size_t)(n + 16));
    if (ids)
        g_sandboxes = ids;
    if (!list || !ids) {
        free(list);
        return;
    }
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    static const char prefix[] = "/mnt/sandbox/pfsmnt/";
    for (int i = 0; i < n; i++) {
        const char *on = list[i].f_mntonname;
        if (strlen(on) != sizeof(prefix) - 1 + 9 + 5 || strncmp(on, prefix, sizeof(prefix) - 1) ||
            strcmp(on + sizeof(prefix) - 1 + 9, "-app0"))
            continue;
        snprintf(g_sandboxes[g_nsandboxes++], 10, "%.9s", on + sizeof(prefix) - 1);
    }
    free(list);
}

static int title_running(const char *tid) {
    refresh_sandboxes();
    for (int i = 0; i < g_nsandboxes; i++)
        if (!strcmp(g_sandboxes[i], tid))
            return 1;
    return 0;
}

static void unmount_all_layers(const char *target, int force);

/* Set while /user/app shows only the games being registered (see mask_user_app): the
 * other game folders are hidden then, and must not be mounted or unmounted. */
static int g_masked;

static int masked_path(const char *target) {
    if (!g_masked || strncmp(target, "/user/app/", 10))
        return 0;
    printf("[mount] %s: skipped while /user/app shows only the new games\n", target);
    return 1;
}

static int mount_dir(const char *dir, const char *target) {
    if (masked_path(target))
        return 0;
    /* /user/patch and /user/addcont only exist once the console installed an update or
     * an add-on the normal way: create the parent like the system does (0770) */
    char parent[64];
    snprintf(parent, sizeof(parent), "%s", target);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (mkdir(parent, 0770) == 0)
            printf("[mount] created %s\n", parent);
    }
    mkdir(target, 0777);
    if (dead_layer(target)) {
        printf("[mount] %s: removing a layer of a disconnected drive\n", target);
        unmount_all_layers(target, 1);
    }
    int top = top_layer(target, dir);
    if (top == 1)
        return 0;
    while (top != 0) {
        if (unmount(target, 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            return -1;
        }
        top = top_layer(target, dir);
    }
    return nullfs_mount(dir, target);
}

static void unmount_all_layers(const char *target, int force) {
    if (masked_path(target))
        return;
    for (int i = 0; i < 8 && (mounted_from(target, NULL, 0) || layers_on(target) > 0); i++)
        if (unmount(target, force ? MNT_FORCE : 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            if (!force)
                return;
        }
}

/* Removes every layer of the title (forced: a system uninstall is about to run). */
static void unmount_title(const char *tid, char (*dtids)[10], int ndtids) {
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    unmount_all_layers(target, 1);
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    unmount_all_layers(target, 1);
    for (int i = 0; i < ndtids; i++) {
        snprintf(target, sizeof(target), "/user/addcont/%s", dtids[i]);
        unmount_all_layers(target, 1);
    }
}

/* Add-ons installed the normal way: their folder is never covered by a mount. */
static int addcont_installed(const char *dtid) {
    char target[64];
    snprintf(target, sizeof(target), "/user/addcont/%s", dtid);
    if (mounted_from(target, NULL, 0))
        return 0;
    DIR *d = opendir(target);
    if (!d)
        return 0;
    struct dirent *e;
    int used = 0;
    while (!used && (e = readdir(d)))
        used = e->d_name[0] != '.';
    closedir(d);
    return used;
}

/* The game was registered after the copy started (STATE_DIR/<GAME>.src is written by each
 * registration): a copy lost now was removed by that registration, not by the user. */
static int ac_rereg(const char *tid, const char *cid) {
    char a[64], b[96];
    struct stat sa, sb;
    snprintf(a, sizeof(a), STATE_DIR "/%s.src", tid);
    snprintf(b, sizeof(b), STATE_DIR "/%s.accopy", cid);
    return !stat(a, &sa) && !stat(b, &sb) && sa.st_mtime > sb.st_mtime;
}

/* Copy mode, once per scan for a game already in the library: releases add-on folders
 * still mounted by the mount mode, then takes one step for each add-on (install, uninstall,
 * turn off). At most one copy runs at a time. */
static int g_scan_again; /* games were just added: scan again without waiting */

static void ac_reconcile(title_src_t *t) {
    if (!g_cfg.dlc_copy || !t->ndlc_all)
        return;
    const char *tid = t->app.title_id;
    for (int i = 0; i < t->ndlc_tids; i++) {
        char target[64], hold[PATH_MAX], from[PATH_MAX];
        snprintf(target, sizeof(target), "/user/addcont/%s", t->dlc_tids[i]);
        if (!mounted_from(target, from, sizeof(from)))
            continue;
        hold_path(hold, sizeof(hold), "addcont", t->dlc_tids[i]);
        if (strcmp(from, hold)) {
            printf("[accopy] %s mounted from %s, add-ons not copied\n", target, from);
            return;
        }
        unmount_all_layers(target, 1);
        if (mounted_from(target, NULL, 0))
            return;
        for (int k = 0; k < t->ndlc_all; k++)
            if (!strcmp(t->dlc[k].pkg.title_id, t->dlc_tids[i]))
                drop_dlc(&t->dlc[k].pkg);
        printf("[accopy] %s: mounted add-ons released for copying\n", target);
    }
    int submit_later = g_ac_submitted; /* a batch is running: these add-ons go in the next one */
    for (int i = 0; i < t->ndlc_all; i++) {
        dlc_src_t *a = &t->dlc[i];
        const char *cid = a->pkg.content_id;
        char p[PATH_MAX], why[64];
        ac_copy_path(p, sizeof(p), cid);
        char pend[96];
        ac_pend_path(pend, sizeof(pend), cid);
        int busy = ac_job_of(cid) != NULL;
        if (!busy && exists(pend)) {
            /* a failed or partial copy: removed before anything else is tried */
            ac_uninstall(cid);
            if (!exists(p)) {
                accopy_unmark(STATE_DIR, cid);
                unlink(pend);
                printf("[accopy] %s: failed copy removed\n", cid);
            }
            continue;
        }
        acin_t in = {0};
        in.on = i < t->ndlc;
        in.marked = accopy_marked(STATE_DIR, cid);
        /* unlock-only add-ons leave no ac.pkg: only our mark says they are there */
        in.files = a->data ? exists(p) : in.marked;
        in.store = in.files && !in.marked;
        in.rereg = in.marked && ac_rereg(tid, cid);
        in.playgo = ac_playgo_ok(cid);
        in.fails = acfail_get(STATE_DIR, cid, NULL, 0);
        in.busy = busy;
        switch (accopy_decide(&in)) {
        case AC_REINSTALL:
            if (submit_later)
                break;
            printf("[accopy] %s: lost after the game was registered again, installing again\n", cid);
            ac_uninstall(cid);
            if (exists(p))
                break; /* still there: tried again next scan */
            accopy_unmark(STATE_DIR, cid);
            /* fall through */
        case AC_INSTALL:
            if (submit_later)
                break;
            int q = ac_queue(a, tid, why, sizeof(why));
            if (!q) {
                snprintf(g_acj[g_acn - 1].title, sizeof(g_acj[0].title), "%s", t->name);
            } else if (q > 0) {
                int k = 0;
                while (k < g_ac_nleft && strcmp(g_ac_left[k], tid))
                    k++;
                if (k == g_ac_nleft && g_ac_nleft < MAX_TITLES)
                    snprintf(g_ac_left[g_ac_nleft++], 10, "%s", tid);
            } else {
                int n = acfail_add(STATE_DIR, cid, why);
                printf("[accopy] %s: not started (%s), %d of %d\n", cid, why, n, ACFAIL_MAX);
                notify("%s: add-on %s not installed (%s)", tid, cid + 20, why);
            }
            break;
        case AC_UNINSTALL:
            if (!ac_uninstall(cid) && !(a->data && exists(p))) {
                accopy_unmark(STATE_DIR, cid);
                printf("[accopy] %s: uninstalled (turned off)\n", cid);
            }
            break;
        case AC_TURN_OFF:
            /* deleted from the system storage screen: off until turned on in the panel */
            kv_set(ADDONS_OFF_FILE, cid, "off");
            accopy_unmark(STATE_DIR, cid);
            printf("[accopy] %s: deleted on the console, turned off\n", cid);
            break;
        default:
            break;
        }
    }
}

/* /user/app/<TID> always, /user/patch/<TID> only with an update and
 * /user/addcont/<ADD-ON TID> for each add-on title. */
static int mount_paths(const char *tid, int has_patch, char (*dtids)[10], int ndtids) {
    char target[64], hold[PATH_MAX];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    hold_path(hold, sizeof(hold), "app", tid);
    if (mount_dir(hold, target))
        return -1;
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    hold_path(hold, sizeof(hold), "patch", tid);
    if (has_patch ? mount_dir(hold, target) : (unmount_all_layers(target, 1), 0))
        return -1;
    for (int i = 0; !g_cfg.dlc_copy && i < ndtids; i++) {
        int copying = 0;
        for (int k = 0; k < g_acn; k++)
            copying |= !g_acj[k].ended && !strncmp(g_acj[k].cid + 7, dtids[i], 9);
        if (copying) {
            printf("[mount] /user/addcont/%s: a copy is still running, not mounted\n", dtids[i]);
            continue;
        }
        if (addcont_installed(dtids[i])) {
            printf("[mount] /user/addcont/%s holds add-ons installed the normal way, not mounted\n", dtids[i]);
            continue;
        }
        snprintf(target, sizeof(target), "/user/addcont/%s", dtids[i]);
        hold_path(hold, sizeof(hold), "addcont", dtids[i]);
        if (mount_dir(hold, target))
            return -1;
    }
    return 0;
}

static int mount_title(title_src_t *t) {
    return mount_paths(t->app.title_id, t->has_patch, t->dlc_tids, t->ndlc_tids);
}

static void manage(const title_src_t *t) {
    managed_t *m = managed_by(t->app.title_id, NULL);
    for (int i = 0; !m && i < MAX_TITLES; i++)
        if (!g_managed[i].used)
            m = &g_managed[i];
    if (!m) {
        if (policy_at_capacity(MAX_TITLES, MAX_TITLES))
            g_managed_full++;
        return;
    }
    /* keep the runtime state across rescans */
    managed_t keep = *m;
    memset(m, 0, sizeof(*m));
    m->used = 1;
    if (keep.used) {
        m->running = keep.running;
        m->running_since = keep.running_since;
    }
    m->has_patch = t->has_patch;
    m->ndlc = t->ndlc;
    m->ndlc_tids = t->ndlc_tids;
    memcpy(m->dlc_tids, t->dlc_tids, sizeof(m->dlc_tids));
    snprintf(m->tid, sizeof(m->tid), "%s", t->app.title_id);
    snprintf(m->cid, sizeof(m->cid), "%s", t->app.content_id);
    snprintf(m->name, sizeof(m->name), "%s", t->name);
    snprintf(m->dir, sizeof(m->dir), "%s", t->dir);
}


static int removed(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), STATE_DIR "/%s.removed", tid);
    return exists(p);
}

/* Real installs keep app.pkg on the internal drive; never touch those. */
static int installed_internally(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), "/user/app/%s", tid);
    if (mounted_from(p, NULL, 0))
        return 0;
    snprintf(p, sizeof(p), "/user/app/%s/app.pkg", tid);
    return exists(p);
}

/* The marker without the drive name: "/mnt/usb0/ps4/<TID>..." -> "/ps4/<TID>...".
 * Drives are named in the order they are mounted, so the same drive can come back
 * under another name; the registration does not depend on it. */
static const char *skip_drive(const char *marker) {
    if (strncmp(marker, "/mnt/", 5))
        return marker;
    const char *s = strchr(marker + 5, '/');
    return s ? s : marker;
}

/* The stored marker, without the drive name, is expected; it is copied to stored. */
static int marker_matches(const char *tid, const char *expected, char *stored, size_t len) {
    char path[64], buf[PATH_MAX + 256] = {0};
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    snprintf(stored, len, "%s", buf);
    return n > 0 && !strcmp(skip_drive(buf), skip_drive(expected));
}

/* A registered game whose drive came back under another name: the links and records
 * that hold the pkg path are written again for the new folder, without registering the
 * game again. */
static int relocate(title_src_t *t, char *stored) {
    if (!strcmp(stored, t->marker))
        return 0;
    char was[PATH_MAX];
    snprintf(was, sizeof(was), "%.*s", (int)strcspn(stored, "\n"), stored);
    if (place_files(t)) {
        printf("[scan] %s: drive renamed (was %s), could not move its files\n", t->dir, was);
        return -1;
    }
    printf("[scan] %s: drive renamed (was %s), kept registered\n", t->dir, was);
    /* Every other .src marker is keyed by title ID (see marker_matches(), the reader this
     * write must agree with); t->dir is the pkg's own path for a title served from its
     * original folder, not a /ps4/<TID> one, so its basename is the pkg's filename, never
     * the title ID -- keying on it here would write a marker nothing ever reads back,
     * re-detecting the same "rename" on every scan forever. */
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", t->app.title_id);
    return write_file(path, t->marker, strlen(t->marker), 0666);
}

/* 1 = needs (re)registration, 0 = registered here, -1 = skip. */
/* The title is served from another copy: the marker names another location (see
 * game_location) whose game pkg is still there. The first copy keeps it, so the same
 * game on two drives is not switched (and registered again) on every scan. */
static int served_elsewhere(const char *tid, const char *dir) {
    char path[64], buf[PATH_MAX + 256] = {0}, pkg[PATH_MAX + 16];
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[strcspn(buf, "\n")] = 0;
    if (!strcmp(buf, dir))
        return 0;
    size_t l = strlen(buf);
    if (l > 4 && !strcasecmp(buf + l - 4, ".pkg"))
        return exists(buf);
    snprintf(pkg, sizeof(pkg), "%s/app.pkg", buf);
    return exists(pkg);
}

/* Games the user chose never to add (panel): one title ID per line. */
#define EXCLUDED_FILE STATE_DIR "/excluded.txt"

static int excluded(const char *tid) {
    char v[8];
    return !kv_get(EXCLUDED_FILE, tid, v, sizeof(v));
}

static retry_table_t g_retry;

/* Update chosen in the panel per title (APP_VER or CAT_NO_UPDATE); others use the highest. */
#define UPDATES_FILE STATE_DIR "/updates.txt"

static struct {
    char tid[10], ver[8];
} g_upd[MAX_TITLES];
static int g_nupd;

static int updates_add(const char *key, const char *value, void *arg) {
    (void)arg;
    if (g_nupd == MAX_TITLES)
        return 1;
    snprintf(g_upd[g_nupd].tid, sizeof(g_upd[0].tid), "%s", key);
    snprintf(g_upd[g_nupd].ver, sizeof(g_upd[0].ver), "%s", value);
    g_nupd++;
    return 0;
}

static void updates_load(void) {
    g_nupd = 0;
    kv_each(UPDATES_FILE, updates_add, NULL);
}

static const char *update_choice(const char *tid) {
    for (int i = 0; i < g_nupd; i++)
        if (!strcmp(g_upd[i].tid, tid))
            return g_upd[i].ver;
    return NULL;
}

#define MAX_ADDONS_OFF 512

static char g_off[MAX_ADDONS_OFF][37];
static int g_noff;

static int addons_off_add(const char *key, const char *value, void *arg) {
    (void)value;
    (void)arg;
    if (g_noff == MAX_ADDONS_OFF)
        return 1;
    snprintf(g_off[g_noff++], sizeof(g_off[0]), "%s", key);
    return 0;
}

static void addons_off_load(void) {
    g_noff = 0;
    kv_each(ADDONS_OFF_FILE, addons_off_add, NULL);
}

static int addon_off(const char *content_id) {
    for (int i = 0; i < g_noff; i++)
        if (!strcmp(g_off[i], content_id))
            return 1;
    return 0;
}

/* POLICY_ADD, POLICY_KEEP or POLICY_SKIP (see policy_decide); why gets the reason of a skip. */
static int title_state(title_src_t *t, const char **why) {
    const char *tid = t->app.title_id;
    game_facts_t f = {0};
    f.internal = installed_internally(tid);
    f.removed = removed(tid);
    f.excluded = excluded(tid);
    f.elsewhere = !f.internal && served_elsewhere(tid, t->dir);
    f.halted = g_halt[0] != 0;
    f.retry_wait = retry_waiting(&g_retry, tid, time(NULL));
    f.registered = db_is_registered(tid);
    if (f.registered) {
        char pdb[PATH_MAX];
        snprintf(pdb, sizeof(pdb), "/user/playgo/app/%s/00/d0.pdb", t->app.content_id);
        /* folders set up by 0.3.1 and older lack the internal folder: registered again. A
         * marker that matches except for the drive name means the drive came back renamed:
         * relocate() rewrites the pkg links for the new name instead of registering again,
         * and only counts as "not installed" if that rewrite itself fails. */
        char stored[PATH_MAX + 256];
        f.installed = exists(pdb) && marker_matches(tid, t->marker, stored, sizeof(stored)) && !relocate(t, stored) &&
                      hold_ready("app", tid, "app.pkg") && (!t->has_patch || hold_ready("patch", tid, "patch.pkg")) &&
                      dlc_ready(t);
    }
    int d = policy_decide(&f, why);
    if (d == POLICY_SKIP && f.elsewhere && first_time(t->app_path))
        printf("[scan] %s: %s is already served from another drive, this copy is not used\n", t->dir, tid);
    return d;
}

static void pump_events(int ms);

/* The launch check compares /system_data/priv/appmeta/<TID>/param.sfo with the
 * update's param.sfo byte for byte; AppInstallAll writes the game's one there. */
static void enforce_patch_sfo(title_src_t *t) {
    if (!t->has_patch)
        return;
    size_t len;
    uint8_t *sfo = pkg_entry(&t->patch, 0x1000, &len);
    if (!sfo)
        return;
    char path[96];
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/param.sfo", t->app.title_id);
    int fd = open(path, O_RDONLY), same = 0;
    if (fd >= 0) {
        uint8_t *cur = malloc(len + 1);
        same = cur && read(fd, cur, len + 1) == (ssize_t)len && !memcmp(cur, sfo, len);
        free(cur);
        close(fd);
    }
    if (!same) {
        printf("[reg] %s: priv param.sfo was not the update's, rewritten\n", t->app.title_id);
        write_file(path, sfo, len, 0777);
    }
    free(sfo);
}

/* Registration method: see fw.h. */
typedef int (*title_dir_fn)(const char *title_id, const char *dir, void *reserved);

static int g_reg_mode = REG_ALL;
static title_dir_fn g_title_dir;
static char g_fw_text[16] = "unknown";

static const char *reg_mode_name(void) {
    return g_reg_mode == REG_ALL ? "AppInstallAll" : g_reg_mode == REG_TITLEDIR ? "AppInstallTitleDir" : "none";
}

static void check_firmware(void) {
    uint32_t fw = kernel_get_fw_version();
    unsigned major, minor;
    fw_decode(fw, &major, &minor);
    fw_text(fw, g_fw_text, sizeof(g_fw_text));
    /* looked up at runtime: linking it would stop the payload from loading where it is gone */
    uint32_t h = 0;
    if (!kernel_dynlib_handle(getpid(), "libSceAppInstUtil.sprx", &h))
        g_title_dir = (title_dir_fn)kernel_dynlib_dlsym(getpid(), h, "sceAppInstUtilAppInstallTitleDir");
    g_reg_mode = fw_reg_mode(major, g_title_dir != NULL);
    printf("[main] firmware %s (0x%08x), AppInstallTitleDir %s, registration: %s\n", g_fw_text, fw,
           g_title_dir ? "present" : "missing", reg_mode_name());
}

/* The system registration runs in its own thread: after rest mode InstallAll was seen to
 * block for good, which froze the whole daemon (panel, klog, stop request). A call still
 * running is never doubled; titles keep being checked in the database. */
static volatile int g_install_busy, g_install_rc;
/* Set around the install_all() call in the batch loop: g_install_busy alone clears as
 * soon as sceAppInstUtilAppInstallAll returns, but install_all then still waits (pumping
 * events, which can run a panel launch on this same thread) for ShellCore to finish
 * registering. g_adding covers that whole wait too. */
static volatile int g_adding;
static char g_install_tids[MAX_TITLES][10];
static int g_install_n;

static int register_call(const char *tid) {
    if (g_reg_mode == REG_TITLEDIR) {
        int rc = g_title_dir(tid, "/user/app/", NULL);
        printf("[reg] %s: AppInstallTitleDir=0x%08x\n", tid, rc);
        return rc == (int)0x80990002 ? 0 : rc; /* 0x80990002: already known, restored */
    }
    return sceAppInstUtilAppInstallAll(NULL);
}

static void *install_thread(void *arg) {
    (void)arg;
    int rc = 0;
    if (g_reg_mode == REG_TITLEDIR) {
        for (int i = 0; i < g_install_n; i++) {
            int r = register_call(g_install_tids[i]);
            if (r)
                rc = r;
        }
    } else {
        rc = register_call(NULL);
    }
    g_install_rc = rc;
    g_install_busy = 0;
    return NULL;
}

/* Registers the titles of tids that are not registered yet. */
static int install_all_start(char (*tids)[10], int n) {
    if (g_reg_mode == REG_NONE || g_halt[0])
        return -1; /* never where it corrupts the database */
    if (g_install_busy) {
        printf("[reg] registration still running from an earlier call\n");
        return -1;
    }
    g_install_n = 0;
    for (int i = 0; i < n && g_install_n < MAX_TITLES; i++)
        if (!db_is_registered(tids[i]))
            snprintf(g_install_tids[g_install_n++], 10, "%s", tids[i]);
    g_install_busy = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, install_thread, NULL)) {
        install_thread(NULL); /* no thread: call it here */
        return 0;
    }
    pthread_detach(th);
    return 0;
}

/* Registration runs in three steps so a group of games costs the system one
 * AppInstallAll and no write to its databases while it is still busy:
 *   1. register_prepare: files and mounts of each game of the group;
 *   2. one sceAppInstUtilAppInstallAll, then wait until ShellCore has the rows;
 *   3. register_finish: checks the title is registered, writes the marker. */
static int register_prepare(title_src_t *t) {
    if (place_files(t) || mount_title(t))
        return -1;
    manage(t);
    return 0;
}

/* Max mtime of the system databases (and their journals): ShellCore is busy while it changes. */
static time_t db_mtime(void) {
    static const char *files[] = {"/system_data/priv/mms/app.db", "/system_data/priv/mms/app.db-journal",
                                  "/system_data/priv/mms/appinfo.db", "/system_data/priv/mms/appinfo.db-journal",
                                  "/system_data/priv/mms/addcont.db", NULL};
    time_t t = 0;
    struct stat st;
    for (int i = 0; files[i]; i++)
        if (!stat(files[i], &st) && st.st_mtime > t)
            t = st.st_mtime;
    return t;
}

/* Waits until the system databases were not written for quiet seconds (max limit). */
static void wait_db_quiet(int quiet, int limit) {
    time_t start = time(NULL), last = db_mtime(), changed = time(NULL);
    while (time(NULL) - changed < quiet && time(NULL) - start < limit) {
        pump_events(1000);
        time_t m = db_mtime();
        if (m != last) {
            last = m;
            changed = time(NULL);
        }
    }
}

/* ShellCore registers every new title under /user/app itself; this also refreshes the
 * home screen and fetches the PSN concept. Waits while titles keep arriving. */
/* AppInstallAll walks every folder of /system_ex/app, /system_ex/rnps/apps and /user/app
 * (about 0.7 s per PS4 game, registered or not). While it runs, /user/app shows only the
 * games being added: an empty staging folder is mounted over /user/app and each new
 * game's folder on top of it. The other games stay registered (their rows are not
 * touched) and their own mounts stay in place underneath. */
#define STAGE_DIR HOLD_DIR "/stage"

static int open_games(uint32_t *ids, char (*tids)[10], int max);

static void stage_clear(void) {
    DIR *d = opendir(STAGE_DIR);
    if (!d)
        return;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof(p), STAGE_DIR "/%s", e->d_name);
            rmdir(p);
        }
    closedir(d);
}

static int user_app_masked(void) {
    struct statfs sf;
    return !statfs("/user/app", &sf) && !strcmp(sf.f_mntonname, "/user/app") && !strcmp(sf.f_mntfromname, STAGE_DIR);
}

static void unmask_user_app(char (*tids)[10], int n) {
    char target[64];
    for (int i = 0; i < n; i++) {
        snprintf(target, sizeof(target), "/user/app/%s", tids[i]);
        if (user_app_masked() && mounted_from(target, NULL, 0) && unmount(target, 0) && unmount(target, MNT_FORCE))
            printf("[reg] unmount %s: %s\n", target, strerror(errno));
    }
    /* the system keeps the folder open for a moment after AppInstallAll returned */
    for (int i = 0; i < 12 && user_app_masked(); i++) {
        if (!unmount("/user/app", 0))
            break;
        pump_events(250);
    }
    if (user_app_masked() && unmount("/user/app", MNT_FORCE))
        printf("[reg] unmount /user/app: %s\n", strerror(errno));
    if (user_app_masked())
        printf("[reg] /user/app still shows only the new games\n");
    g_masked = 0;
    stage_clear();
}

/* A daemon stopped while /user/app showed only the new games: show all of them again. */
static void unmask_leftover(void) {
    static char tids[MAX_TITLES][10];
    int n = 0;
    DIR *d = opendir(STAGE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < MAX_TITLES)
        if (strlen(e->d_name) == 9)
            snprintf(tids[n++], 10, "%s", e->d_name);
    if (d)
        closedir(d);
    printf("[reg] /user/app showed only %d game(s) from an earlier run, restoring it\n", n);
    g_masked = 1;
    unmask_user_app(tids, n);
}

/* Returns 1 when /user/app now shows only the games of tids that are not registered. */
static int mask_user_app(char (*tids)[10], int n) {
    uint32_t ids[4];
    char open[4][10];
    if (g_reg_mode != REG_ALL || open_games(ids, open, 4))
        return 0; /* an open game keeps its folder in use: plain AppInstallAll */
    if (user_app_masked())
        unmask_leftover();
    stage_clear();
    mkdirs(STAGE_DIR, 0777);
    char new_tids[MAX_TITLES][10];
    int nn = 0;
    for (int i = 0; i < n && nn < MAX_TITLES; i++)
        if (!db_is_registered(tids[i])) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), STAGE_DIR "/%s", tids[i]);
            if (mkdir(p, 0777) && errno != EEXIST)
                return 0;
            snprintf(new_tids[nn++], 10, "%s", tids[i]);
        }
    if (!nn || nullfs_mount(STAGE_DIR, "/user/app"))
        return 0;
    for (int i = 0; i < nn; i++) {
        char hold[PATH_MAX], target[64];
        hold_path(hold, sizeof(hold), "app", new_tids[i]);
        snprintf(target, sizeof(target), "/user/app/%s", new_tids[i]);
        if (nullfs_mount(hold, target)) {
            unmask_user_app(new_tids, i);
            return 0;
        }
    }
    g_masked = 1;
    printf("[reg] /user/app shows only the %d new game(s) during AppInstallAll\n", nn);
    return 1;
}

static void install_all(char (*tids)[10], int n) {
    int need = 0;
    for (int i = 0; i < n; i++)
        need += !db_is_registered(tids[i]);
    if (!need)
        return;
    int masked = mask_user_app(tids, n);
    if (install_all_start(tids, n) && masked) {
        unmask_user_app(tids, n);
        masked = 0;
    }
    int done = n - need, idle = 0;
    while (done < n && idle < 30) {
        pump_events(1000);
        int now = 0;
        for (int i = 0; i < n; i++)
            now += db_is_registered(tids[i]);
        idle = now > done ? 0 : idle + 1;
        done = now;
        if (masked && !g_install_busy) {
            unmask_user_app(tids, n);
            masked = 0;
        }
    }
    /* the rows can be there before AppInstallAll returns: the other games stay hidden
     * until it does (at most two minutes) */
    for (int i = 0; masked && g_install_busy && i < 120; i++)
        pump_events(1000);
    if (masked)
        unmask_user_app(tids, n);
    printf("[reg] %s=%s0x%08x: %d of %d title(s) registered\n", reg_mode_name(), g_install_busy ? "(still running) " : "",
           g_install_rc, done, n);
}

/* The system databases are only ever written by the system: ShellCore keeps them
 * open with its own cache, and rows written from outside make it flag app.db as
 * corrupted (and rebuild it at the next boot). A title the system did not register
 * is tried again later. */
/* Logs what the system and PS4Mount have of each add-on after a registration, so a log
 * shows where an add-on the game does not see stops (reported below 6.00). */
static void log_dlc_state(const title_src_t *t) {
    for (int i = 0; i < t->ndlc; i++) {
        const pkg_t *p = &t->dlc[i].pkg;
        const char *label = p->content_id + 20;
        char link[PATH_MAX], pdb[PATH_MAX], target[64], tables[128];
        snprintf(link, sizeof(link), HOLD_DIR "/addcont/%s/%s/ac.pkg", p->title_id, label);
        snprintf(pdb, sizeof(pdb), "/user/playgo/addcont/%s/00/d0.pdb", p->content_id);
        snprintf(target, sizeof(target), "/user/addcont/%s", p->title_id);
        int rows = db_addcont_rows(p->content_id, tables, sizeof(tables));
        printf("[dlc] %s %s (%s): link %s, playgo %s, /user/addcont %s, addcont.db %d row(s)%s%s, mode %s\n",
               t->app.title_id, p->content_id, t->dlc[i].data ? "data" : "license", exists(link) ? "ok" : "MISSING",
               exists(pdb) ? "ok" : "MISSING", mounted_from(target, NULL, 0) ? "mounted" : "NOT mounted", rows,
               rows > 0 ? " in " : "", tables, reg_mode_name());
    }
}

static int register_finish(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (!db_is_registered(tid)) {
        printf("[reg] %s: not registered by the system\n", tid);
        return -1;
    }
    log_dlc_state(t);
    enforce_patch_sfo(t);
    char marker[64];
    snprintf(marker, sizeof(marker), STATE_DIR "/%s.src", tid);
    if (write_file(marker, t->marker, strlen(t->marker), 0666))
        return -1;
    manage(t);
    return 0;
}

/* Logs a skipped root file once per daemon run (any number of files). */
static int first_time(const char *path) {
    static char **seen;
    static size_t n, cap;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(seen[i], path))
            return 0;
    if (n == cap) {
        size_t ncap = cap ? cap * 2 : 64;
        char **grown = realloc(seen, ncap * sizeof(*seen));
        if (!grown)
            return 1;
        seen = grown;
        cap = ncap;
    }
    seen[n] = strdup(path);
    if (seen[n])
        n++;
    return 1;
}


/* 1 when sfo lists the add-on prefix (UP0000-CUSA00000_00) as one of its add-on services. */
static int sfo_serves(const sfo_t *sfo, const char *prefix) {
    char key[32];
    for (int i = 1; i <= 7; i++) {
        snprintf(key, sizeof(key), "SERVICE_ID_ADDCONT_ADD_%d", i);
        const sfo_item_t *it = sfo_get(sfo, key);
        if (it && !it->is_int && !strncmp(it->sval, prefix, 19))
            return 1;
    }
    return 0;
}

/* The game on this drive an add-on belongs to: same title ID, or a game (or its
 * update) that names the add-on's content ID prefix in SERVICE_ID_ADDCONT_ADD_n
 * (regional versions share add-ons this way). */
static int find_dlc_game(const char *root, const pkg_t *dlc, char *tid) {
    char lib[PATH_MAX], p[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, dlc->title_id);
    if (exists(p)) {
        snprintf(tid, 10, "%s", dlc->title_id);
        return 0;
    }
    DIR *d = opendir(lib);
    if (!d)
        return -1;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
            continue;
        static const char *names[] = {"app.pkg", "patch.pkg"};
        for (int i = 0; i < 2 && !found; i++) {
            pkg_t pkg;
            sfo_t sfo;
            snprintf(p, sizeof(p), "%s/%s/%s", lib, e->d_name, names[i]);
            if (!exists(p) || pkg_open(&pkg, p))
                continue;
            found = !read_sfo(&pkg, &sfo) && sfo_serves(&sfo, dlc->content_id);
            pkg_close(&pkg);
        }
        if (found)
            snprintf(tid, 10, "%s", e->d_name);
    }
    closedir(d);
    return found ? 0 : -1;
}

/* Add-ons left in place during one adoption run, reported in one notification. */
static int g_dlc_orphans, g_dlc_dupes;

/* Add-ons waiting for their game: checked again only when the games of the drive
 * change (the check opens the pkg of every game). */
typedef struct {
    char *path;
    unsigned long sig;
} waiting_t;

static waiting_t *g_waiting;
static size_t g_nwaiting, g_capwaiting;
static unsigned long g_lib_sig; /* games of the drive being scanned */

/* Signature of the games in <root>/ps4: changes when a game is added or removed. */
static unsigned long lib_signature(const char *root) {
    char lib[PATH_MAX], p[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    if (!d)
        return 0;
    struct dirent *e;
    unsigned long sig = 5381;
    while ((e = readdir(d))) {
        if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
            continue;
        snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, e->d_name);
        if (!exists(p))
            continue;
        unsigned long h = 0;
        for (const char *c = e->d_name; *c; c++)
            h = h * 33 + (unsigned char)*c;
        sig += h; /* order independent */
    }
    closedir(d);
    return sig;
}

static waiting_t *waiting_find(const char *path) {
    for (size_t i = 0; i < g_nwaiting; i++)
        if (!strcmp(g_waiting[i].path, path))
            return &g_waiting[i];
    return NULL;
}

static void waiting_set(const char *path, unsigned long sig) {
    waiting_t *w = waiting_find(path);
    if (w) {
        w->sig = sig;
        return;
    }
    if (g_nwaiting == g_capwaiting) {
        size_t cap = g_capwaiting ? g_capwaiting * 2 : 64;
        waiting_t *grown = realloc(g_waiting, cap * sizeof(*grown));
        if (!grown)
            return;
        g_waiting = grown;
        g_capwaiting = cap;
    }
    if ((g_waiting[g_nwaiting].path = strdup(path)))
        g_waiting[g_nwaiting++].sig = sig;
}

static void waiting_drop(const char *path) {
    waiting_t *w = waiting_find(path);
    if (!w)
        return;
    free(w->path);
    *w = g_waiting[--g_nwaiting];
}

/* Add-ons of root still waiting for their game. */
static int waiting_count(const char *root) {
    size_t l = strlen(root);
    int n = 0;
    for (size_t i = 0; i < g_nwaiting; i++)
        n += !strncmp(g_waiting[i].path, root, l) && g_waiting[i].path[l] == '/' && exists(g_waiting[i].path);
    return n;
}

/* Path of a file relative to the drive root it is on. */
static const char *drive_rel(const char *root, const char *path) {
    size_t n = strlen(root);
    return !strncmp(path, root, n) && path[n] == '/' ? path + n + 1 : path;
}

/* Records where a pkg moved into <root>/ps4/<TID>/<rel> came from. */
/* A free name for an update kept aside: replaced/patch-<time>.pkg, with -1, -2... when two
 * are kept in the same second. 0 when found. */
static int aside_name(const char *game, char *rel, size_t n) {
    char p[PATH_MAX + 64];
    snprintf(p, sizeof(p), "%s/replaced", game);
    mkdirs(p, 0777);
    long now = (long)time(NULL);
    for (int k = 0; k < 100; k++) {
        if (k)
            snprintf(rel, n, "replaced/patch-%ld-%d.pkg", now, k);
        else
            snprintf(rel, n, "replaced/patch-%ld.pkg", now);
        snprintf(p, sizeof(p), "%s/%s", game, rel);
        if (!exists(p))
            return 0;
    }
    return -1;
}

static void record_origin(const char *root, const char *tid, const char *rel, const char *from) {
    char game[PATH_MAX];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    if (origin_add(game, rel, drive_rel(root, from)))
        printf("[scan] %s: could not record where %s came from\n", game, rel);
}

/* Moves an add-on pkg (ac) into <root>/ps4/<GAME>/addcont/<ADD-ON TID>/<LABEL>/ac.pkg. */
static int adopt_dlc(const char *root, const char *name, const char *path, pkg_t *pkg) {
    char game[10], dst[PATH_MAX];
    if (find_dlc_game(root, pkg, game)) {
        if (first_time(path)) {
            /* a game served from another drive is mounted there too: only a real install counts */
            printf("[scan] %s: no game for add-on %s on the drive%s, waiting\n", name, pkg->content_id,
                   installed_internally(pkg->title_id) ? " (that title is installed on the console)" : "");
            g_dlc_orphans++;
        }
        waiting_set(path, g_lib_sig);
        return 0;
    }
    waiting_drop(path);
    snprintf(dst, sizeof(dst), "%s/ps4/%s/addcont/%s/%s", root, game, pkg->title_id, pkg->content_id + 20);
    mkdirs(dst, 0777);
    snprintf(dst, sizeof(dst), "%s/ps4/%s/addcont/%s/%s/ac.pkg", root, game, pkg->title_id, pkg->content_id + 20);
    if (exists(dst)) {
        if (first_time(path)) {
            printf("[scan] %s: %s already has this add-on, the copy was left in place\n", name, game);
            g_dlc_dupes++;
        }
        return 0;
    }
    if (rename(path, dst)) {
        printf("[scan] move %s: %s\n", path, strerror(errno));
        return 0;
    }
    printf("[scan] moved %s -> %s\n", path, dst);
    char rel[64];
    snprintf(rel, sizeof(rel), "addcont/%s/%s/ac.pkg", pkg->title_id, pkg->content_id + 20);
    record_origin(root, game, rel, path);
    return 1;
}

/* Moves PS4 game (gd) and update (gp) pkgs found in dir (the drive root or a scan
 * folder) into <root>/ps4/<TID>/app.pkg and patch.pkg, then add-ons (ac) next to
 * their game. */
static int adopt_dir_pkgs(const char *root, const char *dir) {
    int moved = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    char path[PATH_MAX], dst[PATH_MAX];
    for (int pass = 0; pass < 2; pass++, rewinddir(d)) {
    if (pass == 1)
        g_lib_sig = lib_signature(root); /* games moved in pass 0 count */
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".pkg"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat rs;
        if (!stat(path, &rs) && restored_has(RESTORED_LIST, drive_rel(root, path), (uint64_t)rs.st_size)) {
            if (first_time(path))
                printf("[scan] skip %s (moved back from ps4/ on request)\n", path);
            continue;
        }
        waiting_t *w = waiting_find(path);
        if (w && w->sig == g_lib_sig)
            continue; /* waiting add-on, and no new game since the last check */
        pkg_t pkg;
        if (pkg_open(&pkg, path)) {
            if (pass == 0 && first_time(path))
                printf("[scan] skip %s (cannot read the pkg header: not a PS4 pkg, or unreadable)\n", path);
            continue;
        }
        sfo_t sfo;
        const sfo_item_t *cat = NULL;
        if (!read_sfo(&pkg, &sfo))
            cat = sfo_get(&sfo, "CATEGORY");
        int is_app = cat && !strcmp(cat->sval, "gd"), is_patch = cat && !strcmp(cat->sval, "gp");
        int is_dlc = cat && !strcmp(cat->sval, "ac");
        if (is_dlc || pass == 1) {
            /* add-ons go in the second pass: their game may be in this folder too */
            if (pass == 1 && is_dlc && strncmp(pkg.title_id, "CUSA", 4)) {
                if (first_time(path))
                    printf("[scan] skip %s (add-on %s is not for a PS4 title)\n", path, pkg.content_id);
            } else if (pass == 1 && is_dlc) {
                if (!g_cfg.dlc) {
                    if (first_time(path))
                        printf("[scan] skip %s (add-ons are turned off)\n", path);
                } else if (pkg_fake_key(&pkg))
                    moved += adopt_dlc(root, e->d_name, path, &pkg);
                else if (first_time(path))
                    printf("[scan] skip %s (not a fake add-on pkg)\n", path);
            }
            pkg_close(&pkg);
            continue;
        }
        int ok = (is_app || is_patch) && !strncmp(pkg.title_id, "CUSA", 4) && pkg_fake_key(&pkg);
        char tid[10], ver[16] = "";
        uint8_t digest[32];
        snprintf(tid, sizeof(tid), "%s", pkg.title_id);
        memcpy(digest, pkg.digest, sizeof(digest));
        const sfo_item_t *av = ok ? sfo_get(&sfo, "APP_VER") : NULL;
        if (av && !av->is_int)
            snprintf(ver, sizeof(ver), "%s", av->sval);
        pkg_close(&pkg);
        if (!ok) {
            if (first_time(path))
                printf("[scan] skip %s (not a fake PS4 game or update pkg)\n", path);
            continue;
        }
        if (installed_internally(tid)) {
            if (first_time(path))
                printf("[scan] skip %s (%s is installed internally)\n", path, tid);
            continue;
        }
        snprintf(dst, sizeof(dst), "%s/ps4/%s", root, tid);
        mkdirs(dst, 0777);
        snprintf(dst, sizeof(dst), "%s/ps4/%s/%s", root, tid, is_app ? "app.pkg" : "patch.pkg");
        if (exists(dst)) {
            pkg_t cur;
            char cur_ver[16] = "";
            int same = !pkg_open(&cur, dst) && !memcmp(cur.digest, digest, sizeof(digest));
            if (cur.fd > 0) {
                sfo_t cs;
                const sfo_item_t *cv = read_sfo(&cur, &cs) ? NULL : sfo_get(&cs, "APP_VER");
                if (cv && !cv->is_int)
                    snprintf(cur_ver, sizeof(cur_ver), "%s", cv->sval);
                pkg_close(&cur);
            }
            if (same || is_app) {
                /* duplicates, and a second copy of a game, stay where they are */
                if (first_time(path))
                    notify(same ? "%s is already in use for %s; the copy was left in the USB root"
                                : "%s: %s already has a game package; the new one was left in the USB root",
                           e->d_name, tid);
                continue;
            }
            char game[PATH_MAX], rel[64], old[PATH_MAX + 64], was[PATH_MAX];
            snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
            if (aside_name(game, rel, sizeof(rel)))
                continue;
            snprintf(old, sizeof(old), "%s/%s", game, rel);
            if (cur_ver[0] && strcmp(ver, cur_ver) < 0) {
                /* an older update: kept aside, the newest stays in use */
                if (rename(path, old)) {
                    printf("[scan] keep %s aside: %s\n", path, strerror(errno));
                    continue;
                }
                printf("[scan] moved %s -> %s\n", path, old);
                record_origin(root, tid, rel, path);
                notify("Older update for %s (v%s) kept in ps4/%s/replaced, v%s stays in use", tid, ver, tid, cur_ver);
                moved++;
                continue;
            }
            /* a newer (or other) update: it replaces the current one, which is kept aside */
            if (rename(dst, old)) {
                printf("[scan] keep %s aside: %s\n", dst, strerror(errno));
                continue;
            }
            /* the update kept aside keeps its original name */
            if (!origin_find(game, "patch.pkg", was, sizeof(was)))
                origin_add(game, rel, was);
            notify("New update for %s (v%s): the previous one was moved to ps4/%s/replaced", tid, ver, tid);
        }
        if (rename(path, dst)) {
            printf("[scan] move %s: %s\n", path, strerror(errno));
        } else {
            printf("[scan] moved %s -> %s\n", path, dst);
            record_origin(root, tid, is_app ? "app.pkg" : "patch.pkg", path);
            moved++;
        }
    }
    }
    closedir(d);
    return moved;
}

typedef struct {
    char tid[10], name[128];
    int state, ndlc, has_patch;
} title_t;

/* A ps4/ folder or a .pkg in the root. Other partitions (like the boot partition
 * Ventoy adds to a drive) are skipped without a notification. */
static void scan_dir(char *out, size_t n, const char *root, int i) {
    if (g_cfg.paths[i][0])
        snprintf(out, n, "%s/%s", root, g_cfg.paths[i]);
    else
        snprintf(out, n, "%s", root);
}

static int has_pkg(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        found = n > 4 && !strcasecmp(e->d_name + n - 4, ".pkg");
    }
    closedir(d);
    return found;
}

/* Titles that failed to be added wait before the next try, and are given up after
 * RETRY_MAX_TRIES failures in a row: when the system refuses registrations, every
 * scan would otherwise try again, without end. */
static retry_t g_retry_items[MAX_TITLES];
static retry_table_t g_retry = {g_retry_items, MAX_TITLES};

/* Records a failed registration and tells the user the first and the last time. reason
 * is a short, plain-English sentence for why it failed this time, shown in the panel. */
static void title_failed(const char *tid, const char *name, const char *reason) {
    int tries = retry_failed(&g_retry, tid, time(NULL), reason);
    if (tries >= RETRY_MAX_TRIES) {
        printf("[reg] %s: failed %d times, given up (%s)\n", tid, tries, reason);
        notify("Failed to add %s (%s) %d times. It is not tried again until you use Add to library or Scan "
               "for games in the panel",
               name, tid, tries);
    } else {
        printf("[reg] %s: failure %d, next try in %d seconds (%s)\n", tid, tries, RETRY_SECONDS, reason);
        if (tries == 1)
            notify("Failed to add %s (%s), it is tried again in a few minutes", name, tid);
    }
}

static void halt_load(void) {
    int fd = open(HALT_FILE, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, g_halt, sizeof(g_halt) - 1);
    close(fd);
    g_halt[n > 0 ? n : 0] = 0;
    g_halt[strcspn(g_halt, "\r\n")] = 0;
    if (!g_halt[0])
        snprintf(g_halt, sizeof(g_halt), "a system database was reported as corrupted");
    printf("[safety] adding games is paused: %s\n", g_halt);
}

static void halt_set(const char *db) {
    if (g_halt[0])
        return;
    snprintf(g_halt, sizeof(g_halt), "the system reported %s as corrupted", db[0] ? db : "a database");
    write_file(HALT_FILE, g_halt, strlen(g_halt), 0666);
    printf("[safety] adding games paused: %s\n", g_halt);
    notify("PS4Mount stopped adding games: %s. Games already added still work. Restart the console, then "
           "resume in the PS4Mount panel",
           g_halt);
}

static int has_candidates(const char *root) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/ps4", root);
    if (exists(p))
        return 1;
    for (int i = 0; i < g_cfg.npaths; i++) {
        scan_dir(p, sizeof(p), root, i);
        if (has_pkg(p))
            return 1;
    }
    return 0;
}

static int adopt_root_pkgs(const char *root) {
    char dir[PATH_MAX];
    int moved = 0;
    g_dlc_orphans = g_dlc_dupes = 0;
    g_lib_sig = lib_signature(root);
    for (int i = 0; i < g_cfg.npaths; i++) {
        scan_dir(dir, sizeof(dir), root, i);
        moved += adopt_dir_pkgs(root, dir);
    }
    if (g_dlc_orphans)
        notify("%s: %d add-on(s) wait for their game, which is not on this drive (see the log)", root + 5,
               waiting_count(root));
    if (g_dlc_dupes)
        notify("%s: %d add-on copy(ies) were left in place because the game already has them", root + 5, g_dlc_dupes);
    return moved;
}

/* Add-ons deleted on the console come back with their game: drive reconnected or
 * "Add to library" in the panel. */
static void clear_removed_dlc(void) {
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d)))
        if (strlen(e->d_name) == 36 + 8 && !strcmp(e->d_name + 36, ".removed")) {
            snprintf(p, sizeof(p), STATE_DIR "/%s", e->d_name);
            unlink(p);
        }
    closedir(d);
}

/* A game already in the library: keeps it mounted and follows whether it runs. */
static void keep_mounted(title_src_t *src) {
    const char *tid = src->app.title_id;
    enforce_patch_sfo(src);
    manage(src);
    managed_t *m = managed_by(tid, NULL);
    int running = title_running(tid);
    if (m && running && !m->running) {
        m->running = 1; /* open before this daemon started (restart or crash) */
    } else if (m && !running && m->running && time(NULL) - m->running_since > 60) {
        m->running = 0; /* closed without a klog end event */
    }
    if (mount_title(src))
        notify("Failed to mount %s (%s)", src->name, tid);
}

/* What the panel shows of each game folder, from the last scan that opened its pkgs:
 * the panel asks every 5 seconds and must not read every pkg on the drives each time. */
typedef struct {
    char dir[PATH_MAX], name[128], ver[8], update_issue[256];
    int has_patch, ndlc, ndlc_all, addons_waiting;
    unsigned long long size;
} info_t;

static info_t g_info[MAX_TITLES];
static int g_info_next;

static info_t *info_find(const char *dir) {
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_info[i].dir[0] && !strcmp(g_info[i].dir, dir))
            return &g_info[i];
    return NULL;
}

static info_t *info_store(const title_src_t *t) {
    info_t *in = info_find(t->dir);
    for (int i = 0; !in && i < MAX_TITLES; i++)
        if (!g_info[i].dir[0])
            in = &g_info[i];
    if (!in) /* full: replace the oldest entries in turn */
        in = &g_info[g_info_next++ % MAX_TITLES];
    snprintf(in->dir, sizeof(in->dir), "%s", t->dir);
    snprintf(in->name, sizeof(in->name), "%s", t->name);
    snprintf(in->ver, sizeof(in->ver), "%s", t->ver);
    snprintf(in->update_issue, sizeof(in->update_issue), "%s", t->update_issue);
    in->has_patch = t->has_patch;
    in->ndlc = t->ndlc;
    in->ndlc_all = t->ndlc_all;
    in->addons_waiting = g_cfg.dlc_copy && ac_game_waiting(t);
    in->size = t->app.pkg_size + (t->has_patch ? t->patch.pkg_size : 0);
    return in;
}

/* A game in a ps4/<TID> folder gets "<title>.txt" there, so the folder can be told apart
 * on a computer. Written once: a file already there is left alone. */
static void write_name_file(const title_src_t *t) {
    size_t l = strlen(t->dir);
    if (l > 4 && !strcasecmp(t->dir + l - 4, ".pkg"))
        return; /* used where it is: its own file name already says what it is */
    char name[128], path[PATH_MAX + 130], orig[PATH_MAX];
    origin_name_file(name, sizeof(name), t->name);
    snprintf(path, sizeof(path), "%s/%s", t->dir, name);
    if (exists(path))
        return;
    buf_t b = {0};
    buf_printf(&b, "%s\r\nTitle ID: %s\r\nVersion: %s%s\r\nAdd-ons: %d\r\n", t->name, t->app.title_id, t->ver,
               t->has_patch ? " (with update)" : "", t->ndlc);
    if (!origin_find(t->dir, "app.pkg", orig, sizeof(orig)))
        buf_printf(&b, "Game pkg was: %s\r\n", orig);
    if (t->has_patch && !origin_find(t->dir, "patch.pkg", orig, sizeof(orig)))
        buf_printf(&b, "Update pkg was: %s\r\n", orig);
    if (write_buf(path, &b))
        printf("[scan] %s: %s\n", path, strerror(errno));
    buf_free(&b);
}

/* Pkgs moved back (RESTORED_LIST) are left out of the catalog of the drive being walked. */
static const char *g_walk_root;
static int g_walk_restored;

static int walk_skip(const char *path, uint64_t size) {
    return g_walk_restored && restored_has(RESTORED_LIST, drive_rel(g_walk_root, path), size);
}

/* Finds the pkgs on the drive (every folder) and groups them into games; only new or
 * changed pkgs are read, also across restarts (STATE_DIR/catalog-<drive>.txt). 0 when the
 * drive has no pkg. */
static int catalog_update(catalog_t *cat, const char *root, int fresh) {
    char file[64];
    snprintf(file, sizeof(file), STATE_DIR "/catalog-%s.txt", root + 5);
    int idx = (int)(cat - g_cat);
    if (!g_cat_loaded[idx]) {
        g_cat_loaded[idx] = 1;
        int n = catalog_load(cat, file);
        if (n > 0)
            printf("[scan] %s: %d pkg(s) known from the last run\n", root, n);
    }
    int before = cat->npkgs;
    g_walk_root = root;
    g_walk_restored = exists(RESTORED_LIST);
    const char *ignore[MAX_SCAN_PATHS];
    for (int i = 0; i < g_cfg.nignore; i++)
        ignore[i] = g_cfg.ignore[i];
    int unread = catalog_walk(cat, root, walk_skip, ignore, g_cfg.nignore);
    if (fresh && cat->npkgs)
        notify("Scanning %s for PS4 games...", root + 5);
    if (unread) {
        g_scan_changed = 1;
        g_drive_changed[idx] = 1;
        int read = catalog_read(cat);
        printf("[scan] %s: %d pkg(s), %d new or changed read\n", root, cat->npkgs, read);
    }
    if ((unread || cat->npkgs != before) && catalog_save(cat, file))
        printf("[scan] %s: could not save %s: %s\n", root, file, strerror(errno));
    if (!cat->npkgs)
        return 0;
    catalog_group(cat, update_choice);
    for (int i = 0; i < cat->npkgs; i++) {
        const cat_pkg_t *p = &cat->pkgs[i];
        if (p->state == CAT_BAD && first_time(p->path))
            printf("[scan] skip %s (not a fake PS4 game, update or add-on pkg)\n", p->path);
        else if (p->state == CAT_SHORT && first_time(p->path))
            printf("[scan] wait %s: incomplete, %llu of %llu bytes (still copying or cut off)\n", p->path,
                   (unsigned long long)p->size, (unsigned long long)p->full_size);
    }
    return 1;
}

/* First step of a scan of the drive at root: its catalog is brought up to date. 0 when
 * the drive has no pkg. */
static int scan_prepare(const char *root, int fresh, int reconnected) {
    catalog_t *cat = drive_catalog(root);
    if (!cat)
        return 0;
    if (reconnected)
        clear_removed_dlc();
    if (g_cfg.organize && has_candidates(root)) {
        /* adopt_root_pkgs() renames pkgs into place; the file manager's background copy/move
         * worker (fmgr.c) independently renames/copies/deletes files on the same drives from
         * its own thread. Both ultimately run from the single main loop thread except for that
         * detached worker, so skip adoption for this scan cycle while it is running rather than
         * race it -- has_candidates(root) stays true until something is actually adopted, so
         * this is only ever deferred to the next scan, never dropped. */
        if (fmgr_busy()) {
            printf("[scan] %s: a file operation is running; adoption deferred to the next scan\n", root);
        } else {
            int moved = adopt_root_pkgs(root);
            if (moved && !fresh)
                notify("%s: %d new package(s) found", root + 5, moved);
        }
    }
    if (!catalog_update(cat, root, fresh)) {
        if (fresh)
            printf("[scan] %s: no pkg, skipped\n", root);
        return 0;
    }
    return 1;
}

/* Second step, once every drive is prepared and linked: adds, keeps or skips its games. */
static void scan_root(const char *root, int fresh, int reconnected) {
    const char *label = root + 5; /* "usb0" / "ext0" */
    catalog_t *cat = drive_catalog(root);
    if (!cat)
        return;
    if (fresh && cat->orphans)
        notify("%s: %d add-on(s) have no game on %s", label, cat->orphans,
               g_cfg.cross ? "any connected drive" : "this drive");
    if (fresh && cat->dupes)
        notify("%s: %d game pkg(s) are second copies of a game and are not used", label, cat->dupes);
    static title_t titles[MAX_TITLES];
    int count = 0, pending = 0, paused = 0;
    for (int gi = 0; gi < cat->ngames && count < MAX_TITLES; gi++) {
        const char *tid = cat->games[gi].title_id;
        title_src_t src;
        if (src_open(&src, root, tid))
            continue;
        if (reconnected) {
            char rm[64];
            snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
            unlink(rm); /* reconnecting the USB drive brings removed games back */
        }
        title_t *t = &titles[count++];
        snprintf(t->tid, sizeof(t->tid), "%s", tid);
        snprintf(t->name, sizeof(t->name), "%s", src.name);
        t->ndlc = src.ndlc;
        t->has_patch = src.has_patch;
        const char *why;
        t->state = title_state(&src, &why);
        paused += t->state == POLICY_SKIP && !strcmp(why, POLICY_WHY_PAUSED);
        pending += t->state == POLICY_ADD;
        if (t->state == 0)
            keep_mounted(&src); /* while its pkgs are open: they are not read again */
        write_name_file(&src);
        info_store(&src);
        src_close(&src);
    }
    if (fresh && !count)
        notify("%s: no PS4 games found", label);
    else if (fresh)
        notify("%s: %d PS4 game(s) found, %d new or updated", label, count, pending);
    else if (pending)
        notify("%s: %d new or updated PS4 game(s)", label, pending);
    if (paused && fresh)
        notify("%s: %d game(s) not added: adding games is paused (%s). Resume in the PS4Mount panel", label, paused,
               g_halt);
    /* games already in the library: keep them mounted */
    for (int i = 0; i < count; i++) {
        title_t *t = &titles[i];
        title_src_t src;
        if (t->state != 0 || src_open(&src, root, t->tid))
            continue;
        enforce_patch_sfo(&src);
        manage(&src);
        managed_t *m = managed_by(t->tid, NULL);
        int running = title_running(t->tid);
        if (m && running && !m->running) {
            m->running = 1; /* open before this daemon started (restart or crash) */
        } else if (m && !running && m->running && time(NULL) - m->running_since > 60) {
            m->running = 0; /* closed without a klog end event */
        }
        if (mount_title(&src))
            notify("Failed to mount %s (%s)", src.name, t->tid);
        ac_reconcile(&src);
        src_close(&src);
    }
    if (!pending)
        return;

    /* New or changed games, in groups: one AppInstallAll per group and a quiet
     * period between groups, so the system is never flooded with registrations. */
    int size = g_cfg.batch && g_cfg.batch_size > 0 ? g_cfg.batch_size : pending;
    int groups = (pending + size - 1) / size, grouped = groups > 1, done = 0, ok = 0, group = 0;
    if (grouped)
        notify("%s: adding %d PS4 games in %d groups of up to %d", label, pending, groups, size);
    int *members = malloc(sizeof(int) * (size_t)size);
    char (*ids)[10] = malloc(10 * (size_t)size);
    int *prepared = malloc(sizeof(int) * (size_t)size);
    char *doing_tids = malloc((size_t)size * 11 + 1);
    if (!members || !ids || !prepared || !doing_tids) {
        free(members);
        free(ids);
        free(prepared);
        free(doing_tids);
        return;
    }
    for (int i = 0; i < count;) {
        int n = 0;
        for (; i < count && n < size; i++)
            if (titles[i].state == 1)
                members[n++] = i;
        if (!n || g_halt[0])
            break;
        if (group++)
            wait_db_quiet(BATCH_QUIET, 120);
        if (grouped) {
            buf_t names = {0};
            for (int k = 0; k < n; k++)
                buf_printf(&names, "%s%s", k ? ", " : "", titles[members[k]].name);
            notify("Group %d/%d, adding %d-%d of %d: %.400s", group, groups, done + 1, done + n, pending,
                   names.data ? (const char *)names.data : "");
            buf_free(&names);
        }
        int np = 0;
        for (int k = 0; k < n; k++) {
            title_t *t = &titles[members[k]];
            title_src_t src;
            prepared[k] = 0;
            if (!grouped) {
                char extra[64] = "";
                if (t->ndlc)
                    snprintf(extra, sizeof(extra), " + %d add-on%s", t->ndlc, t->ndlc > 1 ? "s" : "");
                notify("Adding %d/%d: %s%s%s (%s)", done + k + 1, pending, t->name,
                       t->has_patch ? " with update" : "", extra, t->tid);
            }
            if (src_open(&src, root, t->tid)) {
                title_failed(t->tid, t->name, "This pkg could not be read. It may be damaged or incomplete.");
            } else if (register_prepare(&src)) {
                title_failed(t->tid, t->name,
                             "The files for this game could not be prepared. Check that the drive has enough free space.");
            } else {
                prepared[k] = 1;
                snprintf(ids[np++], 10, "%s", t->tid);
            }
            if (src.app.fd > 0)
                src_close(&src);
        }
        doing_tids[0] = 0;
        for (int k = 0; k < np; k++)
            snprintf(doing_tids + strlen(doing_tids), (size_t)size * 11 + 1 - strlen(doing_tids), "%s%s",
                     k ? "," : "", ids[k]);
        int owned_adding = np && doing_mark("adding", doing_tids);
        g_adding = 1;
        install_all(ids, np);
        g_adding = 0;
        if (owned_adding)
            doing_clear("adding");
        /* own rows only after ShellCore stopped writing to its databases */
        wait_db_quiet(3, 30);
        for (int k = 0; k < n; k++) {
            title_t *t = &titles[members[k]];
            title_src_t src;
            if (!prepared[k] || src_open(&src, root, t->tid))
                continue;
            if (register_finish(&src)) {
                if (!g_halt[0]) /* paused: not the title's fault */
                    title_failed(t->tid, t->name, "The console didn't accept this game. This is usually temporary.");
            } else {
                retry_clear(&g_retry, t->tid);
                ok++;
                if (!grouped && g_cfg.dlc_copy && t->ndlc)
                    notify("%s added, its add-ons are installed next", t->name);
                else if (!grouped)
                    notify("%s ready to play", t->name);
            }
            src_close(&src);
        }
        done += n;
    }
    free(members);
    free(ids);
    free(prepared);
    free(doing_tids);
    if (ok)
        g_scan_again = 1; /* their add-on copies start at the next pass, right away */
    if (pending > 1)
        notify("%s: %d of %d game(s) added%s", label, ok, pending, ok == pending ? ", ready to play" : "");
}

/* Releases mounts over /user/app/<TID> of older versions, which mounted the drive
 * folder itself, once that folder is gone. The internal folders mounted now stay
 * while the drive is away (rest mode powers drives off): their pkg links simply
 * point to nothing until the drive is back. */
static void release_stale(void) {
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n != 13 || strcmp(e->d_name + 9, ".src"))
            continue;
        char tid[10], target[64], from[PATH_MAX], pkg_path[PATH_MAX + 16];
        snprintf(tid, sizeof(tid), "%.9s", e->d_name);
        snprintf(target, sizeof(target), "/user/app/%s", tid);
        if (!mounted_from(target, from, sizeof(from)) || strncmp(from, "/mnt/", 5))
            continue;
        snprintf(pkg_path, sizeof(pkg_path), "%s/app.pkg", from);
        if (!exists(pkg_path)) {
            printf("[scan] releasing %s (source gone)\n", target);
            managed_t *m = managed_by(tid, NULL);
            if (m) {
                unmount_title(tid, m->dlc_tids, m->ndlc_tids);
                m->used = 0;
            } else {
                unmount_title(tid, NULL, 0);
            }
        }
    }
    closedir(d);
}

/* --- drive speed --- */

/* Best read speed of each drive, keyed by its exact size: the slot (usb0...) changes with
 * the port, and nothing is written on the drives. */
#define SPEED_FILE STATE_DIR "/speed.txt"
#define SPEED_TEST_BYTES (256ULL << 20)
#define SPEED_QUICK_BYTES (64ULL << 20)
#define SPEED_SECONDS 20

enum { SPEED_NONE, SPEED_TESTING, SPEED_DONE, SPEED_FAILED };

typedef struct {
    int status;  /* SPEED_NONE, SPEED_TESTING, SPEED_DONE or SPEED_FAILED */
    int verdict; /* SPEED_DONE: policy_speed_verdict */
    double mbps, best;
    int warn;    /* plug-in check found it slower than usual: notify from the main thread */
} drive_speed_t;

typedef struct {
    int drive, quick;
    char path[PATH_MAX];
    unsigned long long size, total;
} speed_job_t;

static drive_speed_t g_speed[NUM_DRIVES];
static pthread_mutex_t g_speed_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_speed_busy;
static int g_speed_due[NUM_DRIVES]; /* plug-in check still to run */
static unsigned g_speed_seed;

static void *speed_thread(void *arg) {
    speed_job_t *job = arg;
    char key[32], value[32];
    snprintf(key, sizeof(key), "size-%llu", job->total);
    double best = 0, mbps = 0;
    if (!kv_get(SPEED_FILE, key, value, sizeof(value)))
        best = strtod(value, NULL);
    unsigned long long done = 0;
    int rc = speed_measure(job->path, job->size, job->quick ? SPEED_QUICK_BYTES : SPEED_TEST_BYTES, SPEED_SECONDS,
                           g_speed_seed++, &mbps, &done);
    int verdict = rc ? SPEED_OK : policy_speed_verdict(mbps, best);
    char root[32];
    drive_root(job->drive, root, sizeof(root));
    if (rc)
        printf("[speed] %s: test failed reading %s\n", root, job->path);
    else
        printf("[speed] %s: %.0f MB/s (%llu MB read, best %.0f MB/s)%s\n", root, mbps, done >> 20, best,
               verdict == SPEED_DROP     ? ", slower than usual"
               : verdict == SPEED_CACHED ? ", from the system cache: not used"
                                         : "");
    if (!rc && verdict != SPEED_CACHED && mbps > best) {
        snprintf(value, sizeof(value), "%.1f", mbps);
        kv_set(SPEED_FILE, key, value);
    }
    pthread_mutex_lock(&g_speed_lock);
    drive_speed_t *s = &g_speed[job->drive];
    s->status = rc || verdict == SPEED_CACHED ? SPEED_FAILED : SPEED_DONE;
    s->verdict = verdict;
    s->mbps = mbps;
    s->best = best > mbps || verdict == SPEED_CACHED ? best : mbps;
    s->warn = job->quick && !rc && verdict == SPEED_DROP;
    pthread_mutex_unlock(&g_speed_lock);
    free(job);
    g_speed_busy = 0;
    return NULL;
}

/* A game of ours is open: a test would slow it down and measure less than the drive gives. */
static int own_game_running(void) {
    refresh_sandboxes();
    for (int s = 0; s < g_nsandboxes; s++)
        for (int i = 0; i < NUM_DRIVES; i++)
            for (int k = 0; k < g_cat[i].ngames; k++)
                if (!strcmp(g_cat[i].games[k].title_id, g_sandboxes[s]))
                    return 1;
    return 0;
}

/* Starts a test of drive i on its largest readable pkg. 0 when started. */
static int speed_start(int i, int quick, char *msg, size_t cap) {
    char root[32];
    drive_root(i, root, sizeof(root));
    unsigned long long avail, total;
    if (drive_space(root, &avail, &total)) {
        snprintf(msg, cap, "%s is not connected", root + 5);
        return -1;
    }
    const cat_pkg_t *pick = NULL;
    for (int k = 0; k < g_cat[i].npkgs; k++) {
        const cat_pkg_t *p = &g_cat[i].pkgs[k];
        if (p->state == CAT_OK && (!pick || p->size > pick->size))
            pick = p;
    }
    if (!pick || pick->size < SPEED_QUICK_BYTES) {
        snprintf(msg, cap, "No pkg of 64 MB or more on %s to test with", root + 5);
        return -1;
    }
    speed_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        snprintf(msg, cap, "Out of memory");
        return -1;
    }
    job->drive = i;
    job->quick = quick;
    job->size = pick->size;
    job->total = total;
    snprintf(job->path, sizeof(job->path), "%s", pick->path);
    pthread_mutex_lock(&g_speed_lock);
    g_speed[i].status = SPEED_TESTING;
    g_speed[i].warn = 0;
    pthread_mutex_unlock(&g_speed_lock);
    g_speed_busy = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, speed_thread, job)) {
        speed_thread(job); /* no thread: test here */
    } else {
        pthread_detach(th);
    }
    snprintf(msg, cap, "Testing the speed of %s", root + 5);
    return 0;
}

/* Main loop, after each scan: forgets unplugged drives, runs the plug-in checks that are
 * due when nothing else uses the drives, and notifies a drive slower than usual. */
static void speed_poll(const int present[NUM_DRIVES], const int fresh[NUM_DRIVES]) {
    char root[32], msg[128];
    for (int i = 0; i < NUM_DRIVES; i++) {
        if (fresh[i])
            g_speed_due[i] = 1;
        pthread_mutex_lock(&g_speed_lock);
        drive_speed_t s = g_speed[i];
        g_speed[i].warn = 0;
        if (!present[i] && s.status != SPEED_TESTING)
            memset(&g_speed[i], 0, sizeof(g_speed[i]));
        pthread_mutex_unlock(&g_speed_lock);
        drive_root(i, root, sizeof(root));
        if (s.warn)
            notify("%s reads at %.0f MB/s, usually %.0f MB/s: check the cable or port", root + 5, s.mbps, s.best);
        if (!present[i])
            g_speed_due[i] = 0;
    }
    if (g_speed_busy || g_install_busy || own_game_running())
        return;
    for (int i = 0; i < NUM_DRIVES; i++)
        if (g_speed_due[i]) {
            g_speed_due[i] = 0;
            if (!speed_start(i, 1, msg, sizeof(msg)))
                return; /* one test at a time */
            printf("[speed] plug-in check skipped: %s\n", msg);
        }
}

/* defined near log_limit() below, with trash_purge_old */
static void trash_reclaim(const char *root, unsigned long long *avail, unsigned long long total);

/* 1 when a managed title is open and its pkg is on drive i: that drive's folders should
 * not be re-walked while it is in active use (I/O contention, and the point of letting a
 * drive idle is defeated by scanning it mid-game). */
static int game_running_on_drive(int i) {
    for (int k = 0; k < MAX_TITLES; k++)
        if (g_managed[k].used && g_managed[k].running && fm_drive_of(g_managed[k].dir) == i)
            return 1;
    return 0;
}

static int scan_all(void) {
    static int seen[NUM_DRIVES], first = 1;
    int ready[NUM_DRIVES] = {0}, present[NUM_DRIVES] = {0}, manual[NUM_DRIVES] = {0}, fresh[NUM_DRIVES] = {0};
    int only = g_scan_only;
    g_scan_only = -1;
    release_stale();
    g_managed_full = 0;
    g_scan_changed = 0;
    for (int i = 0; i < NUM_DRIVES; i++)
        g_drive_changed[i] = 0;
    /* pkgs of a drive move when it is walked: drop what points to other drives first */
    catalog_unlink(g_cat, NUM_DRIVES);
    char root[32];
    for (int i = 0; i < NUM_DRIVES; i++) {
        drive_root(i, root, sizeof(root));
        present[i] = is_mountpoint(root);
        manual[i] = g_manual_scan && (only < 0 || i == only);
        if (present[i] && only >= 0 && i != only && seen[i])
            /* scan of one drive: the others keep their catalog; their games are checked
             * again only when they may use pkgs of the scanned drive */
            ready[i] = g_cfg.cross && g_cat[i].ngames;
        else if (present[i]) {
            int first_or_manual = !seen[i] || manual[i];
            int speed_drop = g_speed[i].status == SPEED_DONE && g_speed[i].verdict == SPEED_DROP;
            int due = !game_running_on_drive(i) && policy_drive_walk_due(g_drive_idle[i], speed_drop);
            if (first_or_manual || due)
                ready[i] = scan_prepare(root, first_or_manual, !seen[i] && !first);
            else
                ready[i] = g_cat[i].ngames > 0; /* reuse the catalog already loaded for it */
        } else if (g_cat_loaded[i]) {
            catalog_free(&g_cat[i]); /* checked again when the drive is back: it may have changed */
            g_cat_loaded[i] = 0;
        }
    }
    catalog_link(g_cat, NUM_DRIVES, update_choice, g_cfg.cross);
    static int low_warned[NUM_DRIVES];
    for (int i = 0; i < NUM_DRIVES; i++) {
        drive_root(i, root, sizeof(root));
        if (ready[i])
            scan_root(root, !seen[i] || manual[i], !seen[i] && !first);
        unsigned long long avail = 0, total = 0;
        int have_space = present[i] && !drive_space(root, &avail, &total);
        if (have_space && policy_space_low(avail, total))
            trash_reclaim(root, &avail, total);
        int low = have_space && policy_space_low(avail, total);
        if (low && !low_warned[i]) {
            notify("%s is almost full: %.1f GB free", root + 5, avail / 1073741824.0);
            printf("[scan] %s low on space: %llu of %llu bytes free\n", root, avail, total);
        }
        low_warned[i] = low; /* warned again only after space came back */
        fresh[i] = present[i] && !seen[i];
        if (!present[i] || fresh[i] || g_drive_changed[i])
            g_drive_idle[i] = 0;
        else if (ready[i])
            g_drive_idle[i]++;
        seen[i] = present[i];
    }
    ac_submit(); /* the add-ons of every game, in one batch */
    first = 0;
    g_manual_scan = 0;
    speed_poll(present, fresh);
    return g_scan_changed;
}

/* --- klog events --- */

static int klog_connect(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(3232);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a))) {
        printf("[klog] klogsrv\n");
        return s;
    }
    if (s >= 0)
        close(s);
    int fd = open("/dev/klog", O_RDONLY);
    printf("[klog] /dev/klog fd=%d\n", fd);
    return fd;
}

static const char *after(const char *line, const char *tag) {
    const char *p = strstr(line, tag);
    return p ? p + strlen(tag) : NULL;
}

static void end_delete_window(void) {
    g_deleting = 0;
    g_deleting_close_at = 0;
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used)
            mount_paths(g_managed[i].tid, g_managed[i].has_patch, g_managed[i].dlc_tids, g_managed[i].ndlc_tids);
    printf("[delete] window closed, game folders mounted again\n");
}

/* Screens of the two delete flows: the home screen (Options > Delete) and
 * Settings > Storage. From the first of them until focus leaves both flows the game
 * folders are unmounted, so a delete removes /user/app/<TID> cleanly (a mount point
 * cannot be removed). Returns 2 for a screen that opens the window, 1 for a screen
 * that keeps it open (menus and dialogs on the way), 0 otherwise. */
static int delete_scene(const char *name) {
    static const char *opens[] = {"AppDeleteConfirmScene", "console storage delete content", "DeleteDialogScreen", NULL};
    static const char *keeps[] = {"AppDeletingScene", "ReactPopupMenu", "Noname", "Render.NPXS40021",
                                  "storage management", NULL};
    for (int i = 0; opens[i]; i++)
        if (strstr(name, opens[i]))
            return 2;
    for (int i = 0; keeps[i]; i++)
        if (strstr(name, keeps[i]))
            return 1;
    return 0;
}

static void start_delete_window(void) {
    g_deleting_since = time(NULL);
    g_deleting_close_at = 0;
    if (g_deleting)
        return;
    g_deleting = 1;
    for (int i = 0; i < MAX_TITLES; i++) {
        managed_t *m = &g_managed[i];
        if (!m->used)
            continue;
        if (m->running || title_running(m->tid))
            continue; /* still open: unmounted when it closes (see the End event) */
        unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
    }
    printf("[delete] delete screen open, game folders unmounted\n");
}

/* Copies klog lines about our titles (and crash/mount errors) into the log. */
static void trace_klog(const char *line) {
    static const char *keys[] = {"CRASH", "Trophy:Register", "_pfs_mount", "mountpfs", "PrepareProcessLaunch",
                                 "preLaunchCheck", "scePlayGoCore", "process_mount", "process_check", "sqlite",
                                 "SQLITE", "corrupt", "Corrupt", "CORRUPT", "dbr", "DBR", "AddcontInfo",
                                 "Addcont", "addcont", NULL};
    for (int i = 0; keys[i]; i++)
        if (strstr(line, keys[i])) {
            printf("[klog] %.300s\n", line);
            return;
        }
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && strstr(line, g_managed[i].tid) && !strstr(line, "BGLayer") && !strstr(line, "PSM.UI")) {
            printf("[klog] %.300s\n", line);
            return;
        }
}

/* "system file disagree [<TID>]": the system refused to start a game with an update.
 * Removing the game from the library and adding it again made it start; this is done
 * once per title and daemon run, so a game that still fails is not removed again. */
static char g_readd_tid[10];
static char g_readd_done[MAX_TITLES][10];
static int g_nreadd_done;

static void disagree_seen(const char *tid) {
    managed_t *m = managed_by(tid, NULL);
    if (!m)
        return;
    m->running = 0; /* the launch was refused */
    for (int i = 0; i < g_nreadd_done; i++)
        if (!strcmp(g_readd_done[i], tid)) {
            printf("[reg] %s: system file disagree again, it was already added again once\n", tid);
            return;
        }
    if (m->has_patch && !g_readd_tid[0])
        snprintf(g_readd_tid, sizeof(g_readd_tid), "%s", tid);
}

static void handle_klog_line(const char *line) {
    const char *p;
    char id[40], db[64];
    trace_klog(line);
    if (safety_db_corrupt(line, db, sizeof(db)))
        halt_set(db);
    if ((p = after(line, "OnFocusActiveSceneChanged ["))) {
        /* "[from : type] -> [to : type]": follow the delete screens */
        const char *to = strstr(p, "] -> [");
        if (g_deleting)
            printf("[delete] screen %.200s\n", p);
        if (to) {
            to += 6;
            int kind = delete_scene(to);
            if (kind == 2 || (kind == 1 && g_deleting))
                start_delete_window();
            else if (!kind && g_deleting)
                end_delete_window();
        }
    } else if ((p = after(line, "launchApp("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 1;
            m->running_since = time(NULL);
        }
    } else if ((p = after(line, "system file disagree ["))) {
        snprintf(id, sizeof(id), "%.9s", p);
        disagree_seen(id);
    } else if (strstr(line, "[BGFT/Telemetry] SendDownloadCopy")) {
        ac_copy_event(line);
    } else if ((p = after(line, "[PlayGoService][End] end ("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 0;
            if (g_deleting) {
                /* closed by a delete: take the folder away before the uninstall runs */
                unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
                printf("[delete] %s closed, game folder unmounted\n", m->tid);
            }
        }
    } else if ((p = after(line, "[PlayGoCore][Uninstall] end ("))) {
        snprintf(id, sizeof(id), "%.36s", p);
        managed_t *m = managed_by(NULL, id);
        if (m) {
            char path[64];
            snprintf(path, sizeof(path), STATE_DIR "/%s.removed", m->tid);
            write_file(path, m->dir, strlen(m->dir), 0666);
            snprintf(path, sizeof(path), STATE_DIR "/%s.src", m->tid);
            unlink(path);
            char hold[PATH_MAX];
            hold_path(hold, sizeof(hold), "app", m->tid);
            rm_tree(hold);
            hold_path(hold, sizeof(hold), "patch", m->tid);
            rm_tree(hold);
            ac_drop_game(m->tid);
            notify("%s removed from the library. The pkg stays on the USB drive.", m->name);
            m->used = 0;
        } else if (strlen(id) == 36 && g_deleting && !g_cfg.dlc_copy) {
            /* an add-on of one of our games deleted on a delete screen: keep it out until the
             * drive is reconnected. Outside those screens the uninstall is the system's own:
             * registering a game again uninstalls its add-ons' PlayGo data, and taking that
             * for a delete removed the add-ons of the game every time a game was added. */
            for (int i = 0; i < MAX_TITLES; i++) {
                managed_t *g = &g_managed[i];
                for (int j = 0; g->used && j < g->ndlc_tids; j++) {
                    if (strncmp(id + 7, g->dlc_tids[j], 9))
                        continue;
                    char path[96], hold[PATH_MAX];
                    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", id);
                    write_file(path, id, strlen(id), 0666);
                    snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%.9s/%s", id + 7, id + 20);
                    rm_tree(hold);
                    printf("[delete] add-on %s removed\n", id);
                    i = MAX_TITLES;
                    break;
                }
            }
        }
        /* the delete is done: mount the other games again shortly after */
        if (g_deleting)
            g_deleting_close_at = time(NULL) + 3;
    }
}

/* --- web panel --- */

static time_t g_next_scan;
static int g_stop;

int app_rescan(const char *drive, char *msg, size_t cap) {
    int only = -1;
    if (drive) {
        char root[32];
        for (int i = 0; i < NUM_DRIVES && only < 0; i++) {
            drive_root(i, root, sizeof(root));
            if (!strcmp(root + 5, drive))
                only = i;
        }
        if (only < 0 || !is_mountpoint(root)) {
            snprintf(msg, cap, "%s is not connected", drive);
            return -1;
        }
    }
    g_scan_only = only;
    g_next_scan = 0;
    g_manual_scan = 1;
    retry_clear_given_up(&g_retry);
    if (drive)
        snprintf(msg, cap, "Scanning %s for games", drive);
    else
        snprintf(msg, cap, "Scanning USB drives for games");
    return 0;
}

int app_speedtest(const char *drive, char *msg, size_t cap) {
    char root[32];
    int i = 0;
    for (; i < NUM_DRIVES; i++) {
        drive_root(i, root, sizeof(root));
        if (!strcmp(root + 5, drive))
            break;
    }
    if (i == NUM_DRIVES || !is_mountpoint(root)) {
        snprintf(msg, cap, "%s is not connected", drive);
        return -1;
    }
    if (g_speed_busy) {
        snprintf(msg, cap, "A speed test is already running");
        return -1;
    }
    if (g_install_busy) {
        snprintf(msg, cap, "Games are being added: test again when they are ready");
        return -1;
    }
    if (own_game_running()) {
        snprintf(msg, cap, "Close the game first: a test would slow it down");
        return -1;
    }
    g_speed_due[i] = 0;
    return speed_start(i, 0, msg, cap);
}

/* The panel's speed fields of drive i. */
static void speed_json(buf_t *out, int i) {
    pthread_mutex_lock(&g_speed_lock);
    drive_speed_t s = g_speed[i];
    pthread_mutex_unlock(&g_speed_lock);
    const char *state = s.status == SPEED_TESTING ? "testing"
                        : s.status == SPEED_FAILED ? "failed"
                        : s.status != SPEED_DONE   ? "none"
                        : s.verdict == SPEED_DROP  ? "slow"
                        : s.verdict == SPEED_USB2  ? "usb2"
                                                   : "ok";
    buf_printf(out, ",\"speed_state\":\"%s\",\"speed\":%.0f,\"best\":%.0f", state, s.mbps, s.best);
}

int app_resume(char *msg, size_t cap) {
    if (!g_halt[0]) {
        snprintf(msg, cap, "Adding games is not paused");
        return -1;
    }
    unlink(HALT_FILE);
    g_halt[0] = 0;
    retry_clear_given_up(&g_retry);
    g_next_scan = 0;
    printf("[safety] adding games resumed from the panel\n");
    snprintf(msg, cap, "Adding games resumed");
    return 0;
}
/* The name of the open game, or 0 when no game is open. */
static int open_game_name(char *name, size_t cap) {
    uint32_t ids[1];
    char tids[1][10];
    if (!open_games(ids, tids, 1))
        return 0;
    managed_t *m = managed_by(tids[0], NULL);
    snprintf(name, cap, "%s", m && m->name[0] ? m->name : tids[0]);
    return 1;
}

int app_stop_open(char *name, size_t cap) { return open_game_name(name, cap); }

int app_stop(char *msg, size_t cap) {
    char name[128];
    if (open_game_name(name, sizeof(name))) {
        snprintf(msg, cap, "Close %s before stopping PS4Mount", name);
        printf("[panel] stop refused: %s is open\n", name);
        return -1;
    }
    g_stop = 1;
    snprintf(msg, cap, "PS4Mount unmounts the games and stops");
    return 0;
}

static int still_mounted(const char *target) { return mounted_from(target, NULL, 0) || layers_on(target) > 0; }

/* Rest mode powers the drive off: before stopping, every folder PS4Mount mounted is
 * unmounted, without forcing (a forced unmount of a folder in use can bring the system
 * down). 0 when none is left; otherwise -1 with the reason in msg, and the games are
 * mounted again. */
static int unmount_games(char *msg, size_t cap) {
    char name[128], target[64];
    if (open_game_name(name, sizeof(name))) {
        snprintf(msg, cap, "Close %s before stopping PS4Mount", name);
        return -1;
    }
    int busy = 0;
    for (int i = 0; i < MAX_TITLES; i++) {
        managed_t *m = &g_managed[i];
        if (!m->used)
            continue;
        snprintf(target, sizeof(target), "/user/app/%s", m->tid);
        unmount_all_layers(target, 0);
        busy += still_mounted(target);
        snprintf(target, sizeof(target), "/user/patch/%s", m->tid);
        unmount_all_layers(target, 0);
        busy += still_mounted(target);
        for (int j = 0; j < m->ndlc_tids; j++) {
            snprintf(target, sizeof(target), "/user/addcont/%s", m->dlc_tids[j]);
            unmount_all_layers(target, 0);
            busy += still_mounted(target);
        }
    }
    if (!busy)
        return 0;
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used)
            mount_paths(g_managed[i].tid, g_managed[i].has_patch, g_managed[i].dlc_tids, g_managed[i].ndlc_tids);
    snprintf(msg, cap, "%d game folder(s) are still in use, try again in a moment", busy);
    return -1;
}

static void json_str(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_put(b, "\\", 1);
        if ((unsigned char)*s >= 0x20)
            buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

/* Finds the game pkg of tid in the catalogs of the connected drives. */
static int find_game(const char *tid, char *root, size_t n, char *app_path, size_t an) {
    for (int i = 0; i < NUM_DRIVES; i++) {
        char r[32];
        drive_root(i, r, sizeof(r));
        const cat_game_t *g = catalog_game(&g_cat[i], tid);
        if (g && is_mountpoint(r)) {
            snprintf(root, n, "%s", r);
            snprintf(app_path, an, "%s", g_cat[i].pkgs[g->app].path);
            return 0;
        }
    }
    return -1;
}

/* The drive serving tid's registered copy right now, read from the .src marker
 * register_finish() writes for every game and relocate() keeps current: its first line is
 * the game's real location (<root>/ps4/<tid> only for a game PS4Mount organized into that
 * folder; the game's own pkg path otherwise, see game_location()), so the drive is its
 * first path component. -1 when the marker is missing or that drive is gone. */
static int find_root(const char *tid, char *root, size_t n) {
    char path[64], buf[PATH_MAX + 256] = {0};
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0 || strncmp(buf, "/mnt/", 5))
        return -1;
    buf[strcspn(buf, "\n")] = 0;
    const char *slash = strchr(buf + 5, '/');
    size_t len = slash ? (size_t)(slash - buf) : strlen(buf);
    if (len >= n)
        return -1;
    memcpy(root, buf, len);
    root[len] = 0;
    return is_mountpoint(root) ? 0 : -1;
}

/* Panel details of a folder no scan has opened yet (the scan is paused while a
 * delete screen is open). */
static const info_t *info_load(const char *root, const char *tid) {
    title_src_t *t = malloc(sizeof(*t));
    if (!t || src_open(t, root, tid)) {
        free(t);
        return NULL;
    }
    const info_t *in = info_store(t);
    src_close(t);
    free(t);
    return in;
}

/* The add-ons of g for the panel: content ID, name and state, "on" (in place for the
 * console), "adding", "off" (turned off in the panel) or "deleted" (deleted on the console). */
static void addons_json(buf_t *out, const catalog_t *cat, const cat_game_t *g) {
    const cat_pkg_t *list[CAT_MAX_DLC];
    int n = catalog_dlc_of(g_cat, cat, g, list, CAT_MAX_DLC);
    for (int k = 0; k < n; k++) {
        const cat_pkg_t *p = list[k];
        int dup = 0;
        for (int j = 0; j < k; j++)
            dup |= !strcmp(list[j]->content_id, p->content_id);
        if (dup)
            continue;
        char path[PATH_MAX];
        const char *state = "adding";
        snprintf(path, sizeof(path), STATE_DIR "/%s.removed", p->content_id);
        if (addon_off(p->content_id))
            state = "off";
        else if (exists(path))
            state = "deleted";
        else {
            snprintf(path, sizeof(path), HOLD_DIR "/addcont/%s/%s/ac.pkg", p->title_id, p->content_id + 20);
            if (exists(path))
                state = "on";
        }
        buf_printf(out, "%s{\"content_id\":\"%s\",\"name\":", k ? "," : "", p->content_id);
        json_str(out, p->name);
        buf_printf(out, ",\"state\":\"%s\",\"other_drive\":%s}", state, k >= g->ndlc ? "true" : "false");
    }
}

/* --- updates and add-ons chosen in the panel, starting a game --- */

int app_game_json(const char *tid, buf_t *out, char *msg, size_t cap) {
    char root[32];
    title_src_t t;
    if (find_root(tid, root, sizeof(root)) || src_open(&t, root, tid)) {
        snprintf(msg, cap, "%s is not on any connected drive", tid);
        return -1;
    }
    const catalog_t *cat = drive_catalog(root);
    const cat_game_t *g = cat ? catalog_game(cat, tid) : NULL;
    const cat_pkg_t *ups[2 * CAT_MAX_UPDATES];
    int nu = g ? catalog_updates_of(g_cat, cat, g, ups, 2 * CAT_MAX_UPDATES) : 0;
    const cat_pkg_t *used = g ? catalog_patch(g_cat, cat, g) : NULL;
    buf_printf(out, "{\"title_id\":\"%s\",\"name\":", tid);
    json_str(out, t.name);
    buf_printf(out, ",\"dlc_limit\":%d,\"updates\":[", DLC_LIMIT);
    for (int i = 0; i < nu; i++) {
        const char *base = strrchr(ups[i]->path, '/');
        buf_printf(out, "%s{\"file\":", i ? "," : "");
        json_str(out, ups[i]->app_ver); /* app_set_update() takes a version, not a path */
        buf_printf(out, ",\"version\":");
        json_str(out, ups[i]->app_ver);
        buf_printf(out, ",\"name\":");
        json_str(out, base ? base + 1 : ups[i]->path);
        buf_printf(out, ",\"active\":%s}", ups[i] == used ? "true" : "false");
    }
    buf_printf(out, "],\"dlcs\":[");
    for (int i = 0; i < t.ndlc_all; i++) {
        const sfo_item_t *n = sfo_get(&t.dlc[i].sfo, "TITLE");
        buf_printf(out, "%s{\"content_id\":\"%s\",\"name\":", i ? "," : "", t.dlc[i].pkg.content_id);
        json_str(out, n && !n->is_int ? n->sval : t.dlc[i].pkg.content_id + 20);
        buf_printf(out, ",\"on\":%s,\"data\":%s", i < t.ndlc ? "true" : "false", t.dlc[i].data ? "true" : "false");
        char why[128] = "";
        buf_printf(out, ",\"copy\":\"%s\",\"reason\":", ac_status(&t.dlc[i], i < t.ndlc, why, sizeof(why)));
        json_str(out, why);
        buf_printf(out, "}");
    }
    buf_printf(out, "]}");
    src_close(&t);
    return 0;
}

static int game_open(const char *tid) {
    managed_t *m = managed_by(tid, NULL);
    return (m && m->running) || title_running(tid);
}

/* Changes asked in the panel, made by the main loop between scans. */
static char g_dlc_tid[10], *g_dlc_on;

/* The game page's Updates dialog: rel is the version chosen, validated the same way the
 * quick toggle's version= query is (see version_param() in web.c). app_set_update() is
 * all either one needs -- no file is ever touched, the next scan picks whichever
 * discovered update pkg has this version, wherever it is. */
int app_choose_update(const char *tid, const char *rel, char *msg, size_t cap) {
    size_t l = strlen(rel);
    if (l && strcmp(rel, CAT_NO_UPDATE)) {
        if (l != 5 || rel[2] != '.') {
            snprintf(msg, cap, "invalid update version");
            return -1;
        }
        for (size_t i = 0; i < l; i++)
            if (i != 2 && (rel[i] < '0' || rel[i] > '9')) {
                snprintf(msg, cap, "invalid update version");
                return -1;
            }
    }
    return app_set_update(tid, rel, msg, cap);
}

/* on: content IDs of the add-ons to keep on, comma separated (empty: all off). */
int app_choose_dlc(const char *tid, const char *on, char *msg, size_t cap) {
    char root[32];
    int n = 0;
    for (const char *p = on; *p;) {
        size_t len = strcspn(p, ",");
        if (len != 36 || strspn(p, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") < 36) {
            snprintf(msg, cap, "invalid add-on: %.40s", p);
            return -1;
        }
        n++;
        p += len;
        if (*p == ',')
            p++;
    }
    if (n > DLC_LIMIT) {
        snprintf(msg, cap, "At most %d add-ons can be on", DLC_LIMIT);
        return -1;
    }
    if (find_root(tid, root, sizeof(root))) {
        snprintf(msg, cap, "%s is not on any connected drive", tid);
        return -1;
    }
    if (game_open(tid)) {
        snprintf(msg, cap, "Close the game before changing its add-ons");
        return -1;
    }
    if (g_dlc_tid[0]) {
        snprintf(msg, cap, "Another change is being made, try again in a moment");
        return -1;
    }
    free(g_dlc_on);
    if (!(g_dlc_on = strdup(on))) {
        snprintf(msg, cap, "out of memory");
        return -1;
    }
    snprintf(g_dlc_tid, sizeof(g_dlc_tid), "%s", tid);
    snprintf(msg, cap, "Changing the add-ons of %s: the game is added again with them", tid);
    return 0;
}

/* The add-ons not in on are turned off (/data/ps4mount/addons-off.txt, shared with the
 * quick toggle in app_addon). */
static void apply_dlc_choice(const char *tid, const char *on) {
    char root[32];
    title_src_t t;
    if (find_root(tid, root, sizeof(root)) || game_open(tid) || src_open(&t, root, tid))
        return;
    char *list = malloc(strlen(on) + 3);
    if (list) {
        snprintf(list, strlen(on) + 3, "\n%s\n", on);
        for (char *c = list; *c; c++)
            if (*c == ',')
                *c = '\n';
        for (int i = 0; i < t.ndlc_all; i++) {
            int keep_on = dlc_listed(list, t.dlc[i].pkg.content_id);
            kv_set(ADDONS_OFF_FILE, t.dlc[i].pkg.content_id, keep_on ? NULL : "off");
            if (keep_on && i >= t.ndlc)
                acfail_clear(STATE_DIR, t.dlc[i].pkg.content_id); /* turned on again: tried anew */
        }
        printf("[panel] %s: add-ons changed\n", tid);
        notify("%s: add-ons changed, adding the game again", tid);
    }
    free(list);
    src_close(&t);
}

static int open_games(uint32_t *ids, char (*tids)[10], int max);
static int close_open_games(const char *why);
static char g_launch_tid[10];

/* Starts a game of the library for the foreground user. Starting a game uses
 * sceSystemServiceLaunchApp, the call other PS5 homebrew launchers use
 * (ps5-payload-dev/websrv); an LncUtil-based lookup does not work on firmware 11.20. */
static int launch_now(const char *tid, char *msg, size_t cap) {
    launch_ctx_t ctx = {0};
    if (sceUserServiceGetForegroundUser(&ctx.user_id)) {
        snprintf(msg, cap, "No user is logged in on the console");
        return -1;
    }
    ctx.structsize = sizeof(ctx);
    kv_set(LAUNCH_HELD_FILE, tid, NULL); /* pressing Play again is the user's own "try again";
                                          * cleared only once a launch is genuinely attempted */
    char *noargs[] = {NULL};
    int owned_launching = doing_mark("launching", tid);
    int rc = sceSystemServiceLaunchApp(tid, noargs, &ctx);
    if (owned_launching)
        doing_clear("launching");
    printf("[panel] launch %s: rc=0x%08x user=%u\n", tid, rc, ctx.user_id);
    if (rc < 0) {
        snprintf(msg, cap, "Failed to start %s (0x%08x)", tid, rc);
        return -1;
    }
    snprintf(msg, cap, "Starting %s", tid);
    return 0;
}

/* 1 (msg names the open game) when another game is open and force is 0: the panel asks
 * first, since that game is closed without saving. With force, the open game is closed by
 * the main loop, which then starts this one. */
int app_launch(const char *tid, int force, char *msg, size_t cap) {
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    if (!db_is_registered(tid) || !mounted_from(target, NULL, 0)) {
        snprintf(msg, cap, "%s is not ready to play", tid);
        return -1;
    }
    if (g_install_busy || g_adding) {
        snprintf(msg, cap, "Games are being added; try again in a moment");
        return -1;
    }
    uint32_t ids[8];
    char tids[8][10];
    int n = open_games(ids, tids, 8);
    if (!n)
        return launch_now(tid, msg, cap);
    if (!strcmp(tids[0], tid)) {
        snprintf(msg, cap, "%s is already open", tid);
        return -1;
    }
    managed_t *m = managed_by(tids[0], NULL);
    if (!force) {
        snprintf(msg, cap, "%s", m ? m->name : tids[0]);
        return 1;
    }
    if (g_launch_tid[0]) {
        snprintf(msg, cap, "Another game is being started, try again in a moment");
        return -1;
    }
    snprintf(g_launch_tid, sizeof(g_launch_tid), "%s", tid);
    snprintf(msg, cap, "Closing %s, then starting %s", m ? m->name : tids[0], tid);
    return 0;
}

/* The deferred start: closes the open game and waits until the system let go of its files. */
static void launch_after_close(const char *tid) {
    char msg[128];
    if (close_open_games("to start another game")) {
        notify("The open game did not close; %s was not started", tid);
        return;
    }
    if (launch_now(tid, msg, sizeof(msg)))
        notify("%s", msg);
}

void app_games_json(buf_t *out) {
    buf_printf(out, "{\"version\":");
    json_str(out, VERSION);
    buf_printf(out, ",\"paused\":");
    json_str(out, g_halt);
    buf_printf(out, ",\"games_capped\":%d,\"games\":[", g_managed_full);
    int first = 1;
    for (int i = 0; i < NUM_DRIVES; i++) {
        char root[32];
        drive_root(i, root, sizeof(root));
        const catalog_t *cat = &g_cat[i];
        if (!cat->ngames || !is_mountpoint(root))
            continue;
        for (int gi = 0; gi < cat->ngames; gi++) {
            const char *tid = cat->games[gi].title_id, *state, *mount = "";
            char dir[PATH_MAX];
            game_location(root, tid, cat->pkgs[cat->games[gi].app].path, dir, sizeof(dir));
            const info_t *in = info_find(dir);
            if (!in && !(in = info_load(root, tid)))
                continue;
            char target[64], from[PATH_MAX];
            snprintf(target, sizeof(target), "/user/app/%s", tid);
            int mounted = mounted_from(target, from, sizeof(from));
            if (mounted) {
                struct statfs sf;
                statfs(target, &sf);
                mount = sf.f_flags & MNT_RDONLY ? "ro" : "rw";
            }
            managed_t *m = managed_by(tid, NULL);
            long retry_in = retry_remaining(&g_retry, tid, time(NULL));
            panel_facts_t pf = {0};
            pf.internal = installed_internally(tid);
            pf.elsewhere = !pf.internal && served_elsewhere(tid, dir);
            pf.removed = removed(tid);
            pf.excluded = excluded(tid);
            pf.running = m && m->running;
            pf.mounted = mounted;
            pf.registered = db_is_registered(tid);
            pf.given_up = retry_given_up(&g_retry, tid);
            pf.retrying = retry_in > 0;
            pf.installing_addons = in->addons_waiting;
            state = policy_panel_state(&pf);
            buf_printf(out, "%s{\"title_id\":\"%s\",\"name\":", first ? "" : ",", tid);
            json_str(out, in->name);
            buf_printf(out, ",\"version\":");
            json_str(out, in->ver);
            buf_printf(out, ",\"has_patch\":%s,\"size\":%llu,\"state\":\"%s\",\"mount\":\"%s\",\"source\":",
                       in->has_patch ? "true" : "false", in->size, state, mount);
            json_str(out, in->dir);
            buf_printf(out, ",\"dlc\":%d,\"dlc_total\":%d,\"update_issue\":", in->ndlc, in->ndlc_all);
            json_str(out, in->update_issue);
            buf_printf(out, ",\"retry_in\":%ld,\"fail_reason\":", retry_in);
            json_str(out, retry_reason(&g_retry, tid));
            const cat_game_t *g = &cat->games[gi];
            const char *choice = update_choice(tid);
            buf_printf(out, ",\"drive\":\"%s\",\"update_choice\":", root + 5);
            json_str(out, choice ? choice : "");
            const cat_pkg_t *ups[2 * CAT_MAX_UPDATES], *used = catalog_patch(g_cat, cat, g);
            int nups = catalog_updates_of(g_cat, cat, g, ups, 2 * CAT_MAX_UPDATES);
            buf_printf(out, ",\"update_other_drive\":%s,\"updates\":[", used && g->patch < 0 ? "true" : "false");
            for (int k = 0; k < nups; k++) {
                buf_printf(out, "%s", k ? "," : "");
                json_str(out, ups[k]->app_ver);
            }
            buf_printf(out, "],\"addons\":[");
            addons_json(out, cat, g);
            buf_printf(out, "]");
            char held[2];
            buf_printf(out, ",\"launch_held\":%s", kv_get(LAUNCH_HELD_FILE, tid, held, sizeof(held)) ? "false" : "true");
            buf_printf(out, "}");
            first = 0;
        }
    }
    buf_printf(out, "],\"drives\":[");
    first = 1;
    for (int i = 0; i < NUM_DRIVES; i++) {
        char root[32];
        drive_root(i, root, sizeof(root));
        unsigned long long avail, total;
        if (!is_mountpoint(root) || drive_space(root, &avail, &total))
            continue;
        buf_printf(out, "%s{\"name\":\"%s\",\"games\":%d,\"pkgs\":%d,\"bad\":%d,\"free\":%llu,\"total\":%llu,\"low\":%s",
                   first ? "" : ",", root + 5, g_cat[i].ngames, g_cat[i].npkgs, catalog_bad_count(&g_cat[i]), avail,
                   total, policy_space_low(avail, total) ? "true" : "false");
        speed_json(out, i);
        buf_printf(out, "}");
        first = 0;
    }
    buf_printf(out, "]}");
}

#define MAX_PROBLEMS 500

void app_problems_json(buf_t *out) {
    static const struct {
        int role;
        const char *kind;
    } kinds[] = {{CAT_ROLE_DUPE, "duplicate"}, {CAT_ROLE_ORPHAN, "addon-without-game"},
                 {CAT_ROLE_NOGAME, "update-without-game"}};
    buf_printf(out, "{\"problems\":[");
    int n = 0;
    for (int i = 0; i < NUM_DRIVES && n < MAX_PROBLEMS; i++) {
        char root[32];
        drive_root(i, root, sizeof(root));
        const catalog_t *c = &g_cat[i];
        if (!c->npkgs || !is_mountpoint(root))
            continue;
        for (int k = 0; k < c->npkgs && n < MAX_PROBLEMS; k++) {
            const cat_pkg_t *p = &c->pkgs[k];
            const char *kind = p->state == CAT_BAD ? "unusable" : p->state == CAT_SHORT ? "incomplete" : NULL;
            for (size_t j = 0; !kind && j < sizeof(kinds) / sizeof(kinds[0]); j++)
                if (p->role == kinds[j].role)
                    kind = kinds[j].kind;
            if (!kind)
                continue;
            buf_printf(out, "%s{\"drive\":\"%s\",\"kind\":\"%s\",\"title_id\":", n ? "," : "", root + 5, kind);
            json_str(out, p->title_id);
            buf_printf(out, ",\"path\":");
            json_str(out, p->path);
            buf_printf(out, ",\"size\":%llu,\"full_size\":%llu}", (unsigned long long)p->size,
                       (unsigned long long)p->full_size);
            n++;
        }
    }
    buf_printf(out, "]}");
}

int app_addon(const char *content_id, int on, char *msg, size_t cap) {
    char tid[10] = "";
    for (int i = 0; i < NUM_DRIVES && !tid[0]; i++) {
        const catalog_t *c = &g_cat[i];
        for (int gi = 0; gi < c->ngames && !tid[0]; gi++) {
            const cat_pkg_t *list[CAT_MAX_DLC];
            int n = catalog_dlc_of(g_cat, c, &c->games[gi], list, CAT_MAX_DLC);
            for (int k = 0; k < n; k++)
                if (!strcmp(list[k]->content_id, content_id)) {
                    snprintf(tid, sizeof(tid), "%s", c->games[gi].title_id);
                    break;
                }
        }
    }
    if (!tid[0]) {
        snprintf(msg, cap, "This add-on is not on any connected drive");
        return -1;
    }
    managed_t *m = managed_by(tid, NULL);
    if (m && m->running) {
        snprintf(msg, cap, "Close the game first");
        return -1;
    }
    if (kv_set(ADDONS_OFF_FILE, content_id, on ? NULL : "off")) {
        snprintf(msg, cap, "Could not save the add-on choice");
        return -1;
    }
    addons_off_load();
    if (on) {
        char rm[96];
        snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", content_id);
        unlink(rm); /* deleted on the console: install it again */
    } else {
        char hold[PATH_MAX];
        snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%.9s/%s", content_id + 7, content_id + 20);
        rm_tree(hold);
    }
    retry_clear(&g_retry, tid);
    g_next_scan = 0;
    printf("[web] add-on %s of %s turned %s\n", content_id, tid, on ? "on" : "off");
    snprintf(msg, cap, on ? "The add-on will be installed again" : "The add-on is turned off; the game keeps the others");
    return 0;
}

int app_set_update(const char *tid, const char *ver, char *msg, size_t cap) {
    managed_t *m = managed_by(tid, NULL);
    if ((m && m->running) || title_running(tid)) {
        printf("[web] update choice of %s refused: game running\n", tid);
        snprintf(msg, cap, "Close the game first");
        return -1;
    }
    if (kv_set(UPDATES_FILE, tid, ver[0] ? ver : NULL)) {
        snprintf(msg, cap, "Could not save the update choice");
        return -1;
    }
    updates_load();
    retry_clear(&g_retry, tid);
    g_next_scan = 0;
    printf("[web] %s: update %s\n", tid, ver[0] ? ver : "highest");
    if (!ver[0])
        snprintf(msg, cap, "%s will use its highest update", tid);
    else if (!strcmp(ver, CAT_NO_UPDATE))
        snprintf(msg, cap, "%s will run without update", tid);
    else
        snprintf(msg, cap, "%s will use update %s", tid, ver);
    return 0;
}

int app_icon(const char *tid, buf_t *out) {
    char p[PATH_MAX], root[32];
    snprintf(p, sizeof(p), "/user/appmeta/%s/icon0.png", tid);
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        char tmp[65536];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof(tmp))) > 0)
            buf_put(out, tmp, (size_t)n);
        close(fd);
        return out->len ? 0 : -1;
    }
    if (find_game(tid, root, sizeof(root), p, sizeof(p)))
        return -1;
    pkg_t pkg;
    if (pkg_open(&pkg, p))
        return -1;
    size_t len;
    uint8_t *png = pkg_entry(&pkg, 0x1200, &len);
    pkg_close(&pkg);
    if (!png)
        return -1;
    buf_put(out, png, len);
    free(png);
    return 0;
}

int app_add(const char *tid, char *msg, size_t cap) {
    char rm[64], root[32], app_path[PATH_MAX];
    if (find_game(tid, root, sizeof(root), app_path, sizeof(app_path))) {
        snprintf(msg, cap, "%s is not on any connected USB drive", tid);
        return -1;
    }
    snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
    unlink(rm);
    clear_removed_dlc();
    retry_clear(&g_retry, tid);
    g_next_scan = 0;
    snprintf(msg, cap, "Adding %s to the library", tid);
    return 0;
}

/* sceAppInstUtilAppUnInstall in its own thread, waited for at most UNINSTALL_WAIT seconds:
 * a call that never returns (as InstallAll did after rest mode) must not freeze PS4Mount.
 * The wait does not serve the panel (this also runs from a panel request). */
#define UNINSTALL_WAIT 60

static volatile int g_uninstall_busy, g_uninstall_rc;
static char g_uninstall_tid[10];

static void *uninstall_thread(void *arg) {
    (void)arg;
    g_uninstall_rc = sceAppInstUtilAppUnInstall(g_uninstall_tid);
    g_uninstall_busy = 0;
    return NULL;
}

/* 0 or the system error; -2 while an earlier call is still running or this one timed out. */
static int uninstall_title(const char *tid) {
    if (g_uninstall_busy) {
        printf("[remove] %s: an earlier uninstall (%s) is still running\n", tid, g_uninstall_tid);
        return -2;
    }
    snprintf(g_uninstall_tid, sizeof(g_uninstall_tid), "%s", tid);
    g_uninstall_busy = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, uninstall_thread, NULL)) {
        uninstall_thread(NULL);
        return g_uninstall_rc;
    }
    pthread_detach(th);
    for (int i = 0; i < UNINSTALL_WAIT * 10 && g_uninstall_busy; i++)
        usleep(100000);
    if (g_uninstall_busy) {
        printf("[remove] %s: uninstall did not return within %d s\n", tid, UNINSTALL_WAIT);
        return -2;
    }
    return g_uninstall_rc;
}

int app_remove(const char *tid, char *msg, size_t cap) {
    managed_t *m = managed_by(tid, NULL);
    if ((m && m->running) || title_running(tid)) {
        printf("[web] remove %s refused: game running\n", tid);
        snprintf(msg, cap, "Close the game before removing it");
        return -1;
    }
    if (m)
        unmount_title(tid, m->dlc_tids, m->ndlc_tids);
    else
        unmount_title(tid, NULL, 0);
    char a[64], b[64];
    snprintf(a, sizeof(a), "/user/app/%s", tid);
    snprintf(b, sizeof(b), "/user/patch/%s", tid);
    if (mounted_from(a, NULL, 0) || mounted_from(b, NULL, 0)) {
        /* never let the system uninstall run while the USB folder is reachable */
        snprintf(msg, cap, "Could not unmount %s; nothing was removed", tid);
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", tid);
    write_file(path, tid, strlen(tid), 0666);
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    unlink(path);
    int rc = db_is_registered(tid) ? uninstall_title(tid) : 0;
    if (m)
        m->used = 0;
    if (!rc)
        ac_drop_game(tid);
    printf("[web] remove %s: uninstall=0x%08x\n", tid, rc);
    if (rc == -2)
        snprintf(msg, cap, "The system did not finish removing %s yet; try again in a moment", tid);
    else
        snprintf(msg, cap, rc ? "Failed to remove %s (0x%08x)" : "%s removed from the library. The pkg stays on the USB drive.",
                 tid, rc);
    return rc ? -1 : 0;
}

int app_exclude(const char *tid, int on, char *msg, size_t cap) {
    char rm[64];
    snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
    if (!on) {
        if (kv_set(EXCLUDED_FILE, tid, NULL)) {
            snprintf(msg, cap, "Could not update the list of games never added");
            return -1;
        }
        unlink(rm);
        retry_clear(&g_retry, tid);
        g_next_scan = 0;
        printf("[web] %s allowed again\n", tid);
        snprintf(msg, cap, "%s will be added on the next scan", tid);
        return 0;
    }
    char app[64];
    snprintf(app, sizeof(app), "/user/app/%s", tid);
    if ((db_is_registered(tid) || mounted_from(app, NULL, 0)) && !exists(rm) && app_remove(tid, msg, cap))
        return -1;
    if (kv_set(EXCLUDED_FILE, tid, "1")) {
        snprintf(msg, cap, "Could not update the list of games never added");
        return -1;
    }
    printf("[web] %s set to never be added\n", tid);
    snprintf(msg, cap, "%s will not be added again. The pkgs stay on the drive.", tid);
    return 0;
}

/* --- file manager: listing a folder with catalog details --- */

/* 1 when path is a pkg of a registered (or being-registered) game, or a folder holding one
 * (so it must not be deleted or moved). name gets the first such game's title ID when
 * non-NULL (the catalog does not keep the game's TITLE, only an add-on's). The update used
 * and the add-ons are resolved with catalog_patch/catalog_dlc_of (not g->patch/g->dlc[]
 * directly) because with the cross-drive setting on they may live on another drive than the
 * game (g_cat[*].xpatch, xdlc[]); g->app is always local, so it needs no resolving.
 *
 * db_is_registered() alone misses the window where a game's files are already live on the
 * console but the database row has not appeared yet: register_prepare() calls place_files()
 * (writes the HOLD_DIR symlinks) and mount_title() (mounts them over /user/app/<TID>) and
 * only then manage() (adds the title to g_managed[]), all before install_all() ever pumps
 * events and lets the panel run a request; db_is_registered() does not turn true until
 * ShellCore's AppInstallAll call that install_all() waits for returns. So managed_by() is
 * checked too: it is true exactly from register_prepare()'s manage() call (files already
 * placed and mounted) through to app_remove()/the uninstall-end klog handler clearing
 * m->used, which covers the whole time this title's files are live, registered or not.
 * marker_matches()/hold_ready() (what title_state's f.installed uses) do not work here: the
 * marker file is written by register_finish(), which only runs after registration already
 * succeeded, so it is never set during the window this check needs to cover. */
static int fm_protected(const char *path, const char **name) {
    for (int d = 0; d < NUM_DRIVES; d++) {
        const catalog_t *cat = &g_cat[d];
        for (int gi = 0; gi < cat->ngames; gi++) {
            const cat_game_t *g = &cat->games[gi];
            if (!db_is_registered(g->title_id) && !managed_by(g->title_id, NULL))
                continue;
            if (fm_covers(path, cat->pkgs[g->app].path)) {
                if (name)
                    *name = g->title_id;
                return 1;
            }
            const cat_pkg_t *used = catalog_patch(g_cat, cat, g);
            if (used && fm_covers(path, used->path)) {
                if (name)
                    *name = g->title_id;
                return 1;
            }
            const cat_pkg_t *dlc[2 * CAT_MAX_DLC];
            int n = catalog_dlc_of(g_cat, cat, g, dlc, 2 * CAT_MAX_DLC);
            for (int k = 0; k < n; k++) {
                if (fm_covers(path, dlc[k]->path)) {
                    if (name)
                        *name = g->title_id;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Every pkg path that belongs to a registered (or being-registered) game, collected once so a
 * listing does not re-walk every catalog for every entry (fm_protected above does that walk
 * for a single path; this does the same traversal once and gathers every path it would have
 * matched). Skips a game exactly where fm_protected would skip it (not registered and not in
 * g_managed[], see fm_protected's comment), then collects the game pkg, the resolved update
 * (catalog_patch, which also finds a cross-drive update) and every add-on
 * (catalog_dlc_of, local and cross-drive). Returns how many paths were collected; the paths
 * point into the catalogs, which do not change while one listing runs.
 *
 * *truncated is set to 1 if max was reached before every path was collected. That can happen
 * even though max is sized from the total pkg count: with cross-drive linking on, catalog_link
 * can link one physical pkg into more than one game (an orphan add-on's xdlc loop has no
 * "first match wins" guard, so one add-on can serve every matching game on every other drive;
 * pick_any_update can likewise make one cross-drive update the patch used by several
 * drive-copies of a title), so the number of paths collected is not actually bounded by the
 * pkg count when that fan-out happens. The caller must fall back to per-path fm_protected()
 * checks when truncated is set, since the paths left uncollected here are still protected. */
static int fm_protected_paths(const char **out, int max, int *truncated) {
    int n = 0;
    *truncated = 0;
    for (int d = 0; d < NUM_DRIVES; d++) {
        const catalog_t *cat = &g_cat[d];
        for (int gi = 0; gi < cat->ngames; gi++) {
            const cat_game_t *g = &cat->games[gi];
            if (!db_is_registered(g->title_id) && !managed_by(g->title_id, NULL))
                continue;
            if (n >= max) {
                *truncated = 1;
                return n;
            }
            out[n++] = cat->pkgs[g->app].path;
            const cat_pkg_t *used = catalog_patch(g_cat, cat, g);
            if (used) {
                if (n >= max) {
                    *truncated = 1;
                    return n;
                }
                out[n++] = used->path;
            }
            const cat_pkg_t *dlc[2 * CAT_MAX_DLC];
            int dn = catalog_dlc_of(g_cat, cat, g, dlc, 2 * CAT_MAX_DLC);
            for (int k = 0; k < dn; k++) {
                if (n >= max) {
                    *truncated = 1;
                    return n;
                }
                out[n++] = dlc[k]->path;
            }
        }
    }
    return n;
}

/* Lists one folder of a drive as JSON rows, joined with the catalog so a pkg's title ID,
 * role, version and title are added when the folder is on a known drive (see app_fm_list,
 * web.h). path is a caller-checked drive path (web.c calls fm_path_ok first). */
int app_fm_list(const char *path, buf_t *out, char *msg, size_t cap) {
    DIR *d = opendir(path);
    if (!d) {
        printf("[fm] opendir %s: %s\n", path, strerror(errno));
        snprintf(msg, cap, "Cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    int di = fm_drive_of(path);
    const catalog_t *cat = di >= 0 ? &g_cat[di] : NULL;
    /* ".trash" (see TRASH_DIR_NAME below) is only ever the drive's Trash bin at its root; a
     * folder named ".trash" deeper in the tree is just a folder (fm_name_ok refuses making a
     * new one, but one from before that rule, or made by another tool, must stay reachable). */
    int at_root = fm_is_drive_root(path);

    /* The protected-path set cannot change while this one listing runs, so it is collected
     * once here instead of once per entry (that used to mean every entry re-walked every
     * catalog, drive x games x add-ons, and re-ran db_is_registered for every game of every
     * entry). pcap (the total pkg count across every drive's catalog, plus a little slack)
     * assumes each collected path is one distinct physical pkg already counted in some
     * catalog's npkgs. Cross-drive linking can break that assumption (see fm_protected_paths),
     * so a true count is not knowable up front; if the walk below hits pcap before it is done,
     * ppaths is dropped and the per-entry fm_protected() fallback further down is used for
     * this listing instead, so a saturated collection never means a dropped protection.
     * Skipped entirely when this folder is not on a known drive (cat is NULL), since then no
     * entry below is ever matched to a catalog pkg either. */
    const char **ppaths = NULL;
    int pcount = 0;
    if (cat) {
        int pcap = 16;
        for (int i = 0; i < NUM_DRIVES; i++)
            pcap += g_cat[i].npkgs;
        ppaths = malloc(sizeof(*ppaths) * (size_t)pcap);
        if (ppaths) {
            int truncated = 0;
            pcount = fm_protected_paths(ppaths, pcap, &truncated);
            if (truncated) {
                free(ppaths);
                ppaths = NULL; /* fall back to fm_protected() per entry below rather than
                                 * report "not protected" for whatever did not fit */
            }
        }
    }

    buf_printf(out, "{\"path\":");
    json_str(out, path);
    buf_printf(out, ",\"entries\":[");
    struct dirent *e;
    int first = 1, count = 0, truncated = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (at_root && !strcmp(e->d_name, ".trash")))
            continue;
        if (count >= 2000) {
            truncated = 1; /* at least one more real entry exists past the cap */
            break;
        }
        char full[PATH_MAX + 256]; /* matches the mutation functions below, so long entries stay visible */
        if (snprintf(full, sizeof(full), "%s/%s", path, e->d_name) >= (int)sizeof(full))
            continue; /* name too long to build a safe path, leave it out */
        struct stat st;
        if (lstat(full, &st))
            continue;
        buf_printf(out, "%s{\"name\":", first ? "" : ",");
        json_str(out, e->d_name);
        buf_printf(out, ",\"dir\":%s,\"size\":%llu,\"mtime\":%lld", S_ISDIR(st.st_mode) ? "true" : "false",
                   (unsigned long long)st.st_size, (long long)st.st_mtime);
        const cat_pkg_t *pk = NULL;
        if (cat && !S_ISDIR(st.st_mode))
            for (int i = 0; i < cat->npkgs; i++)
                if (!strcmp(cat->pkgs[i].path, full)) {
                    pk = &cat->pkgs[i];
                    break;
                }
        if (pk) {
            buf_printf(out, ",\"pkg\":{\"title_id\":");
            json_str(out, pk->title_id);
            buf_printf(out, ",\"role\":%d,\"version\":", pk->role);
            json_str(out, pk->app_ver);
            buf_printf(out, ",\"title\":");
            json_str(out, pk->name);
            int prot;
            if (ppaths) {
                prot = 0;
                for (int i = 0; i < pcount; i++)
                    if (fm_covers(full, ppaths[i])) {
                        prot = 1;
                        break;
                    }
            } else {
                prot = fm_protected(full, NULL); /* ppaths is NULL: either the malloc above
                                                   * failed or the collection saturated pcap;
                                                   * either way fall back per entry so a
                                                   * protected pkg is still never missed */
            }
            buf_printf(out, ",\"protected\":%s}", prot ? "true" : "false");
        } else if (cat && S_ISDIR(st.st_mode)) {
            /* A folder is "protected" exactly when some protected pkg lives inside it (at any
             * depth): fm_covers(dir, pkg_path) is true when dir is a prefix of pkg_path, so the
             * same check (and fm_protected() fallback) used for a pkg file above also answers
             * this for a directory path, unchanged. Emitted as a top-level field rather than
             * nested in "pkg" (folders have none), so the panel can disable Rename/Move/Delete
             * for a folder that directly contains a protected pkg, not just the pkg file
             * itself. */
            int prot;
            if (ppaths) {
                prot = 0;
                for (int i = 0; i < pcount; i++)
                    if (fm_covers(full, ppaths[i])) {
                        prot = 1;
                        break;
                    }
            } else {
                prot = fm_protected(full, NULL);
            }
            buf_printf(out, ",\"protected\":%s", prot ? "true" : "false");
        }
        buf_printf(out, "}");
        first = 0;
        count++;
    }
    closedir(d);
    free(ppaths);
    buf_printf(out, "],\"truncated\":%s}", truncated ? "true" : "false");
    return 0;
}

/* Queues a rescan a few seconds out instead of right away, so a run of several file manager
 * operations (drag-reorganizing a folder, say) collapses into one scan instead of stalling the
 * panel on scan_all() after every single mutation. Only the drive the mutation touched needs a
 * fresh walk; reuses the g_scan_only mechanism app_rescan already has for "scan just this
 * drive", widening it to every drive if a different drive is already queued so neither change
 * is lost. Never touches a pending explicit "Scan for games" (app_rescan sets g_manual_scan),
 * which must still run immediately and scan what the user asked for. */
static void fm_queue_scan(int drive) {
    if (g_manual_scan)
        return;
    if (g_next_scan > time(NULL) && g_scan_only != drive)
        g_scan_only = -1;
    else
        g_scan_only = drive;
    g_next_scan = time(NULL) + 3;
}

/* --- file manager: mutations (make, rename, delete, move) ---
 * dir/path/src/dst are caller-checked (web.c calls fm_path_ok, fm_is_drive_root first); name is
 * caller-checked with fm_name_ok, so it never contains '/' or "." / "..". A combined path can
 * still reach dir's 1023 chars plus a name's 240, so every buffer below is sized PATH_MAX + 256
 * and every snprintf that joins them is checked: on would-be truncation the mutation fails
 * closed instead of risking an operation on a path that isn't the one the caller asked for.
 * app_fm_mkdir/app_fm_rename/app_fm_delete/app_fm_move also refuse outright while g_install_busy
 * || g_adding (install_all() pumps events, including panel requests, during that window; see
 * app_launch's own check and fm_protected's comment above): fm_protected alone already covers a
 * game whose files are already placed, but a pkg about to be used by a game not yet even in the
 * catalog (its register_prepare() has not been reached this batch) is not something fm_protected
 * can see at all, so the blanket refusal covers that case too. Each of the four also refuses
 * while fmgr_busy(): a background copy/move task (see fmgr.[ch]) may right now be reading from
 * or writing into the very folder a single-file mutation is about to touch. */

int app_fm_mkdir(const char *dir, const char *name, char *msg, size_t cap) {
    if (g_install_busy || g_adding) {
        snprintf(msg, cap, "Games are being added; try again in a moment");
        return -1;
    }
    if (fmgr_busy()) {
        snprintf(msg, cap, "A file operation is already running; wait for it to finish");
        return -1;
    }
    char full[PATH_MAX + 256];
    if (snprintf(full, sizeof(full), "%s/%s", dir, name) >= (int)sizeof(full)) {
        snprintf(msg, cap, "Path too long");
        return -1;
    }
    if (mkdir(full, 0777)) {
        int err = errno;
        snprintf(msg, cap, err == EEXIST ? "%s already exists" : "Could not create %s", name);
        return -1;
    }
    fm_queue_scan(fm_drive_of(dir));
    printf("[fm] mkdir %s\n", full);
    snprintf(msg, cap, "Folder %s created", name);
    return 0;
}

int app_fm_rename(const char *path, const char *name, char *msg, size_t cap) {
    if (g_install_busy || g_adding) {
        snprintf(msg, cap, "Games are being added; try again in a moment");
        return -1;
    }
    if (fmgr_busy()) {
        snprintf(msg, cap, "A file operation is already running; wait for it to finish");
        return -1;
    }
    const char *game = NULL;
    if (fm_protected(path, &game)) {
        snprintf(msg, cap, "%s is used by %s; remove the game from the library first",
                 strrchr(path, '/') + 1, game ? game : "a game");
        return -1;
    }
    const char *slash = strrchr(path, '/');
    char dst[PATH_MAX + 256];
    if (snprintf(dst, sizeof(dst), "%.*s/%s", (int)(slash - path), path, name) >= (int)sizeof(dst)) {
        snprintf(msg, cap, "Path too long");
        return -1;
    }
    struct stat st;
    if (!stat(dst, &st)) {
        snprintf(msg, cap, "%s already exists", name);
        return -1;
    }
    if (rename(path, dst)) {
        int err = errno;
        snprintf(msg, cap, "Could not rename: %s", strerror(err));
        return -1;
    }
    fm_queue_scan(fm_drive_of(path));
    printf("[fm] rename %s -> %s\n", path, dst);
    snprintf(msg, cap, "Renamed to %s", name);
    return 0;
}

/* Deletes path, recursing into directories first. errno is preserved across each closedir() so
 * a failure anywhere in the tree is still reported accurately by the caller. An entry whose full
 * path would not fit the buffer is left alone rather than built into a truncated (and possibly
 * wrong) path: the directory below it then fails to go empty and rmdir reports it, so the whole
 * delete fails closed instead of silently touching something the caller never asked to remove. */
static int fm_rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st))
        return -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *e;
    int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[PATH_MAX + 256];
        if (snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >= (int)sizeof(sub))
            continue; /* name too long to build a safe path: skip it, never guess */
        rc = fm_rm_tree(sub);
    }
    int err = errno;
    closedir(d);
    errno = err;
    return rc ? rc : rmdir(path);
}

#define TRASH_DIR_NAME ".trash"

/* Moves path into <drive>/.trash/ instead of removing it, so a delete from the panel is
 * reversible until a Task 7 purge runs fm_rm_tree on the trashed copy. fm_protected is checked
 * first, before anything else (including the mkdirs below) touches the filesystem, so a
 * protected path is never moved even into Trash. path is caller-checked (web.c calls
 * fm_path_ok first), which guarantees it sits on a known drive, so fm_drive_of(path) is safe to
 * use unchecked here, same as app_fm_rename/app_fm_move already trust it. */
int app_fm_delete(const char *path, char *msg, size_t cap) {
    if (g_install_busy || g_adding) {
        snprintf(msg, cap, "Games are being added; try again in a moment");
        return -1;
    }
    if (fmgr_busy()) {
        snprintf(msg, cap, "A file operation is already running; wait for it to finish");
        return -1;
    }
    const char *game = NULL;
    if (fm_protected(path, &game)) {
        snprintf(msg, cap, "Used by %s; remove the game from the library first", game ? game : "a game");
        return -1;
    }
    int di = fm_drive_of(path);
    char root[32], trash_dir[64];
    drive_root(di, root, sizeof(root));
    if (snprintf(trash_dir, sizeof(trash_dir), "%s/" TRASH_DIR_NAME, root) >= (int)sizeof(trash_dir)) {
        snprintf(msg, cap, "Path too long");
        return -1;
    }
    mkdirs(trash_dir, 0700);
    struct stat tst;
    if (stat(trash_dir, &tst) || !S_ISDIR(tst.st_mode)) {
        snprintf(msg, cap, "Could not open Trash on %s; the drive may be read-only or full", root + 5);
        return -1;
    }

    const char *base = strrchr(path, '/') + 1;
    char dst[PATH_MAX + 256], meta[PATH_MAX + 256];
    time_t ts = time(NULL);
    int tries;
    struct stat st;
    for (tries = 0; tries < 1000; tries++) {
        if (snprintf(dst, sizeof(dst), "%s/%lld-%s", trash_dir, (long long)ts + tries, base) >= (int)sizeof(dst)) {
            snprintf(msg, cap, "Path too long");
            return -1;
        }
        if (stat(dst, &st))
            break; /* dst does not exist yet: free to use */
    }
    if (tries == 1000) {
        snprintf(msg, cap, "Could not make room in Trash for %s", base);
        return -1;
    }
    if (rename(path, dst)) {
        int err = errno;
        snprintf(msg, cap, "Could not delete: %s", strerror(err));
        return -1;
    }
    if (snprintf(meta, sizeof(meta), "%s.trashmeta", dst) < (int)sizeof(meta)) {
        char body[PATH_MAX + 64];
        int n = snprintf(body, sizeof(body), "%s\n%lld\n", path, (long long)ts);
        write_file(meta, body, (size_t)n, 0600); /* best-effort: a missing sidecar only affects age-purge, not safety */
    } /* truncation here would risk writing a sidecar at the wrong path; skip it rather than guess */

    fm_queue_scan(di);
    printf("[fm] trashed %s -> %s\n", path, dst);
    snprintf(msg, cap, "%s moved to Trash", base);
    return 0;
}

/* Same-drive move only. fm_covers (is dst inside src, or src itself) is checked before
 * fm_protected and before anything destructive, so a folder is never moved into itself or one
 * of its own subfolders regardless of in-use state. */
int app_fm_move(const char *src, const char *dst, char *msg, size_t cap) {
    if (g_install_busy || g_adding) {
        snprintf(msg, cap, "Games are being added; try again in a moment");
        return -1;
    }
    if (fmgr_busy()) {
        snprintf(msg, cap, "A file operation is already running; wait for it to finish");
        return -1;
    }
    const char *game = NULL;
    if (fm_drive_of(src) != fm_drive_of(dst)) {
        snprintf(msg, cap, "Moving between drives is not supported yet");
        return -1;
    }
    if (fm_covers(src, dst)) {
        snprintf(msg, cap, "Cannot move a folder into itself");
        return -1;
    }
    if (fm_protected(src, &game)) {
        snprintf(msg, cap, "Used by %s; remove the game from the library first", game ? game : "a game");
        return -1;
    }
    if (fm_protected(dst, &game)) {
        snprintf(msg, cap, "Used by %s; remove the game from the library first", game ? game : "a game");
        return -1;
    }
    struct stat st;
    if (stat(dst, &st) || !S_ISDIR(st.st_mode)) {
        snprintf(msg, cap, "The destination is not a folder");
        return -1;
    }
    char full[PATH_MAX + 256];
    if (snprintf(full, sizeof(full), "%s/%s", dst, strrchr(src, '/') + 1) >= (int)sizeof(full)) {
        snprintf(msg, cap, "Path too long");
        return -1;
    }
    if (!stat(full, &st)) {
        snprintf(msg, cap, "%s already exists there", strrchr(src, '/') + 1);
        return -1;
    }
    if (rename(src, full)) {
        int err = errno;
        snprintf(msg, cap, "Could not move: %s", strerror(err));
        return -1;
    }
    fm_queue_scan(fm_drive_of(src));
    printf("[fm] move %s -> %s\n", src, full);
    snprintf(msg, cap, "Moved to %s", dst);
    return 0;
}

/* --- file manager: background copy/move task (see fmgr.[ch]) ---
 * app_fm_copy only validates the request and calls fmgr_start(); the actual copy or move runs
 * later, entirely on fmgr's own worker thread. Isolation (deliberate, see the Phase 2 plan):
 * this file never calls a catalog_* write function, a registration/mount/unmount function, or
 * touches g_retry for this task, and touches g_cat only through the existing read-only
 * fm_protected() helper, same as the single-file mutations above. g_fm_task_drive remembers the
 * destination drive of the task just started; g_fm_task_seen_state remembers the FMGR_* state
 * app_fm_task_json last reported, so the first poll that observes the task land in a terminal
 * state can queue exactly one rescan of that drive (see app_fm_task_json below). */
static int g_fm_task_drive = -1;
static int g_fm_task_seen_state = FMGR_IDLE;

int app_fm_copy(const char *srcs, const char *dst, int copy, int conflict, char *msg, size_t cap) {
    if (g_install_busy || g_adding) {
        snprintf(msg, cap, "Games are being added; try again in a moment");
        return -1;
    }
    if (fmgr_busy()) {
        snprintf(msg, cap, "A file operation is already running; wait for it to finish");
        return -1;
    }

    /* Split srcs on '\n'; empty segments (a trailing newline, or none at all) are skipped. */
    char srcs_buf[FMGR_MAX_SRCS][1024];
    int n = 0;
    const char *seg_start = srcs;
    for (const char *p = srcs;; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t len = (size_t)(p - seg_start);
            if (len > 0) {
                if (n >= FMGR_MAX_SRCS) {
                    snprintf(msg, cap, "Too many items selected (limit %d)", FMGR_MAX_SRCS);
                    return -1;
                }
                if (len >= sizeof(srcs_buf[0])) {
                    snprintf(msg, cap, "A source path is too long");
                    return -1;
                }
                memcpy(srcs_buf[n], seg_start, len);
                srcs_buf[n][len] = 0;
                if (!fm_path_ok(srcs_buf[n])) {
                    snprintf(msg, cap, "One of the selected paths is not valid");
                    return -1;
                }
                n++;
            }
            seg_start = p + 1;
            if (*p == '\0')
                break;
        }
    }
    if (n == 0) {
        snprintf(msg, cap, "Nothing selected");
        return -1;
    }

    const char *game = NULL;
    for (int i = 0; i < n; i++) {
        if (fm_protected(srcs_buf[i], &game)) {
            snprintf(msg, cap, "Used by %s; remove the game from the library first", game ? game : "a game");
            return -1;
        }
    }
    if (fm_protected(dst, &game)) {
        snprintf(msg, cap, "Used by %s; remove the game from the library first", game ? game : "a game");
        return -1;
    }

    int dd = fm_drive_of(dst);
    unsigned long long free_bytes = 0, total_bytes = 0;
    if (dd >= 0) {
        char root[32];
        drive_root(dd, root, sizeof(root));
        drive_space(root, &free_bytes, &total_bytes);
    }

    if (fmgr_start(srcs_buf, n, dst, copy, conflict, free_bytes, msg, cap))
        return -1;

    g_fm_task_drive = dd;
    g_fm_task_seen_state = FMGR_RUNNING;
    printf("[fm] %s %d item(s) -> %s\n", copy ? "copy" : "move", n, dst);
    return 0;
}

int app_fm_cancel(char *msg, size_t cap) {
    if (!fmgr_busy()) {
        snprintf(msg, cap, "No file operation is running");
        return -1;
    }
    fmgr_cancel();
    snprintf(msg, cap, "Cancelling");
    return 0;
}

/* Writes the running (or just-finished) task's status for the panel. The worker thread already
 * did the actual filesystem work; the ONLY thing done here, on the first poll that sees the
 * task land in a terminal state after having been running, is to queue a rescan of the
 * destination drive -- not a catalog update, not registration, nothing else -- so a scan the
 * daemon would have run anyway is the one and only way this task's result reaches the library,
 * keeping the isolation described above. */
void app_fm_task_json(buf_t *out) {
    fmgr_status_t s;
    fmgr_status(&s);

    if (g_fm_task_seen_state == FMGR_RUNNING &&
        (s.state == FMGR_DONE || s.state == FMGR_FAILED || s.state == FMGR_CANCELLED)) {
        if (g_fm_task_drive >= 0)
            fm_queue_scan(g_fm_task_drive);
    }
    g_fm_task_seen_state = s.state;

    const char *state = s.state == FMGR_RUNNING ? "running"
                         : s.state == FMGR_DONE  ? "done"
                         : s.state == FMGR_FAILED ? "failed"
                         : s.state == FMGR_CANCELLED ? "cancelled"
                                                      : "idle";
    buf_printf(out, "{\"state\":\"%s\",\"what\":", state);
    json_str(out, s.what);
    buf_printf(out,
               ",\"done_items\":%d,\"total_items\":%d,\"done_bytes\":%llu,\"total_bytes\":%llu,"
               "\"speed\":%llu,\"eta\":%ld,\"msg\":",
               s.done_items, s.total_items, s.done_bytes, s.total_bytes, s.bytes_per_sec, s.eta_seconds);
    json_str(out, s.msg);
    buf_printf(out, "}");
}

static void pump_events(int ms); /* defined with the main loop below */

/* Removes a game refused with "system file disagree" from the library and adds it again. */
static void readd_title(const char *tid) {
    char msg[256], name[128];
    managed_t *m = managed_by(tid, NULL);
    snprintf(name, sizeof(name), "%s", m ? m->name : tid);
    if (g_nreadd_done < MAX_TITLES)
        snprintf(g_readd_done[g_nreadd_done++], 10, "%s", tid);
    pump_events(5000); /* let the system finish the refused launch */
    notify("%s did not start (system file disagree): adding it to the library again", name);
    int rc = app_remove(tid, msg, sizeof(msg));
    printf("[reg] %s: removed to add it again: %s\n", tid, msg);
    if (!rc)
        wait_db_quiet(3, 30);
    app_add(tid, msg, sizeof(msg));
}

/* --- moving pkgs back where they came from --- */

static void clean_name(char *s) {
    for (; *s; s++)
        if (strchr("\\:*?\"<>|", *s) || (unsigned char)*s < 0x20)
            *s = '_';
}

/* Moves <game_dir>/<rel> to its recorded original path (or fallback, relative to the
 * drive root, for pkgs moved before origins were recorded) and adds it to the list of
 * restored pkgs. 1 moved, 0 nothing there, -1 failed. */
static int restore_pkg(const char *root, const char *game_dir, const char *rel, const char *fallback) {
    char src[PATH_MAX], orig[PATH_MAX], dst[PATH_MAX], dir[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s", game_dir, rel);
    struct stat st;
    if (stat(src, &st))
        return 0;
    if (origin_find(game_dir, rel, orig, sizeof(orig))) {
        snprintf(orig, sizeof(orig), "%s", fallback);
        clean_name(orig);
    }
    snprintf(dir, sizeof(dir), "%s/%s", root, orig);
    char *slash = strrchr(dir, '/');
    if (slash && slash > dir + strlen(root))
        *slash = 0, mkdirs(dir, 0777); /* the original folder may be gone */
    if (origin_free_path(root, orig, dst, sizeof(dst)) || rename(src, dst)) {
        printf("[restore] %s: %s\n", src, strerror(errno));
        return -1;
    }
    restored_add(RESTORED_LIST, drive_rel(root, dst), (uint64_t)st.st_size);
    printf("[restore] %s -> %s\n", src, dst);
    return 1;
}

static void unlink_in(const char *dir, const char *const *names) {
    char p[PATH_MAX];
    for (; *names; names++) {
        snprintf(p, sizeof(p), "%s/%s", dir, *names);
        unlink(p);
    }
}

/* Moves every pkg of one game folder back and removes what PS4Mount created for it.
 * Folders are only removed when empty, so nothing else is ever deleted. */
static void restore_files(const char *root, const char *tid, const char *name, int *moved, int *failed) {
    static const char *const generated[] = {"app.json", "app.pbm", "app.pbm.backup", "app.xml", "patch.json",
                                            "patch.pbm", "patch.pbm.backup", "patch.xml", NULL};
    static const char *const ac_generated[] = {"ac.json", "ac.pbm", "ac.pbm.backup", NULL};
    char game[PATH_MAX], rel[PATH_MAX], fb[PATH_MAX], sub[PATH_MAX];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    int rc;
#define RESTORE(r, f)                                                                                                  \
    do {                                                                                                               \
        rc = restore_pkg(root, game, r, f);                                                                            \
        *moved += rc > 0;                                                                                              \
        *failed += rc < 0;                                                                                             \
    } while (0)
    snprintf(fb, sizeof(fb), "%s - %s - game.pkg", tid, name);
    RESTORE("app.pkg", fb);
    snprintf(fb, sizeof(fb), "%s - %s - update.pkg", tid, name);
    RESTORE("patch.pkg", fb);
    snprintf(sub, sizeof(sub), "%s/replaced", game);
    DIR *d = opendir(sub);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(rel, sizeof(rel), "replaced/%s", e->d_name);
        snprintf(fb, sizeof(fb), "%s - %s - older update %s", tid, name, e->d_name);
        RESTORE(rel, fb);
    }
    if (d)
        closedir(d);
    rmdir(sub);
    snprintf(sub, sizeof(sub), "%s/addcont", game);
    d = opendir(sub);
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char tdir[PATH_MAX], hold[PATH_MAX];
        snprintf(tdir, sizeof(tdir), "%s/%s", sub, e->d_name);
        DIR *l = opendir(tdir);
        struct dirent *f;
        while (l && (f = readdir(l))) {
            if (f->d_name[0] == '.')
                continue;
            char ldir[PATH_MAX + 64];
            snprintf(ldir, sizeof(ldir), "%s/%s", tdir, f->d_name);
            snprintf(rel, sizeof(rel), "addcont/%s/%s/ac.pkg", e->d_name, f->d_name);
            snprintf(fb, sizeof(fb), "%s - %s - %s.pkg", e->d_name, name, f->d_name);
            RESTORE(rel, fb);
            unlink_in(ldir, ac_generated);
            rmdir(ldir);
            snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s/%s", e->d_name, f->d_name);
            rm_tree(hold);
        }
        if (l)
            closedir(l);
        rmdir(tdir);
        snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s", e->d_name);
        rmdir(hold); /* kept while other games still use add-ons of this title */
    }
    if (d)
        closedir(d);
#undef RESTORE
    rmdir(sub);
    unlink_in(game, generated); /* written next to the pkgs by versions before 0.3.2 */
    char marker[64];
    snprintf(marker, sizeof(marker), "%s/" ORIGIN_FILE, game);
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/app.pkg", game);
    if (!exists(probe)) { /* keep the record while a pkg could not be moved */
        unlink(marker);
        char nf[128], nfp[PATH_MAX + 130];
        origin_name_file(nf, sizeof(nf), name);
        snprintf(nfp, sizeof(nfp), "%s/%s", game, nf);
        unlink(nfp);
    }
    rmdir(game);
    snprintf(probe, sizeof(probe), HOLD_DIR "/app/%s", tid);
    rm_tree(probe);
    snprintf(probe, sizeof(probe), HOLD_DIR "/patch/%s", tid);
    rm_tree(probe);
}

/* Add-on title IDs of a game folder (its /user/addcont mounts). */
static int folder_dlc_tids(const char *game, char (*tids)[10], int max) {
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/addcont", game);
    DIR *d = opendir(sub);
    int n = 0;
    struct dirent *e;
    while (d && n < max && (e = readdir(d)))
        if (strlen(e->d_name) == 9 && !strncmp(e->d_name, "CUSA", 4))
            snprintf(tids[n++], 10, "%s", e->d_name);
    if (d)
        closedir(d);
    return n;
}

/* Takes one game out of the library (through the system) and unmounts it. */
static int restore_unregister(const char *root, const char *tid) {
    char game[PATH_MAX], dtids[MAX_DLC_TIDS][10];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    int nd = folder_dlc_tids(game, dtids, MAX_DLC_TIDS);
    unmount_title(tid, dtids, nd);
    char msg[256];
    if (!installed_internally(tid) && app_remove(tid, msg, sizeof(msg))) {
        printf("[restore] %s kept: %s\n", tid, msg);
        return -1;
    }
    return 0;
}

static void restore_clear_state(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), STATE_DIR "/%s.removed", tid);
    unlink(p);
    snprintf(p, sizeof(p), STATE_DIR "/%s.src", tid);
    unlink(p);
    retry_clear(&g_retry, tid);
}

/* Requests from the panel, run by the main loop: some titles, or all of them. */
static char g_restore_tids[MAX_TITLES][10];
static int g_restore_n, g_restore_all;

/* --- closing the open game --- */

typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[10];
    uint32_t app_type;
    char unknown2[0x36];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);
int sceSystemServiceKillApp(uint32_t app_id, int how, int reason, int core_dump);

/* Open games: processes named eboot.bin (PS4 and PS5 games alike). Fills up to max
 * app ids and title ids, returns how many are open. */
static int open_games(uint32_t *ids, char (*tids)[10], int max) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}, n = 0;
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) || !len)
        return 0;
    len += len / 4; /* processes started meanwhile */
    uint8_t *buf = malloc(len);
    if (!buf || sysctl(mib, 4, buf, &len, NULL, 0)) {
        free(buf);
        return 0;
    }
    for (size_t off = 0; off + sizeof(struct kinfo_proc) <= len;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)(buf + off);
        if (ki->ki_structsize < (int)sizeof(*ki) || (size_t)ki->ki_structsize > len - off)
            break;
        app_info_t info;
        memset(&info, 0, sizeof(info));
        if (!strcmp(ki->ki_comm, "eboot.bin") && !sceKernelGetAppInfo(ki->ki_pid, &info) && info.app_id) {
            if (n < max) {
                ids[n] = info.app_id;
                snprintf(tids[n], 10, "%.9s", info.title_id);
            }
            n++;
        }
        off += (size_t)ki->ki_structsize;
    }
    free(buf);
    return n;
}

/* Every pkg operation that takes games out of the library closes the open game first,
 * whichever it is: a game left open keeps reading pkgs that are being moved. 0 once no
 * game is open; -1 when one did not close within 30 seconds. */
static int close_open_games(const char *why) {
    uint32_t ids[8];
    char tids[8][10];
    int n = open_games(ids, tids, 8);
    if (!n)
        return 0;
    for (int i = 0; i < n && i < 8; i++) {
        managed_t *m = managed_by(tids[i], NULL);
        notify("Closing %s %s", m ? m->name : tids[i], why);
        int rc = sceSystemServiceKillApp(ids[i], -1, 0, 0);
        printf("[close] %s (app 0x%08x): 0x%08x\n", tids[i], ids[i], rc);
    }
    char closing[8][10];
    int nc = n < 8 ? n : 8;
    memcpy(closing, tids, sizeof(closing[0]) * (size_t)nc);
    for (int s = 0; s < 30; s++) {
        pump_events(1000);
        int mounted = 0;
        for (int i = 0; i < nc; i++)
            mounted |= title_running(closing[i]);
        if (!open_games(ids, tids, 8) && !mounted) {
            /* its package images are released last: a game started before that cannot
             * read its own files (0x80020060) */
            for (int w = 0; w < 20; w++) {
                int left = 0;
                for (int i = 0; i < nc; i++)
                    left += pfs_mounts(closing[i]);
                if (!left)
                    break;
                pump_events(1000);
            }
            pump_events(2000); /* let the system finish unmounting the game */
            for (int i = 0; i < MAX_TITLES; i++)
                g_managed[i].running = 0;
            return 0;
        }
    }
    return -1;
}

/* Folders of games on every connected drive (or only tid), with their name. */
typedef struct {
    char root[32], tid[10], name[128];
    int other; /* the game has another copy on another drive: it stays in the library with it */
} restore_item_t;

/* only: n title ids to keep, or NULL for every game. */
static int restore_collect(char (*only)[10], int nonly, restore_item_t *items, int max) {
    int n = 0;
    for (int i = 0; i < 10 && n < max; i++) {
        char root[32], lib[PATH_MAX];
        if (i < 8)
            snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else
            snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        if (!is_mountpoint(root))
            continue;
        snprintf(lib, sizeof(lib), "%s/ps4", root);
        DIR *d = opendir(lib);
        struct dirent *e;
        while (d && n < max && (e = readdir(d))) {
            if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
                continue;
            int wanted = !only;
            for (int k = 0; k < nonly && !wanted; k++)
                wanted = !strcmp(only[k], e->d_name);
            if (!wanted)
                continue;
            restore_item_t *it = &items[n++];
            snprintf(it->root, sizeof(it->root), "%s", root);
            snprintf(it->tid, sizeof(it->tid), "%s", e->d_name);
            snprintf(it->name, sizeof(it->name), "%s", e->d_name);
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, e->d_name);
            pkg_t pkg;
            sfo_t sfo;
            if (exists(p) && !pkg_open(&pkg, p)) {
                const sfo_item_t *t = read_sfo(&pkg, &sfo) ? NULL : sfo_get(&sfo, "TITLE");
                if (t) {
                    snprintf(it->name, sizeof(it->name), "%s", t->sval);
                    clean_name(it->name);
                }
                pkg_close(&pkg);
            }
        }
        if (d)
            closedir(d);
    }
    /* a game picked by title ID on two drives: only the copy in use goes back, the other one
     * stays and is used from the next scan */
    for (int i = 0; only && i < n; i++)
        for (int k = i + 1; k < n; k++) {
            if (strcmp(items[i].tid, items[k].tid))
                continue;
            /* <TID>.src names the folder in use */
            char path[64], used[PATH_MAX + 256] = {0}, game[PATH_MAX];
            snprintf(path, sizeof(path), STATE_DIR "/%s.src", items[k].tid);
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                if (read(fd, used, sizeof(used) - 1) < 0)
                    used[0] = 0;
                close(fd);
            }
            used[strcspn(used, "\n")] = 0;
            snprintf(game, sizeof(game), "%s/ps4/%s", items[k].root, items[k].tid);
            int drop = !strcmp(used, game) ? i : k;
            items[drop == i ? k : i].other = 1;
            memmove(&items[drop], &items[drop + 1], sizeof(items[0]) * (size_t)(n - drop - 1));
            n--;
            k = i; /* items[i] may have changed: compare it again with the rest */
        }
    return n;
}

/* Moves the pkgs of the requested games (only, or all when NULL) back, in groups of the
 * restore setting: the games of a group leave the library one at a time, their pkgs are
 * moved, and the next group waits until the system databases are idle. */
static void run_restore(char (*only)[10], int nonly) {
    static restore_item_t items[MAX_TITLES];
    int n = restore_collect(only, nonly, items, MAX_TITLES), moved = 0, failed = 0, done = 0, kept = 0;
    if (!n) {
        notify("No PS4Mount games to move back");
        return;
    }
    if (close_open_games("to move pkgs back")) {
        notify("The open game did not close; no pkgs were moved back");
        return;
    }
    int size = g_cfg.restore_batch;
    if (n == 1)
        notify("Moving the pkgs of %s back...", items[0].name);
    else
        notify("Moving the pkgs of %d game(s) back to where they came from, %d at a time...", n, size);
    for (int i = 0; i < n; i += size) {
        if (i)
            wait_db_quiet(BATCH_QUIET, 120);
        int end = i + size < n ? i + size : n;
        for (int k = i; k < end; k++) {
            if (items[k].other) {
                /* the scan moves the game over to the other copy, registered as it is */
                char game[PATH_MAX], dtids[MAX_DLC_TIDS][10];
                snprintf(game, sizeof(game), "%s/ps4/%s", items[k].root, items[k].tid);
                unmount_title(items[k].tid, dtids, folder_dlc_tids(game, dtids, MAX_DLC_TIDS));
                restore_files(items[k].root, items[k].tid, items[k].name, &moved, &failed);
                printf("[restore] %s: kept in the library with its copy on another drive\n", items[k].tid);
                done++;
                continue;
            }
            if (restore_unregister(items[k].root, items[k].tid)) {
                kept++;
                continue;
            }
            /* one removal at a time: the next waits until the system databases are idle */
            wait_db_quiet(3, 30);
            restore_files(items[k].root, items[k].tid, items[k].name, &moved, &failed);
            done++;
        }
    }
    /* the uninstall events arrive late and mark the games as removed: clear that, so a
     * game added again later is not taken for one removed on the console */
    pump_events(3000);
    for (int i = 0; i < n; i++)
        if (!items[i].other)
            restore_clear_state(items[i].tid);
    clear_removed_dlc();
    g_next_scan = 0;
    if (n == 1 && done)
        notify("%s: %d pkg file(s) moved back%s", items[0].name, moved,
               failed ? ", some could not be moved (see the log)" : "");
    else if (n > 1)
        notify("%d pkg file(s) of %d game(s) moved back%s%s", moved, done,
               failed ? ", some could not be moved (see the log)" : "",
               kept ? ", some games could not leave the library (see the log)" : "");
}

int app_restore(const char *tids, char *msg, size_t cap) {
    if (g_restore_all || g_restore_n) {
        snprintf(msg, cap, "Already moving pkgs back, wait for it to finish");
        return -1;
    }
    if (!tids) {
        g_restore_all = 1;
        snprintf(msg, cap, "Closing the open game and moving the pkgs of all games back to where they came from");
        return 0;
    }
    int n = 0;
    for (const char *p = tids; *p;) {
        size_t len = strcspn(p, ",");
        char tid[10], root[32];
        if (len != 9 || strncmp(p, "CUSA", 4) || strspn(p + 4, "0123456789") < 5) {
            snprintf(msg, cap, "invalid title id: %.20s", p);
            return -1;
        }
        snprintf(tid, sizeof(tid), "%.9s", p);
        if (find_root(tid, root, sizeof(root))) {
            snprintf(msg, cap, "%s is not on any connected drive", tid);
            return -1;
        }
        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= !strcmp(g_restore_tids[i], tid);
        if (!dup && n < MAX_TITLES)
            snprintf(g_restore_tids[n++], 10, "%s", tid);
        p += len;
        if (*p == ',')
            p++;
    }
    if (!n) {
        snprintf(msg, cap, "No games selected");
        return -1;
    }
    g_restore_n = n;
    if (n == 1)
        snprintf(msg, cap, "Closing the open game and moving the pkgs of %s back to where they came from",
                 g_restore_tids[0]);
    else
        snprintf(msg, cap, "Closing the open game and moving the pkgs of %d games back, %d at a time", n,
                 g_cfg.restore_batch);
    return 0;
}

int app_restored_forget(char *msg, size_t cap) {
    unlink(RESTORED_LIST);
    g_next_scan = 0;
    snprintf(msg, cap, "Pkgs moved back will be picked up again on the next scan");
    return 0;
}

/* --- settings --- */

/* A scan folder: relative to the drive, "" for the root. Folders inside ps4/ (where
 * PS4Mount keeps the games) and anything leaving the drive are refused. */
static int clean_scan_path(const char *in, char *out, size_t n, char *why, size_t wn) {
    while (*in == ' ' || *in == '/')
        in++;
    snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '/' || out[l - 1] == '\r'))
        out[--l] = 0;
    if (l >= 120) {
        snprintf(why, wn, "folder name too long: %.40s...", out);
        return -1;
    }
    if (strchr(out, '\\') || strchr(out, ':') || strstr(out, "//")) {
        snprintf(why, wn, "invalid folder: %s", out);
        return -1;
    }
    for (const char *c = out; *c; c = strchr(c, '/') ? strchr(c, '/') + 1 : c + strlen(c))
        if (!strncmp(c, "..", 2) && (c[2] == '/' || !c[2])) {
            snprintf(why, wn, "folders outside the drive are not allowed: %s", out);
            return -1;
        }
    if (!strncasecmp(out, "ps4", 3) && (!out[3] || out[3] == '/')) {
        snprintf(why, wn, "the ps4 folder holds the games PS4Mount already moved; choose another folder");
        return -1;
    }
    return 0;
}

static void config_save(void) {
    buf_t b = {0};
    buf_printf(&b,
               "batch=%d\nbatch_size=%d\ndlc=%d\ndlc_copy=%d\norganize=%d\ninterval=%d\ncross=%d\nrestore_batch=%d\n",
               g_cfg.batch, g_cfg.batch_size, g_cfg.dlc, g_cfg.dlc_copy, g_cfg.organize, g_cfg.interval, g_cfg.cross,
               g_cfg.restore_batch);
    for (int i = 0; i < g_cfg.npaths; i++)
        buf_printf(&b, "path=%s\n", g_cfg.paths[i]);
    for (int i = 0; i < g_cfg.nignore; i++)
        buf_printf(&b, "ignore=%s\n", g_cfg.ignore[i]);
    if (g_cfg.pin[0])
        buf_printf(&b, "pin=%s\n", g_cfg.pin);
    if (b.failed)
        printf("[config] out of memory: settings not saved\n");
    else
        write_file(CONFIG_PATH, b.data, b.len, 0600);
    chmod(CONFIG_PATH, 0600); /* open() keeps the mode of a file that already exists */
    buf_free(&b);
}

/* 4 to 8 digits. */
static int pin_valid(const char *s) {
    size_t l = strspn(s, "0123456789");
    return !s[l] && l >= 4 && l <= 8;
}

const char *app_pin(void) { return g_cfg.pin; }

/* The console's own language, read once and cached: one of the panel's languages
 * ("en"/"es"/"pt"/"fr"), or "" when it is not one of them (the panel then falls back to
 * the browser's own language, as it already did before this existed). */
const char *app_lang(void) {
    static char cached[3] = "?";
    if (cached[0] == '?') {
        cached[0] = 0;
        int id;
        if (!sceSystemServiceParamGetInt(1, &id)) {
            static const struct {
                int id;
                char lang[3];
            } map[] = {{1, "en"}, {18, "en"}, {3, "es"}, {20, "es"}, {7, "pt"}, {17, "pt"}, {2, "fr"}, {22, "fr"}};
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
                if (map[i].id == id) {
                    snprintf(cached, sizeof(cached), "%s", map[i].lang);
                    break;
                }
        }
    }
    return cached;
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return;
    config_t c = g_cfg;
    c.npaths = c.nignore = 0;
    char line[256], why[160], path[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "batch=", 6))
            c.batch = atoi(line + 6) != 0;
        else if (!strncmp(line, "batch_size=", 11))
            c.batch_size = atoi(line + 11);
        else if (!strncmp(line, "dlc=", 4))
            c.dlc = atoi(line + 4) != 0;
        else if (!strncmp(line, "dlc_copy=", 9))
            c.dlc_copy = atoi(line + 9) != 0;
        else if (!strncmp(line, "organize=", 9))
            c.organize = atoi(line + 9) != 0;
        else if (!strncmp(line, "interval=", 9))
            c.interval = atoi(line + 9);
        else if (!strncmp(line, "cross=", 6))
            c.cross = atoi(line + 6) != 0;
        else if (!strncmp(line, "restore_batch=", 14))
            c.restore_batch = atoi(line + 14);
        else if (!strncmp(line, "pin=", 4) && pin_valid(line + 4))
            snprintf(c.pin, sizeof(c.pin), "%s", line + 4);
        else if (!strncmp(line, "ignore=", 7) && c.nignore < MAX_SCAN_PATHS &&
                 !clean_scan_path(line + 7, path, sizeof(path), why, sizeof(why)) && path[0])
            snprintf(c.ignore[c.nignore++], sizeof(c.ignore[0]), "%s", path);
        else if (!strncmp(line, "path=", 5) && c.npaths < MAX_SCAN_PATHS &&
                 !clean_scan_path(line + 5, path, sizeof(path), why, sizeof(why)))
            snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
    }
    fclose(f);
    if (c.batch_size < 1 || c.batch_size > 50)
        c.batch_size = 5;
    if (c.interval < MIN_INTERVAL || c.interval > MAX_INTERVAL)
        c.interval = SCAN_SECONDS;
    if (c.restore_batch < 1 || c.restore_batch > MAX_RESTORE_BATCH)
        c.restore_batch = 5;
    if (!c.npaths)
        c.npaths = 1; /* the root */
    g_cfg = c;
    printf("[config] batch=%d batch_size=%d dlc=%d organize=%d restore_batch=%d folders=%d ignored=%d interval=%d\n",
           g_cfg.batch, g_cfg.batch_size, g_cfg.dlc, g_cfg.organize, g_cfg.restore_batch, g_cfg.npaths, g_cfg.nignore,
           g_cfg.interval);
}

void app_config_json(buf_t *out) {
    buf_printf(out,
               "{\"batch\":%s,\"batch_size\":%d,\"dlc\":%s,\"dlc_copy\":%s,\"organize\":%s,\"interval\":%d,"
               "\"cross\":%s,\"restore_batch\":%d,\"max_restore_batch\":%d,\"paths\":[",
               g_cfg.batch ? "true" : "false", g_cfg.batch_size, g_cfg.dlc ? "true" : "false",
               g_cfg.dlc_copy ? "true" : "false", g_cfg.organize ? "true" : "false", g_cfg.interval,
               g_cfg.cross ? "true" : "false", g_cfg.restore_batch, MAX_RESTORE_BATCH);
    for (int i = 0; i < g_cfg.npaths; i++) {
        char shown[130];
        snprintf(shown, sizeof(shown), "/%s", g_cfg.paths[i]);
        buf_printf(out, "%s", i ? "," : "");
        json_str(out, shown);
    }
    buf_printf(out, "],\"ignore\":[");
    for (int i = 0; i < g_cfg.nignore; i++) {
        char shown[130];
        snprintf(shown, sizeof(shown), "/%s", g_cfg.ignore[i]);
        buf_printf(out, "%s", i ? "," : "");
        json_str(out, shown);
    }
    buf_printf(out, "],\"pin_set\":%s}", g_cfg.pin[0] ? "true" : "false");
}

static void url_decode(const char *in, size_t len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        if (in[i] == '+')
            out[o++] = ' ';
        else if (in[i] == '%' && i + 2 < len) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else
            out[o++] = in[i];
    }
    out[o] = 0;
}

/* body: batch=0|1&batch_size=N&dlc=0|1&dlc_copy=0|1&organize=0|1&interval=N&cross=0|1&restore_batch=N
 * &paths=<folders, one per line>&ignore=<folders, one per line>
 * &pin=<4 to 8 digits, empty to remove; absent keeps it> (form encoded); keys left out keep
 * their value. */
int app_config_set(const char *body, char *msg, size_t cap) {
    config_t c = g_cfg;
    char value[1200], why[160] = "";
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&'), *eq = strchr(p, '=');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (eq && eq < p + len) {
            url_decode(eq + 1, (size_t)(p + len - eq - 1), value, sizeof(value));
            size_t k = (size_t)(eq - p);
            if (k == 5 && !strncmp(p, "batch", 5))
                c.batch = atoi(value) != 0;
            else if (k == 10 && !strncmp(p, "batch_size", 10))
                c.batch_size = atoi(value);
            else if (k == 3 && !strncmp(p, "dlc", 3))
                c.dlc = atoi(value) != 0;
            else if (k == 8 && !strncmp(p, "organize", 8))
                c.organize = atoi(value) != 0;
            else if (k == 8 && !strncmp(p, "dlc_copy", 8))
                c.dlc_copy = atoi(value) != 0;
            else if (k == 13 && !strncmp(p, "restore_batch", 13)) {
                char *end;
                long v = strtol(value, &end, 10);
                c.restore_batch = *value && !*end ? (int)v : 0;
            }
            else if (k == 8 && !strncmp(p, "interval", 8))
                c.interval = atoi(value);
            else if (k == 5 && !strncmp(p, "cross", 5))
                c.cross = atoi(value) != 0;
            else if (k == 3 && !strncmp(p, "pin", 3)) {
                if (value[0] && !pin_valid(value)) {
                    snprintf(msg, cap, "Not saved: the PIN must be 4 to 8 digits");
                    return -1;
                }
                snprintf(c.pin, sizeof(c.pin), "%s", value);
            }
            else if (k == 6 && !strncmp(p, "ignore", 6)) {
                c.nignore = 0;
                for (char *line = strtok(value, "\n"); line; line = strtok(NULL, "\n")) {
                    char path[128];
                    if (clean_scan_path(line, path, sizeof(path), why, sizeof(why))) {
                        snprintf(msg, cap, "Not saved: %s", why);
                        return -1;
                    }
                    if (!path[0]) {
                        snprintf(msg, cap, "Not saved: the drive root cannot be ignored");
                        return -1;
                    }
                    if (c.nignore == MAX_SCAN_PATHS) {
                        snprintf(msg, cap, "Not saved: at most %d folders to ignore", MAX_SCAN_PATHS);
                        return -1;
                    }
                    snprintf(c.ignore[c.nignore++], sizeof(c.ignore[0]), "%s", path);
                }
            }
            else if (k == 5 && !strncmp(p, "paths", 5)) {
                c.npaths = 0;
                for (char *line = strtok(value, "\n"); line; line = strtok(NULL, "\n")) {
                    char path[128];
                    if (clean_scan_path(line, path, sizeof(path), why, sizeof(why))) {
                        snprintf(msg, cap, "Not saved: %s", why);
                        return -1;
                    }
                    int dup = 0;
                    for (int i = 0; i < c.npaths; i++)
                        dup |= !strcmp(c.paths[i], path);
                    if (dup)
                        continue;
                    if (c.npaths == MAX_SCAN_PATHS) {
                        snprintf(msg, cap, "Not saved: at most %d folders", MAX_SCAN_PATHS);
                        return -1;
                    }
                    snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
                }
            }
        }
        p = amp ? amp + 1 : NULL;
    }
    if (c.batch_size < 1 || c.batch_size > 50) {
        snprintf(msg, cap, "Not saved: games per group must be between 1 and 50");
        return -1;
    }
    if (c.interval < MIN_INTERVAL || c.interval > MAX_INTERVAL) {
        snprintf(msg, cap, "Not saved: the time between scans must be between %d and %d seconds", MIN_INTERVAL,
                 MAX_INTERVAL);
        return -1;
    }
    if (c.restore_batch < 1 || c.restore_batch > MAX_RESTORE_BATCH) {
        snprintf(msg, cap, "Not saved: games per group when moving back must be a number from 1 to %d",
                 MAX_RESTORE_BATCH);
        return -1;
    }
    if (!c.npaths) {
        snprintf(msg, cap, "Not saved: add at least one folder (/ is the drive root)");
        return -1;
    }
    if (g_cfg.dlc_copy && !c.dlc_copy)
        g_ac_revert = 1;
    g_cfg = c;
    config_save();
    printf("[config] saved: batch=%d batch_size=%d dlc=%d organize=%d restore_batch=%d folders=%d\n", c.batch,
           c.batch_size, c.dlc, c.organize, c.restore_batch, c.npaths);
    g_next_scan = 0;
    snprintf(msg, cap, "Settings saved");
    return 0;
}

/* --- Media tab shortcut (category 65536 + deeplinkUri, as other PS5 web panels) --- */

#define PANEL_ID "PMNT00001"
static const char k_panel_param[] =
    "{\n"
    "    \"titleId\": \"" PANEL_ID "\",\n"
    "    \"applicationCategoryType\": 65536,\n"
    "    \"deeplinkUri\": \"http://127.0.0.1:8770/\",\n"
    "    \"localizedParameters\": {\n"
    "        \"defaultLanguage\": \"en-US\",\n"
    "        \"en-US\": {\n"
    "            \"titleName\": \"PS4Mount\"\n"
    "        }\n"
    "    }\n"
    "}\n";

static int same_file(const char *path, const void *data, size_t size) {
    struct stat st;
    if (stat(path, &st) || (size_t)st.st_size != size)
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t *tmp = malloc(size);
    int same = tmp && read(fd, tmp, size) == (ssize_t)size && !memcmp(tmp, data, size);
    free(tmp);
    close(fd);
    return same;
}

/* Installs the shortcut when it is missing or out of date (deleting it from the
 * home screen makes the next start install it again). */
static void install_panel_shortcut(void) {
    const char *param = "/user/app/" PANEL_ID "/sce_sys/param.json", *icon = "/user/app/" PANEL_ID "/sce_sys/icon0.png";
    /* sceAppInstUtilAppExists reports 0 for this shortcut on fw 13.20; app.db is reliable */
    int present = db_is_registered(PANEL_ID), rc = 0;
    int same_param = same_file(param, k_panel_param, sizeof(k_panel_param) - 1);
    int same_icon = same_file(icon, icon0_png, sizeof(icon0_png));
    printf("[panel] exists rc=0x%08x present=%d param=%d icon=%d\n", rc, present, same_param, same_icon);
    if (!rc && present && same_param && same_icon)
        return;
    mkdirs("/user/app/" PANEL_ID "/sce_sys", 0755);
    if (write_file(param, k_panel_param, sizeof(k_panel_param) - 1, 0644) ||
        write_file(icon, icon0_png, sizeof(icon0_png), 0644)) {
        printf("[panel] shortcut files: %s\n", strerror(errno));
        return;
    }
    rc = register_call(PANEL_ID);
    printf("[panel] shortcut install rc=0x%08x\n", rc);
    if (!rc)
        notify("PS4Mount added to the Media tab");
}

/* Returns the listening socket, or -1 with errno left as the failing call set it: the
 * genuine "already running" case is bind() failing with EADDRINUSE because another copy
 * holds LOCK_PORT; any other errno is a real failure (out of sockets/permissions/etc.)
 * that the caller must not describe as "already running". */
static int single_instance(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LOCK_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1)) {
        int err = errno;
        if (s >= 0)
            close(s);
        errno = err;
        return -1;
    }
    return s;
}

/* Rest mode closes the network sockets of payloads: a listening socket that stopped
 * listening is found here so it can be opened again. */
static int still_listening(int s) {
    int on = 0;
    socklen_t n = sizeof(on);
    return s >= 0 && !getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &on, &n) && on;
}

static int g_klog = -1, g_web = -1;
static int g_live; /* klogsrv replays its backlog first: live after the first quiet 100 ms */

/* Serves the panel and handles klog lines for up to ms milliseconds. */
static void pump_events(int ms) {
    static char buf[16384], line[4096];
    static size_t ll;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += ms / 1000;
    end.tv_nsec += (long)(ms % 1000) * 1000000;
    for (;;) {
        if (g_klog < 0) {
            g_klog = klog_connect();
            g_live = 0;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = (end.tv_sec - now.tv_sec) * 1000 + (end.tv_nsec - now.tv_nsec) / 1000000;
        if (left <= 0)
            return;
        long wait = g_live || g_klog < 0 ? left : (left < 100 ? left : 100);
        struct timeval tv = {wait / 1000, (wait % 1000) * 1000};
        fd_set fs;
        FD_ZERO(&fs);
        int maxfd = -1;
        if (g_klog >= 0) {
            FD_SET(g_klog, &fs);
            maxfd = g_klog;
        }
        if (g_web >= 0 && !g_masked) { /* panel requests wait until all games show again */
            FD_SET(g_web, &fs);
            if (g_web > maxfd)
                maxfd = g_web;
        }
        if (maxfd < 0) {
            usleep((useconds_t)left * 1000);
            return;
        }
        int ready = select(maxfd + 1, &fs, NULL, NULL, &tv);
        if (ready == 0 && !g_live && g_klog >= 0) {
            g_live = 1;
            printf("[klog] backlog done, following live\n");
        }
        if (ready <= 0)
            continue;
        if (g_web >= 0 && FD_ISSET(g_web, &fs))
            web_serve(g_web);
        if (g_klog < 0 || !FD_ISSET(g_klog, &fs))
            continue;
        ssize_t n = read(g_klog, buf, sizeof(buf));
        if (n <= 0) {
            close(g_klog);
            g_klog = -1;
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (ll < sizeof(line) - 1)
                    line[ll++] = buf[i];
                continue;
            }
            line[ll] = 0;
            ll = 0;
            if (g_live)
                handle_klog_line(line);
        }
    }
}

#define TRASH_KEEP_DAYS 14

/* Really deletes anything in every drive's .trash older than TRASH_KEEP_DAYS, reading each
 * item's deleted-at time from its .trashmeta sidecar (an item with no sidecar is left alone:
 * never silently delete something we cannot confirm the age of). */
static void trash_purge_old(void) {
    time_t now = time(NULL);
    for (int i = 0; i < NUM_DRIVES; i++) {
        char root[32], trash_dir[64];
        drive_root(i, root, sizeof(root));
        if (!is_mountpoint(root))
            continue;
        snprintf(trash_dir, sizeof(trash_dir), "%s/" TRASH_DIR_NAME, root);
        DIR *d = opendir(trash_dir);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 10 || strcmp(e->d_name + l - 10, ".trashmeta"))
                continue;
            char meta[PATH_MAX], item[PATH_MAX];
            snprintf(meta, sizeof(meta), "%s/%s", trash_dir, e->d_name);
            size_t ml = strlen(meta); /* precision must come from the full path, not from e->d_name's
                                        * length, or the slice lands inside trash_dir itself */
            snprintf(item, sizeof(item), "%.*s", (int)(ml - 10), meta); /* meta minus ".trashmeta" */
            struct stat ist;
            if (lstat(item, &ist))
                continue; /* no item for this name: a file legitimately named "*.trashmeta" trashed by
                           * itself, not a real sidecar; leave it alone */
            char buf[PATH_MAX + 64];
            read_file(meta, buf, sizeof(buf));
            char *nl = strchr(buf, '\n');
            long long deleted_at = nl ? atoll(nl + 1) : 0;
            if (deleted_at > 0 && now - (time_t)deleted_at >= TRASH_KEEP_DAYS * 86400) {
                fm_rm_tree(item);
                unlink(meta);
            }
        }
        closedir(d);
    }
}

/* Really deletes the oldest items in root's .trash, one at a time, until drive_space(root)
 * reports avail/total no longer policy_space_low(), or .trash is empty. avail is updated
 * in place so the caller's low-space decision uses the post-reclaim figure. */
static void trash_reclaim(const char *root, unsigned long long *avail, unsigned long long total) {
    char trash_dir[64];
    snprintf(trash_dir, sizeof(trash_dir), "%s/" TRASH_DIR_NAME, root);
    for (int guard = 0; guard < 10000 && policy_space_low(*avail, total); guard++) {
        DIR *d = opendir(trash_dir);
        if (!d)
            return;
        char oldest_item[PATH_MAX] = "", oldest_meta[PATH_MAX] = "";
        long long oldest_ts = -1;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 10 || strcmp(e->d_name + l - 10, ".trashmeta"))
                continue;
            char meta[PATH_MAX], item[PATH_MAX], buf[PATH_MAX + 64];
            snprintf(meta, sizeof(meta), "%s/%s", trash_dir, e->d_name);
            size_t ml = strlen(meta); /* precision must come from the full path, not from e->d_name's
                                        * length, or the slice lands inside trash_dir itself */
            snprintf(item, sizeof(item), "%.*s", (int)(ml - 10), meta); /* meta minus ".trashmeta" */
            struct stat ist;
            if (lstat(item, &ist))
                continue; /* no item for this name: a file legitimately named "*.trashmeta" trashed by
                           * itself, not a real sidecar; leave it alone */
            read_file(meta, buf, sizeof(buf));
            char *nl = strchr(buf, '\n');
            long long ts = nl ? atoll(nl + 1) : -1;
            if (ts > 0 && (oldest_ts < 0 || ts < oldest_ts)) {
                oldest_ts = ts;
                snprintf(oldest_meta, sizeof(oldest_meta), "%s", meta);
                snprintf(oldest_item, sizeof(oldest_item), "%s", item);
            }
        }
        closedir(d);
        if (!oldest_item[0])
            return; /* trash is empty (or every item lost its sidecar): nothing left to reclaim */
        printf("[fm] reclaiming trash on %s: %s\n", root, oldest_item);
        fm_rm_tree(oldest_item);
        unlink(oldest_meta);
        unsigned long long avail2, total2;
        if (drive_space(root, &avail2, &total2))
            return; /* drive_space zeroes its out-params on failure: leave *avail at its last
                      * known-good value instead of letting a transient statfs failure read as "full" */
        *avail = avail2;
    }
}

#define LOG_FILE STATE_DIR "/log.txt"
#define LOG_MAX (1 << 20)   /* log.txt is cut back to its last LOG_KEEP bytes past this size */
#define LOG_KEEP (256 << 10)

/* Keeps log.txt bounded; stdout is opened again on the trimmed file. */
static void log_limit(void) {
    /* The install and speed threads print to stdout; reopening it while they write is undefined. */
    if (g_install_busy || g_speed_busy)
        return;
    if (log_trim(LOG_FILE, LOG_MAX, LOG_KEEP) != 1)
        return;
    /* log_trim renamed a new file over the log, so the timestamping logger still holds the
     * old one: start it again on the trimmed file */
    log_stop();
    log_start(LOG_FILE);
    printf("[main] log trimmed to its last %d KB\n", LOG_KEEP >> 10);
}

#define DIAG_LOG_LINES 200

static int diag_excluded(const char *key, const char *value, void *arg) {
    (void)value;
    buf_printf(arg, " %s", key);
    return 0;
}

void app_diag(buf_t *out) {
    buf_printf(out, "%s\nFirmware %s, registration %s, klog %s\n", VERSION, g_fw_text, reg_mode_name(),
               g_klog >= 0 ? (g_live ? "live" : "connected") : "not connected");
    buf_printf(out, "Adding games: %s%s\n", g_halt[0] ? "PAUSED, " : "on", g_halt);
    buf_printf(out, "Settings: organize=%d dlc=%d batch=%d/%d interval=%d cross=%d folders=%d ignored=%d\n",
               g_cfg.organize, g_cfg.dlc, g_cfg.batch, g_cfg.batch_size, g_cfg.interval, g_cfg.cross, g_cfg.npaths,
               g_cfg.nignore);
    for (int i = 0; i < NUM_DRIVES; i++) {
        char root[32];
        drive_root(i, root, sizeof(root));
        if (!is_mountpoint(root))
            continue;
        const catalog_t *c = &g_cat[i];
        int bad = 0, partial = 0;
        for (int k = 0; k < c->npkgs; k++) {
            bad += c->pkgs[k].state == CAT_BAD;
            partial += c->pkgs[k].state == CAT_SHORT;
        }
        struct statfs sf;
        memset(&sf, 0, sizeof(sf));
        unsigned long long total = 0, avail = 0;
        if (!statfs(root, &sf)) {
            total = (unsigned long long)sf.f_blocks * sf.f_bsize;
            avail = (unsigned long long)sf.f_bavail * sf.f_bsize;
        }
        buf_printf(out,
                   "Drive %s (%s): %d pkg(s), %d game(s), %d unusable, %d incomplete, %d add-on(s) without game, "
                   "%d duplicate(s), %.1f of %.1f GB free\n",
                   root + 5, sf.f_fstypename, c->npkgs, c->ngames, bad, partial, c->orphans, c->dupes,
                   avail / 1073741824.0, total / 1073741824.0);
        pthread_mutex_lock(&g_speed_lock);
        drive_speed_t s = g_speed[i];
        pthread_mutex_unlock(&g_speed_lock);
        if (s.status == SPEED_DONE)
            buf_printf(out, "  speed %.0f MB/s, best %.0f MB/s%s\n", s.mbps, s.best,
                       s.verdict == SPEED_DROP ? " (slower than usual)" : "");
    }
    buf_printf(out, "Failed:");
    for (int i = 0; i < g_retry.n; i++)
        if (g_retry.items[i].tid[0])
            buf_printf(out, " %s(%d)", g_retry.items[i].tid, g_retry.items[i].tries);
    buf_printf(out, "\nNever added:");
    kv_each(EXCLUDED_FILE, diag_excluded, out);
    buf_printf(out, "\n\nLast %d log lines:\n", DIAG_LOG_LINES);
    char *tail = malloc(64 << 10);
    if (tail && log_tail(LOG_FILE, DIAG_LOG_LINES, tail, 64 << 10) >= 0)
        buf_put(out, tail, strlen(tail));
    free(tail);
}

/* The console's address on the local network: the first IPv4 address of an interface
 * that is up, other than loopback. "<PS5 IP>" when there is none. */
static void local_ip(char *out, size_t cap) {
    struct ifaddrs *list, *i;
    snprintf(out, cap, "<PS5 IP>");
    if (getifaddrs(&list))
        return;
    for (i = list; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) &&
            !(i->ifa_flags & IFF_LOOPBACK)) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, cap);
            break;
        }
    freeifaddrs(list);
}

int main(void) {
    int lock = single_instance();
    if (lock < 0) {
        if (errno == EADDRINUSE) {
            printf(VERSION " already running\n");
            notify(VERSION " is already running. Check the autoload list or payload manager for an older copy.");
        } else {
            printf(VERSION ": could not open the instance lock (%s)\n", strerror(errno));
            notify(VERSION " could not start: its instance lock could not be opened");
        }
        return 0;
    }
    sceUserServiceInitialize(0);
    sceAppInstUtilInitialize();
    mkdirs(STATE_DIR, 0777);
    /* the log goes to a file, every line with its time; when it cannot be opened, stdout
     * stays as it was */
    log_trim(LOG_FILE, LOG_MAX, LOG_KEEP);
    if (!log_start(LOG_FILE))
        atexit(log_stop); /* the last lines reach the file before the payload exits */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* must run before a scan or anything else gets a chance to write a new marker */
    {
        char action[16], tids[DOING_MAX];
        if (doing_find(action, sizeof(action), tids, sizeof(tids))) {
            if (!strcmp(action, "adding")) {
                for (char *tid = strtok(tids, ","); tid; tid = strtok(NULL, ",")) {
                    if (strlen(tid) != 9) /* a truncated entry: never make it a retry row */
                        continue;
                    for (int i = 0; i < RETRY_MAX_TRIES; i++)
                        retry_failed(&g_retry, tid, time(NULL),
                                      "Held back: the console may have shut down while this was being added "
                                      "last time. Tap Try again once you're ready to retry.");
                    printf("[safety] %s: held back, was being added when the console last stopped responding\n", tid);
                }
                notify("Some games were held back: the console may have shut down while they were being added "
                       "last time. See the panel to try again.");
            } else if (!strcmp(action, "launching")) {
                kv_set(LAUNCH_HELD_FILE, tids, "1");
                printf("[safety] %s: held, starting it may have caused the console to stop responding last time\n",
                       tids);
            }
            doing_clear(action);
        }
    }
    config_load();
    halt_load();
    updates_load();
    addons_off_load();
    signal(SIGPIPE, SIG_IGN);
    check_firmware();
    if (user_app_masked())
        unmask_leftover(); /* a leftover /user/app mask is a mount-state fix, unrelated to a
                             * database-corruption pause, so it runs regardless of g_halt */
    if (g_reg_mode != REG_NONE && !g_halt[0])
        install_panel_shortcut(); /* registered by the system like the games */
    g_web = web_listen();
    char ip[INET_ADDRSTRLEN];
    local_ip(ip, sizeof(ip));
    if (g_reg_mode == REG_ALL) {
        if (g_web >= 0)
            notify(VERSION " running. Panel in the Media tab or http://%s:8770", ip);
        else
            notify(VERSION " running (panel unavailable)");
    } else if (g_reg_mode == REG_TITLEDIR)
        notify(VERSION " running (firmware %s: games are added one at a time). Panel in the Media tab or "
                       "http://%s:8770",
               g_fw_text, ip);
    else
        notify(VERSION ": firmware %s is not supported yet. To protect the system database, no games are added.",
               g_fw_text);
    if (g_halt[0])
        notify("Adding games is paused: %s. Resume in the PS4Mount panel", g_halt);
    unlink(STATE_DIR "/stop");
    for (;;) {
        if (g_stop || exists(STATE_DIR "/stop")) {
            char msg[192];
            unlink(STATE_DIR "/stop");
            g_stop = 0;
            if (unmount_games(msg, sizeof(msg))) {
                printf("[main] not stopped: %s\n", msg);
                notify(VERSION " not stopped: %s", msg);
                continue;
            }
            notify(VERSION " stopped: the games are unmounted");
            return 0;
        }
        if (!still_listening(lock)) {
            /* lost in rest mode: take it again, or stop if another PS4Mount has it now */
            close(lock);
            if ((lock = single_instance()) < 0) {
                if (errno == EADDRINUSE)
                    notify(VERSION " stopped: another PS4Mount is running");
                else
                    printf("[main] stopped: could not reopen the instance lock (%s)\n", strerror(errno));
                return 0;
            }
            printf("[main] instance lock opened again\n");
        }
        if (!still_listening(g_web)) {
            int was = g_web >= 0;
            if (g_web >= 0)
                close(g_web);
            g_web = web_listen();
            if (was || g_web >= 0) /* log changes only, not every retry */
                printf("[main] web panel %s\n", g_web >= 0 ? "listening again" : "unavailable");
        }
        if (g_deleting && g_deleting_close_at && time(NULL) >= g_deleting_close_at)
            end_delete_window();
        if (g_deleting && time(NULL) - g_deleting_since > 600)
            end_delete_window(); /* the line of the screen change was missed */
        if (g_restore_n || g_restore_all) {
            run_restore(g_restore_all ? NULL : g_restore_tids, g_restore_n);
            g_restore_n = 0;
            g_restore_all = 0;
            continue;
        }
        if (g_launch_tid[0]) {
            char tid[10];
            snprintf(tid, sizeof(tid), "%s", g_launch_tid);
            launch_after_close(tid);
            g_launch_tid[0] = 0;
            continue;
        }
        if (g_dlc_tid[0] && !g_deleting) {
            apply_dlc_choice(g_dlc_tid, g_dlc_on ? g_dlc_on : "");
            g_dlc_tid[0] = 0;
            g_next_scan = 0;
            continue;
        }
        if (g_readd_tid[0] && !g_deleting) {
            char tid[10];
            snprintf(tid, sizeof(tid), "%s", g_readd_tid);
            g_readd_tid[0] = 0;
            readd_title(tid);
            g_next_scan = 0; /* its add-on copies start now, not at the next scan */
            continue;
        }
        {
            /* Queue the fast rescan the moment a copy/move task finishes, from the main loop
             * itself, instead of only when app_fm_task_json() happens to observe the same
             * transition on some client's HTTP poll (which never happens if nobody has the
             * Files tab open). fm_queue_scan() is harmless to call twice for the same task: the
             * one in app_fm_task_json() stays, unchanged, for its own purpose (see there). */
            int fin_drive = fmgr_take_finished_drive();
            if (fin_drive >= 0)
                fm_queue_scan(fin_drive);
        }
        if (time(NULL) >= g_next_scan && !g_deleting) {
            /* not 0 during the scan: a panel request served meanwhile sets 0 and is kept */
            g_next_scan = time(NULL) + g_cfg.interval;
            int was_manual = g_manual_scan, changed = 0;
            if (g_reg_mode != REG_NONE)
                changed = scan_all();
            else if (g_manual_scan)
                notify("Firmware %s is not supported yet: no games are added", g_fw_text);
            g_manual_scan = 0;
            g_scan_only = -1;
            log_limit();
            trash_purge_old();
            g_idle_scans = was_manual || changed ? 0 : g_idle_scans + 1;
            if (g_next_scan)
                g_next_scan = time(NULL) + (g_scan_again ? 0 : g_cfg.interval * policy_scan_backoff(g_idle_scans));
            g_scan_again = 0;
        }
        ac_poll();
        ac_tasks(g_deleting);
        pump_events(1000);
    }
}
