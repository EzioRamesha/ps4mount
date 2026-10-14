# File Manager (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A pkg-aware file manager in the web panel: browse USB drives, rename, new folder, delete, move within a drive, with a pkg detail sheet — all instant operations, no worker thread.

**Architecture:** A new pure module `src/fm.[ch]` holds every path/name decision (host-tested). `src/web.c` gains `/api/fm/*` endpoints that validate with fm.c and call new `app_fm_*` functions declared in `src/web.h` and implemented in `src/main.c` (the only file allowed syscalls). The panel gets a Files dialog in `web/index.html`. Design follows `docs/research/web-file-managers.md`: minimal JSON API in the existing `/api/*` style, fail-closed semantics, no trash bin, in-use pkgs protected server-side.

**Tech Stack:** C (no new dependencies), vanilla JS in the single embedded HTML file, existing test harness (`test/run.sh`, `CHECK(...)` assertions).

## Global Constraints

- Every new source file starts with `/* SPDX-License-Identifier: GPL-3.0-or-later` and the `Copyright (C) 2026 gasb07` line.
- Comments and panel text in plain, simple English sentences; daemon log/messages stay English; every new panel string must be added to the `I18N` object for **es, pt, fr** or `test/i18n_check.js` fails.
- `src/fm.c` must have **no system calls** (not even stat) so it compiles and tests on the host.
- Paths accepted from the panel must start with `/mnt/usb0..7/` or `/mnt/ext0..1/` (matching `drive_root` in `src/main.c:101`), contain no `..` segment, and never be a drive root for mutating operations.
- Mutations fail closed: an existing destination is an error (no silent overwrite). Cross-drive move is refused with a plain message (Phase 2 adds the copy task).
- After any successful mutation set `g_next_scan = 0` so the next scan refreshes the catalog.
- Conventional commits, one purpose per commit.

---

### Task 1: Pure path and name rules (`src/fm.[ch]`)

**Files:**
- Create: `src/fm.h`, `src/fm.c`
- Modify: `test/run.sh:12` (add `src/fm.c` to the `unit` compile line), `build.sh` (add `src/fm.c` to the payload compile line)
- Test: `test/unit.c` (new `test_fm()` registered in `main()` near `test/unit.c:975`)

**Interfaces:**
- Produces: `int fm_drive_of(const char *path)` → drive index 0..9 for a path on a drive, −1 otherwise; `int fm_path_ok(const char *path)` → 1 when the path is on a drive, absolute, has no `..`/`//`/control chars, fits `PATH_MAX`; `int fm_is_drive_root(const char *path)`; `int fm_name_ok(const char *name)` → 1 for a valid new file/folder name (no `/`, not `.`/`..`, 1..240 bytes, no control chars); `int fm_covers(const char *dir, const char *path)` → 1 when `path` equals `dir` or lies under `dir/`.

- [ ] **Step 1: Write the failing tests** — append to `test/unit.c` (before `main`):

```c
static void test_fm(void) {
    printf("file manager path rules\n");
    CHECK(fm_path_ok("/mnt/usb0/games") && fm_drive_of("/mnt/usb0/games") == 0);
    CHECK(fm_path_ok("/mnt/ext1/a b/c.pkg") && fm_drive_of("/mnt/ext1/x") == 9);
    CHECK(fm_path_ok("/mnt/usb7/x") && fm_drive_of("/mnt/usb7/x") == 7);
    CHECK(!fm_path_ok("/mnt/usb8/x"));            /* no such drive */
    CHECK(!fm_path_ok("/data/ps4mount/log.txt")); /* not a drive */
    CHECK(!fm_path_ok("/mnt/usb0/../usb1/x"));    /* .. escape */
    CHECK(!fm_path_ok("/mnt/usb0//x"));           /* empty segment */
    CHECK(!fm_path_ok("mnt/usb0/x"));             /* not absolute */
    CHECK(!fm_path_ok("/mnt/usb0/a\nb"));         /* control char */
    CHECK(fm_path_ok("/mnt/usb0") && fm_is_drive_root("/mnt/usb0"));
    CHECK(fm_is_drive_root("/mnt/usb0/"));
    CHECK(!fm_is_drive_root("/mnt/usb0/games"));
    CHECK(fm_name_ok("Bloodborne v1.09.pkg") && fm_name_ok("games"));
    CHECK(!fm_name_ok("") && !fm_name_ok(".") && !fm_name_ok(".."));
    CHECK(!fm_name_ok("a/b") && !fm_name_ok("a\tb"));
    CHECK(fm_covers("/mnt/usb0/games", "/mnt/usb0/games"));
    CHECK(fm_covers("/mnt/usb0/games", "/mnt/usb0/games/sub/x.pkg"));
    CHECK(!fm_covers("/mnt/usb0/games", "/mnt/usb0/games2/x.pkg"));
}
```

Add `#include "fm.h"` next to the other includes and `test_fm();` in `main()` beside the other `test_*()` calls.

- [ ] **Step 2: Run to verify failure** — `bash test/run.sh`; expected: compile error `fm.h: No such file` (after editing run.sh in step 3 it becomes "undefined symbol" until fm.c exists).

- [ ] **Step 3: Implement** — `src/fm.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Path and name rules of the panel's file manager. No system calls: tested on the host.
 * Paths from the panel are only ever accepted on a drive (/mnt/usbN or /mnt/extN). */
#pragma once

/* Drive index (0..7 usb, 8..9 ext) of a path on a drive, -1 when it is not on one. */
int fm_drive_of(const char *path);
/* 1 when path is absolute, on a drive, free of "..", empty segments and control chars. */
int fm_path_ok(const char *path);
/* 1 when path is exactly a drive root (with or without a trailing slash). */
int fm_is_drive_root(const char *path);
/* 1 when name can be used for a new file or folder (one segment, printable, <= 240 bytes). */
int fm_name_ok(const char *name);
/* 1 when path equals dir or lies under dir/ (used to refuse moving a folder into itself). */
int fm_covers(const char *dir, const char *path);
```

`src/fm.c`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Path and name rules of the panel's file manager. */
#include "fm.h"

#include <string.h>

int fm_drive_of(const char *path) {
    if (strncmp(path, "/mnt/usb", 8) == 0 && path[8] >= '0' && path[8] <= '7' &&
        (path[9] == 0 || path[9] == '/'))
        return path[8] - '0';
    if (strncmp(path, "/mnt/ext", 8) == 0 && path[8] >= '0' && path[8] <= '1' &&
        (path[9] == 0 || path[9] == '/'))
        return 8 + (path[8] - '0');
    return -1;
}

int fm_path_ok(const char *path) {
    size_t n = strlen(path);
    if (n < 9 || n > 1023 || path[0] != '/' || fm_drive_of(path) < 0 || path[n - 1] == '/')
        return fm_is_drive_root(path); /* a root may end with / and has no segments to check */
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)path[i] < 0x20)
            return 0;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len == 0)
            return 0;                                      /* "//" */
        if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
            return 0;
        if (s > path && s[-1] == '/' && s - 1 > path && s[-2] == '/')
            return 0;
    }
    return 1;
}

int fm_is_drive_root(const char *path) {
    char t[16];
    size_t n = strlen(path);
    if (n == 10 && path[9] == '/')
        n = 9;
    if (n != 9 || n >= sizeof(t))
        return 0;
    memcpy(t, path, n);
    t[n] = 0;
    return fm_drive_of(t) >= 0;
}

int fm_name_ok(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > 240)
        return 0;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x20 || name[i] == '/')
            return 0;
    return 1;
}

int fm_covers(const char *dir, const char *path) {
    size_t n = strlen(dir);
    return strncmp(dir, path, n) == 0 && (path[n] == 0 || path[n] == '/');
}
```

Note the `"//"` double-check inside the loop is redundant with the `len == 0` check — drop it. `fm_path_ok` must accept a bare drive root (`/mnt/usb0`) because listing starts there; the early `fm_is_drive_root` return handles the trailing-slash form.

Add `src/fm.c` to the `unit` compile line in `test/run.sh` and to the source list in `build.sh`.

- [ ] **Step 4: Run to verify pass** — `bash test/run.sh`; expected: `... checks, 0 failed` with the new `file manager path rules` line, count up by 19.

- [ ] **Step 5: Commit** — `git add src/fm.c src/fm.h test/unit.c test/run.sh build.sh && git commit -m "feat(fm): path and name rules for the file manager"`

---

### Task 2: Endpoints and seam (`web.h`, `web.c`, `web_unit.c`)

**Files:**
- Modify: `src/web.h` (after `app_launch` declaration), `src/web.c` (route chain in `web_serve`, around `src/web.c:296-407`), `test/run.sh:13` (add `src/fm.c` to the `web_unit` compile line)
- Test: `test/web_unit.c` (stubs beside the other `app_*` stubs at `test/web_unit.c:36-67`, new `test_fm_api()` registered in `main`)

**Interfaces:**
- Consumes: `fm_path_ok`, `fm_name_ok` from Task 1.
- Produces (declared in `src/web.h`, implemented by Task 3/4 in `main.c`, stubbed in `web_unit.c`):

```c
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
```

- [ ] **Step 1: Write failing endpoint tests** — in `test/web_unit.c`, add echo stubs:

```c
int app_fm_list(const char *path, buf_t *out, char *msg, size_t cap) {
    (void)msg; (void)cap;
    buf_printf(out, "{\"path\":\"%s\",\"entries\":[]}", path);
    return 0;
}
int app_fm_mkdir(const char *dir, const char *name, char *msg, size_t cap) {
    return snprintf(msg, cap, "mkdir[%s + %s]", dir, name) < 0;
}
int app_fm_rename(const char *path, const char *name, char *msg, size_t cap) {
    return snprintf(msg, cap, "rename[%s -> %s]", path, name) < 0;
}
int app_fm_delete(const char *path, char *msg, size_t cap) {
    return snprintf(msg, cap, "delete[%s]", path) < 0;
}
int app_fm_move(const char *src, const char *dst, char *msg, size_t cap) {
    return snprintf(msg, cap, "move[%s -> %s]", src, dst) < 0;
}
```

and the test (register `test_fm_api(listen_fd);` in `main` after `test_launch`):

```c
static void test_fm_api(int listen_fd) {
    printf("file manager parameters\n");
    char resp[4096];
    request(listen_fd, "GET /api/fm/list?path=/mnt/usb0/my%20games HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "\"path\":\"/mnt/usb0/my games\"") != NULL); /* %20 decoded */
    request(listen_fd, "GET /api/fm/list?path=/data/ps4mount HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid path") != NULL);
    request(listen_fd, "GET /api/fm/list?path=/mnt/usb0/../usb1 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid path") != NULL);
    request(listen_fd, "POST /api/fm/mkdir?path=/mnt/usb0&name=new%20folder HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "mkdir[/mnt/usb0 + new folder]") != NULL);
    request(listen_fd, "POST /api/fm/mkdir?path=/mnt/usb0&name=a/b HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid name") != NULL);
    request(listen_fd, "POST /api/fm/rename?path=/mnt/usb0/a.pkg&name=b.pkg HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "rename[/mnt/usb0/a.pkg -> b.pkg]") != NULL);
    request(listen_fd, "POST /api/fm/delete?path=/mnt/usb0/a.pkg HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "delete[/mnt/usb0/a.pkg]") != NULL);
    request(listen_fd, "POST /api/fm/delete?path=/mnt/usb0 HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "invalid path") != NULL); /* a drive root is never deleted */
    request(listen_fd, "POST /api/fm/move?src=/mnt/usb0/a.pkg&dst=/mnt/usb0/games HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "move[/mnt/usb0/a.pkg -> /mnt/usb0/games]") != NULL);
    request(listen_fd, "GET /api/fm/delete?path=/mnt/usb0/a.pkg HTTP/1.1\r\n\r\n", resp, sizeof(resp));
    CHECK(strstr(resp, "404") != NULL); /* mutations are POST only */
}
```

- [ ] **Step 2: Run to verify failure** — `bash test/run.sh`; expected: web_unit compile fails (`app_fm_list` undeclared) until web.h is edited, then test failures (404s) until routes exist.

- [ ] **Step 3: Implement the routes** — in `src/web.h` add the five declarations from the Interfaces block (with their comments) after `app_launch`. In `src/web.c` add `#include "fm.h"` and a percent-decoding param helper near `title_param` (`src/web.c:85`):

```c
/* Copies the percent-decoded value of key= from the query into out. -1 when missing/too long. */
static int fm_param(const char *path, const char *key, char *out, size_t cap) {
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(path, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '&' && *p != ' ') {
        char c = *p++;
        if (c == '%' && isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
            char h[3] = {p[0], p[1], 0};
            c = (char)strtol(h, NULL, 16);
            p += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (o + 1 >= cap)
            return -1;
        out[o++] = c;
    }
    out[o] = 0;
    return o ? 0 : -1;
}
```

(`#include <ctype.h>` and `<stdlib.h>` if not already present.) Then extend the `web_serve` chain after the `/api/launch` branch:

```c
    } else if (!post && !strncmp(path, "/api/fm/list", 12)) {
        char p[1024];
        if (fm_param(path, "path", p, sizeof(p)) || !fm_path_ok(p))
            json_msg(fd, 0, "invalid path");
        else {
            buf_t b = {0};
            if (!app_fm_list(p, &b, msg, sizeof(msg)))
                reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
            else
                json_msg(fd, 0, msg);
            buf_free(&b);
        }
    } else if (post && !strncmp(path, "/api/fm/mkdir", 13)) {
        char p[1024], name[256];
        if (fm_param(path, "path", p, sizeof(p)) || !fm_path_ok(p))
            json_msg(fd, 0, "invalid path");
        else if (fm_param(path, "name", name, sizeof(name)) || !fm_name_ok(name))
            json_msg(fd, 0, "invalid name");
        else {
            int rc = app_fm_mkdir(p, name, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/fm/rename", 14)) {
        char p[1024], name[256];
        if (fm_param(path, "path", p, sizeof(p)) || !fm_path_ok(p) || fm_is_drive_root(p))
            json_msg(fd, 0, "invalid path");
        else if (fm_param(path, "name", name, sizeof(name)) || !fm_name_ok(name))
            json_msg(fd, 0, "invalid name");
        else {
            int rc = app_fm_rename(p, name, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/fm/delete", 14)) {
        char p[1024];
        if (fm_param(path, "path", p, sizeof(p)) || !fm_path_ok(p) || fm_is_drive_root(p))
            json_msg(fd, 0, "invalid path");
        else {
            int rc = app_fm_delete(p, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/fm/move", 12)) {
        char s[1024], d[1024];
        if (fm_param(path, "src", s, sizeof(s)) || !fm_path_ok(s) || fm_is_drive_root(s) ||
            fm_param(path, "dst", d, sizeof(d)) || !fm_path_ok(d))
            json_msg(fd, 0, "invalid path");
        else {
            int rc = app_fm_move(s, d, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    }
```

Add `src/fm.c` to the `web_unit` compile line in `test/run.sh`.

- [ ] **Step 4: Run to verify pass** — `bash test/run.sh`; expected: all suites pass, web checks count up by 11, `file manager parameters` printed.

- [ ] **Step 5: Commit** — `git add src/web.h src/web.c test/web_unit.c test/run.sh && git commit -m "feat(fm): file manager endpoints and seam"`

---

### Task 3: Listing with pkg enrichment (`main.c`)

**Files:**
- Modify: `src/main.c` (new functions after `app_launch`, which sits after `app_exclude` around `src/main.c:2900`; add `#include "fm.h"` with the local includes at the top)

**Interfaces:**
- Consumes: `fm_drive_of`, `fm_covers` (Task 1); existing `drive_catalog` (`src/main.c:108`), `db_is_registered` (`src/db.h`), `buf_printf`/`json_str` (`src/web.h`), `g_cat`, `NUM_DRIVES`.
- Produces: `app_fm_list` (signature from Task 2) and an internal helper later tasks reuse:

```c
/* 1 when path is a pkg of a registered game, or a folder holding one (so it must not be
 * deleted or moved). name gets the first such game's name when non-NULL. */
static int fm_protected(const char *path, const char **name);
```

- [ ] **Step 1: Implement `fm_protected`** — the catalog knows every pkg a game uses (`cat_game_t` indices into `cat->pkgs`, see `src/catalog.h:30-60`); a pkg is protected when its game is registered:

```c
static int fm_protected(const char *path, const char **name) {
    for (int d = 0; d < NUM_DRIVES; d++) {
        const catalog_t *cat = &g_cat[d];
        for (int gi = 0; gi < cat->ngames; gi++) {
            const cat_game_t *g = &cat->games[gi];
            if (!db_is_registered(g->title_id))
                continue;
            int idx[2 + CAT_MAX_DLC];
            int n = 0;
            idx[n++] = g->app;
            if (g->patch >= 0)
                idx[n++] = g->patch;
            for (int k = 0; k < g->ndlc && n < (int)(sizeof(idx) / sizeof(idx[0])); k++)
                idx[n++] = g->dlc[k];
            for (int k = 0; k < n; k++) {
                const char *p = cat->pkgs[idx[k]].path;
                if (fm_covers(path, p) || !strcmp(path, p)) {
                    if (name)
                        *name = cat->pkgs[cat->games[gi].app].name;
                    return 1;
                }
            }
        }
    }
    return 0;
}
```

Check the real field names in `src/catalog.h` before coding (`app`, `patch`, `dlc[]`, `ndlc`, `CAT_MAX_DLC` — adjust to what exists; `catalog.h:30-45` has `cat_pkg_t` with `path`, `name`, `app_ver`, `category`, `role`).

- [ ] **Step 2: Implement `app_fm_list`** — readdir + stat each entry, emit JSON, and join pkg rows against the same drive's catalog by path:

```c
int app_fm_list(const char *path, buf_t *out, char *msg, size_t cap) {
    DIR *d = opendir(path);
    if (!d) {
        snprintf(msg, cap, "Cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    const catalog_t *cat = drive_catalog(path); /* adjust: drive_catalog takes the root; use fm_drive_of + g_cat */
    buf_printf(out, "{\"path\":");
    json_str(out, path);
    buf_printf(out, ",\"entries\":[");
    struct dirent *e;
    int first = 1, count = 0;
    while ((e = readdir(d)) && count < 2000) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[1200];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st))
            continue;
        buf_printf(out, "%s{\"name\":", first ? "" : ",");
        json_str(out, e->d_name);
        buf_printf(out, ",\"dir\":%s,\"size\":%llu,\"mtime\":%lld",
                   S_ISDIR(st.st_mode) ? "true" : "false",
                   (unsigned long long)st.st_size, (long long)st.st_mtime);
        const cat_pkg_t *pk = NULL;
        int di = fm_drive_of(path);
        if (!S_ISDIR(st.st_mode) && di >= 0)
            for (int i = 0; i < g_cat[di].npkgs && !pk; i++)
                if (!strcmp(g_cat[di].pkgs[i].path, full))
                    pk = &g_cat[di].pkgs[i];
        if (pk) {
            const char *prot_name = NULL;
            buf_printf(out, ",\"pkg\":{\"title_id\":\"%s\",\"role\":%d,\"version\":", pk->title_id, pk->role);
            json_str(out, pk->app_ver);
            buf_printf(out, ",\"title\":");
            json_str(out, pk->name);
            buf_printf(out, ",\"protected\":%s}", fm_protected(full, &prot_name) ? "true" : "false");
        }
        buf_printf(out, "}");
        first = 0;
        count++;
    }
    closedir(d);
    buf_printf(out, "]}");
    return 0;
}
```

Before coding, confirm the catalog pkg fields (`title_id`, `role`, `app_ver`, `name`) in `src/catalog.h` and whether role is an enum (`CAT_ROLE_*`) — send the role as its number and map to words in the panel (`roleNames` table in Task 5) to keep the JSON small.

- [ ] **Step 3: Compile check** — `main.c` only builds in CI, so run `bash test/run.sh` (host modules unaffected, must stay green) and lean on `gcc -fsyntax-only`-style review; then push after Task 4 and let CI compile it.

- [ ] **Step 4: Commit** — `git add src/main.c && git commit -m "feat(fm): list a folder with pkg details from the catalog"`

---

### Task 4: Mutations with in-use protection (`main.c`)

**Files:**
- Modify: `src/main.c` (after `app_fm_list`)

**Interfaces:**
- Consumes: `fm_protected` (Task 3), `fm_covers`, `fm_drive_of` (Task 1), `mkdirs` (exists, used at `src/main.c:3460`), `g_next_scan`.
- Produces: `app_fm_mkdir`, `app_fm_rename`, `app_fm_delete`, `app_fm_move` (signatures from Task 2).

- [ ] **Step 1: Implement mkdir and rename** — fail closed on an existing destination:

```c
int app_fm_mkdir(const char *dir, const char *name, char *msg, size_t cap) {
    char full[1200];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    if (mkdir(full, 0777)) {
        snprintf(msg, cap, errno == EEXIST ? "%s already exists" : "Could not create %s", name);
        return -1;
    }
    printf("[fm] mkdir %s\n", full);
    snprintf(msg, cap, "Folder %s created", name);
    return 0;
}

int app_fm_rename(const char *path, const char *name, char *msg, size_t cap) {
    const char *game = NULL;
    if (fm_protected(path, &game)) {
        snprintf(msg, cap, "%s is used by %s; remove the game from the library first",
                 strrchr(path, '/') + 1, game ? game : "a game");
        return -1;
    }
    char dst[1200];
    const char *slash = strrchr(path, '/');
    snprintf(dst, sizeof(dst), "%.*s/%s", (int)(slash - path), path, name);
    struct stat st;
    if (!stat(dst, &st)) {
        snprintf(msg, cap, "%s already exists", name);
        return -1;
    }
    if (rename(path, dst)) {
        snprintf(msg, cap, "Could not rename: %s", strerror(errno));
        return -1;
    }
    g_next_scan = 0;
    printf("[fm] rename %s -> %s\n", path, dst);
    snprintf(msg, cap, "Renamed to %s", name);
    return 0;
}
```

- [ ] **Step 2: Implement delete** — recursive, with the protection check running over the whole tree *before* anything is removed (walk once collecting a verdict, then walk again deleting). Reuse the nested-readdir shape of `restore_files` (`src/main.c:2990`-ish):

```c
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
        char sub[1200];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        rc = fm_rm_tree(sub);
    }
    closedir(d);
    return rc ? rc : rmdir(path);
}

int app_fm_delete(const char *path, char *msg, size_t cap) {
    const char *game = NULL;
    if (fm_protected(path, &game)) {
        snprintf(msg, cap, "Used by %s; remove the game from the library first", game ? game : "a game");
        return -1;
    }
    if (fm_rm_tree(path)) {
        snprintf(msg, cap, "Could not delete: %s", strerror(errno));
        return -1;
    }
    g_next_scan = 0;
    printf("[fm] delete %s\n", path);
    snprintf(msg, cap, "Deleted %s", strrchr(path, '/') + 1);
    return 0;
}
```

`fm_protected` already covers "a folder holding a protected pkg" because it tests `fm_covers(path, pkg_path)`.

- [ ] **Step 3: Implement move** — same drive only; destination must be a folder; refuse moving a folder into itself; fail closed on an existing name:

```c
int app_fm_move(const char *src, const char *dst, char *msg, size_t cap) {
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
    struct stat st;
    if (stat(dst, &st) || !S_ISDIR(st.st_mode)) {
        snprintf(msg, cap, "The destination is not a folder");
        return -1;
    }
    char full[1200];
    snprintf(full, sizeof(full), "%s/%s", dst, strrchr(src, '/') + 1);
    if (!stat(full, &st)) {
        snprintf(msg, cap, "%s already exists there", strrchr(src, '/') + 1);
        return -1;
    }
    if (rename(src, full)) {
        snprintf(msg, cap, "Could not move: %s", strerror(errno));
        return -1;
    }
    g_next_scan = 0;
    printf("[fm] move %s -> %s\n", src, full);
    snprintf(msg, cap, "Moved to %s", dst);
    return 0;
}
```

- [ ] **Step 4: Run host tests, push, verify CI** — `bash test/run.sh` stays green; `git add src/main.c && git commit -m "feat(fm): make folder, rename, delete and same-drive move" && git push origin pkgs-in-place`, then `gh workflow run "Build and release" -R EzioRamesha/ps4mount --ref pkgs-in-place` and confirm the SDK build compiles `main.c` (this is the first CI contact for Tasks 3+4).

---

### Task 5: Files dialog in the panel (browse + breadcrumbs)

**Files:**
- Modify: `web/index.html` — header button, new dialog markup after `#probdlg`, CSS after the `.addons` rules, JS after the problems-dialog code.

**Interfaces:**
- Consumes: `GET /api/fm/list` (Task 2/3 JSON shape: `{path, entries:[{name,dir,size,mtime,pkg?:{title_id,role,version,title,protected}}]}`), existing helpers `api()`, `t()`, `esc()`, `gb()`, `$()`.
- Produces: `fmOpen(path)`, `fmRender()`, module state `fm = {path:'', entries:[], sel:{}, moving:null}` that Task 6 extends.

- [ ] **Step 1: Markup** — header: `<button id="filesbtn">Files</button>` after `#probbtn`. Dialog:

```html
<dialog id="fmdlg"><h3>Files</h3>
<div class="fmbar"><span id="fmcrumbs"></span></div>
<div id="fmlist" class="fmlist"></div>
<div class="actions" style="padding:10px 0 0">
  <button id="fmnew">New folder</button>
  <form method="dialog" style="display:contents"><button>Close</button></form>
</div></dialog>
```

- [ ] **Step 2: CSS** —

```css
.fmbar{display:flex;flex-wrap:wrap;gap:4px;align-items:center;margin-bottom:10px;color:var(--muted);font-size:13px}
.fmbar button{min-height:32px;padding:2px 10px;font-size:13px}
.fmlist{max-height:60vh;overflow:auto;border:1px solid var(--line);border-radius:8px}
.fmrow{display:flex;gap:10px;align-items:center;padding:8px 10px;border-bottom:1px solid var(--line);cursor:pointer}
.fmrow:last-child{border-bottom:none}
.fmrow:hover{background:#1d222b}
.fmrow .fmicon{width:36px;height:36px;border-radius:6px;background:#0b0d11 center/cover no-repeat;flex:none;display:flex;align-items:center;justify-content:center;font-size:18px}
.fmrow .fmname{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fmrow small{display:block;color:var(--muted);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.fmrow .fmsize{color:var(--muted);font-size:12px;flex:none}
```

- [ ] **Step 3: JS browse logic** —

```js
var fm = {path: '', entries: [], sel: {}, moving: null};
var roleNames = {0: 'Game', 1: 'Update', 2: 'Add-on'}; /* match CAT_ROLE_* order in src/catalog.h */

function fmOpen(path) {
  api('/api/fm/list?path=' + encodeURIComponent(path)).then(function (j) {
    fm.path = j.path; fm.entries = j.entries; fm.sel = {};
    fmRender();
  }).catch(function (e) { toast(t('Error: {e}', {e: e.message})); });
}

function fmRender() {
  var parts = fm.path.split('/').filter(Boolean); /* mnt usb0 games ... */
  var html = '', acc = '';
  for (var i = 1; i < parts.length; i++) { /* skip "mnt": crumbs start at the drive */
    acc = i === 1 ? '/mnt/' + parts[1] : acc + '/' + parts[i];
    html += (i > 1 ? ' / ' : '') + '<button type="button" data-crumb="' + esc(acc) + '">' + esc(parts[i]) + '</button>';
  }
  $('fmcrumbs').innerHTML = html;
  var rows = fm.entries.slice().sort(function (a, b) {
    return (b.dir - a.dir) || a.name.toLowerCase().localeCompare(b.name.toLowerCase());
  }).map(function (e) {
    var icon = e.dir ? '📁' : (e.pkg ? '' : '📄');
    var iconStyle = e.pkg ? ' style="background-image:url(/icon/' + esc(e.pkg.title_id) + ')"' : '';
    var label = e.pkg ? e.pkg.title : e.name;
    var meta = e.dir ? '' : gb(e.size);
    var sub = e.pkg ? t(roleNames[e.pkg.role] || 'Pkg') + (e.pkg.version ? ' v' + esc(e.pkg.version) : '') +
      (e.pkg.protected ? ' · ' + t('in use') : '') : '';
    return '<div class="fmrow" data-name="' + esc(e.name) + '"><span class="fmicon"' + iconStyle + '>' + icon + '</span>' +
      '<span class="fmname">' + esc(label) + (sub || e.pkg ? '<small>' + esc(e.name) + (sub ? ' · ' + sub : '') + '</small>' : '') + '</span>' +
      '<span class="fmsize">' + meta + '</span></div>';
  }).join('');
  $('fmlist').innerHTML = rows || '<div class="fmrow">' + t('Empty folder') + '</div>';
}

$('filesbtn').addEventListener('click', function () {
  var start = drives.length ? '/mnt/' + drives[0].name : '/mnt/usb0';
  fmOpen($('drive').value ? '/mnt/' + $('drive').value : start);
  $('fmdlg').showModal();
});
$('fmcrumbs').addEventListener('click', function (e) {
  var b = e.target.closest('button[data-crumb]');
  if (b) fmOpen(b.getAttribute('data-crumb'));
});
$('fmlist').addEventListener('click', function (e) {
  var row = e.target.closest('.fmrow[data-name]');
  if (!row) return;
  var entry = fm.entries.filter(function (x) { return x.name === row.getAttribute('data-name'); })[0];
  if (!entry) return;
  if (entry.dir) fmOpen(fm.path + '/' + entry.name);
  else fmSheet(entry); /* Task 6 */
});
```

Note: `showModal()` needs the same fallback older firmown browsers get elsewhere — follow how the other dialogs are opened (search `dialogFallback` in the file) and open `#fmdlg` the same way.

- [ ] **Step 4: Verify with the mock server** — extend the session mock (`scratchpad/mock_panel.py`) with a `/api/fm/list` handler returning a folder with a game pkg (protected), an update pkg, a plain file, and two folders; screenshot with headless Chrome at 1280 px and inside the 390 px iframe wrapper; check rows, crumbs, icons.

- [ ] **Step 5: Commit** — `git add web/index.html && git commit -m "feat(panel): files dialog with pkg-aware rows"` (i18n strings land in Task 7; `test/i18n_check.js` will fail until then, so run only the C suites here or add the strings in the same commit if the check blocks).

---

### Task 6: Selection, actions and the pkg sheet

**Files:**
- Modify: `web/index.html` (extends Task 5's dialog and JS)

**Interfaces:**
- Consumes: `POST /api/fm/mkdir|rename|delete|move` (Task 2 params: `path`, `name`, `src`, `dst`), `fm` state, `fmOpen`, `fmRender`.
- Produces: `fmSheet(entry)` detail sheet; selection mode with Move/Delete/Rename.

- [ ] **Step 1: Pkg/file sheet** — reuse the dialog pattern:

```html
<dialog id="fmsheet"><h3 id="fms-title"></h3>
<div id="fms-body" style="display:flex;gap:14px;align-items:flex-start">
  <div class="fmicon" id="fms-icon" style="width:72px;height:72px;font-size:34px"></div>
  <div id="fms-meta" style="font-size:14px;color:var(--muted)"></div>
</div>
<div class="actions" style="padding:14px 0 0">
  <button id="fms-rename">Rename</button>
  <button id="fms-move">Move</button>
  <button class="danger" id="fms-delete">Delete</button>
  <form method="dialog" style="display:contents"><button>Close</button></form>
</div></dialog>
```

```js
function fmSheet(entry) {
  fm.sheet = entry;
  $('fms-title').textContent = entry.pkg ? entry.pkg.title : entry.name;
  $('fms-icon').style.backgroundImage = entry.pkg ? 'url(/icon/' + entry.pkg.title_id + ')' : '';
  $('fms-icon').textContent = entry.pkg ? '' : (entry.dir ? '📁' : '📄');
  var m = [entry.name, gb(entry.size)];
  if (entry.pkg) {
    m.push(t(roleNames[entry.pkg.role] || 'Pkg') + (entry.pkg.version ? ' v' + entry.pkg.version : ''));
    m.push(entry.pkg.title_id);
    if (entry.pkg.protected) m.push(t('In use by a game in the library: it cannot be moved or deleted'));
  }
  $('fms-meta').innerHTML = m.map(esc).join('<br>');
  $('fms-delete').disabled = $('fms-move').disabled = $('fms-rename').disabled = !!(entry.pkg && entry.pkg.protected);
  $('fmsheet').showModal();
}
```

- [ ] **Step 2: Rename / New folder** — `prompt()` keeps it dependency-free, matching the existing `confirm()` usage:

```js
$('fms-rename').addEventListener('click', function () {
  var n = prompt(t('New name'), fm.sheet.name);
  if (!n || n === fm.sheet.name) return;
  api('/api/fm/rename?path=' + encodeURIComponent(fm.path + '/' + fm.sheet.name) + '&name=' + encodeURIComponent(n), 'POST')
    .then(function (j) { toast(j.message); $('fmsheet').close(); fmOpen(fm.path); })
    .catch(function (e) { toast(t('Error: {e}', {e: e.message})); });
});
$('fmnew').addEventListener('click', function () {
  var n = prompt(t('Folder name'), '');
  if (!n) return;
  api('/api/fm/mkdir?path=' + encodeURIComponent(fm.path) + '&name=' + encodeURIComponent(n), 'POST')
    .then(function (j) { toast(j.message); fmOpen(fm.path); })
    .catch(function (e) { toast(t('Error: {e}', {e: e.message})); });
});
```

- [ ] **Step 3: Delete with a confirm that states what goes** —

```js
$('fms-delete').addEventListener('click', function () {
  var e = fm.sheet;
  var what = (e.pkg ? e.pkg.title + ' (' + e.name + ')' : e.name) + (e.dir ? '' : ' · ' + gb(e.size));
  if (!confirm(t('Delete {what}? This cannot be undone.', {what: what}))) return;
  api('/api/fm/delete?path=' + encodeURIComponent(fm.path + '/' + e.name), 'POST')
    .then(function (j) { toast(j.message); $('fmsheet').close(); fmOpen(fm.path); })
    .catch(function (err) { toast(t('Error: {e}', {e: err.message})); });
});
```

- [ ] **Step 4: Move via "navigate then drop"** — press Move in the sheet → the sheet closes, a bar appears in the files dialog ("Moving `name` — open the destination folder and press Move here"); browse normally; a `Move here` button in `.fmbar` sends `/api/fm/move?src=...&dst=<current fm.path>`:

```js
$('fms-move').addEventListener('click', function () {
  fm.moving = fm.path + '/' + fm.sheet.name;
  $('fmsheet').close();
  fmRender(); /* fmRender shows the moving bar when fm.moving is set */
});
```

In `fmRender`, when `fm.moving` is set, prepend to `#fmcrumbs`'s container:

```js
'<div class="warn" style="flex-basis:100%">' + t('Moving {name}. Open the destination folder and press Move here.',
  {name: esc(fm.moving.split('/').pop())}) +
  ' <button type="button" id="fmdo">' + t('Move here') + '</button>' +
  ' <button type="button" id="fmcancel">' + t('Cancel') + '</button></div>'
```

with a click handler (delegate on `.fmbar`) that calls the move endpoint then clears `fm.moving` and reloads.

- [ ] **Step 5: Verify with the mock server** — add POST handlers to the mock returning `{ok:true,message:"mock ok"}`; click through rename/new-folder/delete/move flows in a headed check or assert via screenshots that the sheet, confirm text, and moving bar render; re-screenshot mobile width.

- [ ] **Step 6: Commit** — `git add web/index.html && git commit -m "feat(panel): rename, delete, new folder and move in the files dialog"`

---

### Task 7: Translations

**Files:**
- Modify: `web/index.html` (the `I18N` object: es at ~line 165, pt at ~246, fr at ~327 — search `'Failed':` to land in each language's state block)

**Interfaces:**
- Consumes: every `t('...')` string introduced in Tasks 5–6.

- [ ] **Step 1: Collect the exact keys** — run `node test/i18n_check.js`; it prints any key used but untranslated. The expected set: `Files`, `New folder`, `Folder name`, `New name`, `Empty folder`, `in use`, `Game`, `Update`, `Add-on`, `Pkg`, `In use by a game in the library: it cannot be moved or deleted`, `Delete {what}? This cannot be undone.`, `Moving {name}. Open the destination folder and press Move here.`, `Move here`, `Move`, `Rename`, `Delete`, `Cancel` (some may already exist — the check tells you).

- [ ] **Step 2: Add all three translations per key.** Spanish/Portuguese/French renderings in the repo's plain style, e.g. es: `'Files': 'Archivos', 'New folder': 'Nueva carpeta', 'Move here': 'Mover aquí', 'Delete {what}? This cannot be undone.': '¿Eliminar {what}? No se puede deshacer.'` — complete the full set for each language; keep `{placeholders}` identical.

- [ ] **Step 3: Verify** — `bash test/run.sh`; the i18n line must end `all translated`.

- [ ] **Step 4: Commit** — `git add web/index.html && git commit -m "feat(panel): translate the file manager"`

---

### Task 8: End-to-end verification and CI

- [ ] **Step 1:** `bash test/run.sh` — all suites green.
- [ ] **Step 2:** Mock-server screenshots at 1280 px and 390 px (iframe wrapper trick — headless Chrome clamps windows below 500 px): files list, pkg sheet, moving bar, delete confirm. Fix any layout breakage.
- [ ] **Step 3:** `git push origin pkgs-in-place`, `gh workflow run "Build and release" -R EzioRamesha/ps4mount --ref pkgs-in-place`, wait for green (SDK compile of Tasks 3/4 code + ASan + fuzz).
- [ ] **Step 4:** Update `README.md` with a short "Files" section (what it does, what it refuses, that delete is permanent) and `CLAUDE.md`'s architecture list with `src/fm.[ch]`; commit `docs(readme): document the file manager`.
- [ ] **Step 5:** Cut `v0.5.0-test.6` (release branch + VERSION bump + tag, the established pattern) for on-console testing: browse real drives, rename/move/delete an unused pkg, confirm a library game's pkg shows "in use" and is refused.

---

## Out of scope (Phase 2 — separate plan)

Cross-drive copy/move as one background worker-thread task with progress/speed/ETA/cancel polling (`GET /api/fm/task`, `POST /api/fm/task/cancel`, 409 while busy), conflict prompts (overwrite/skip/auto-rename), free-space pre-check, temp-name-then-rename writes, file download. See `docs/research/web-file-managers.md` §5–6.

Also deferred from the research's Phase 1 list: **bulk select** (long-press → checkboxes → delete/move several at once). The detail-sheet actions cover every operation one item at a time, the API already supports repeating a request per item (the game grid's multi-select does exactly this), and the selection UI is the costliest part on phones — so it joins Phase 2, where the same selection model is needed for copy anyway.
