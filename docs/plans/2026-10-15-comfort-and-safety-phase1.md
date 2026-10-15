# Comfort & Safety Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make two things true that aren't today: (1) the panel explains *why* a game failed instead of showing a bare "Failed" label, and (2) deleting something in the file manager is never instantly permanent.

**Architecture:** Part B (plain-language failure reasons) extends the existing host-tested retry table in `src/safety.c` with a reason string, threaded through the three places a registration can actually fail in `src/main.c`, surfaced as a new `fail_reason` field in `app_games_json` and shown in the panel next to the existing retry countdown. Part A (reversible delete) changes `app_fm_delete` from an unconditional `fm_rm_tree()` into a `rename()` into a per-drive `.trash/` folder — the same primitive `app_fm_move` already uses — with two purge paths (age, reused from the existing periodic-maintenance call site; space pressure, reused from the existing low-space check in `scan_all()`) that do the real delete later.

**Tech Stack:** C (no new dependencies), vanilla JS in the single embedded panel file, existing test harness (`test/run.sh`).

## Global Constraints

- Every new/modified source file keeps its `SPDX-License-Identifier: GPL-3.0-or-later` header.
- Comments and panel text in plain, simple English sentences; every new panel string is added to the `I18N` object for **es, pt, fr** or `test/i18n_check.js` fails.
- `src/safety.c` stays system-call-free (host-tested); the trash mechanism's actual syscalls live in `src/main.c`, same as every other file-manager mutation.
- Mutations fail closed: if the trash move itself fails, the file is never deleted — `app_fm_delete` returns an error, nothing is lost.
- `.trash` is never user-configurable and never shows up in a folder listing or a catalog scan — it is skipped the same unconditional way `.`/`..` already are, not through the user's own ignore-folder list.
- Conventional commits, one purpose per commit.

## Roadmap context (not part of this plan)

This plan covers 2 of a 10-item roadmap. For reference, the full list in recommended order: (1) library status strip, **(2) plain-language failure reasons — this plan**, (3) database backup, **(4) reversible delete/trash — this plan**, (5) guided corruption-recovery wizard, (6) database restore, (7) Problems list "Fix all", (8) batch-add progress, (9) named/trackable drives, (10) pre-add integrity check.

---

## Part B — Plain-language failure reasons

### Task 1: Reason storage in the retry table

**Files:**
- Modify: `src/safety.h`, `src/safety.c`
- Test: `test/unit.c` (`test_safety`, in place — see exact diff below)

**Interfaces:**
- Produces:
```c
/* A title id (CUSA00000), when it is next tried again, how many times in a row it
 * failed, and the plain-English reason the last failure gave (empty when none failed yet). */
typedef struct {
    char tid[10];
    time_t until;
    int tries;
    char reason[96];
} retry_t;

/* Records a failure with why it happened; returns how many in a row (0 when the table
 * is full). reason is copied, so the caller's buffer does not need to outlive the call. */
int retry_failed(retry_table_t *t, const char *tid, time_t now, const char *reason);
/* The reason given to the most recent retry_failed() for tid, "" when none is on record
 * (never failed, or retry_clear() was called since). */
const char *retry_reason(const retry_table_t *t, const char *tid);
```

- [ ] **Step 1: Write the failing test** — in `test/unit.c`, change the existing `test_safety` retry section:

```c
    printf("safety: titles that keep failing\n");
    retry_t items[2] = {0};
    retry_table_t t = {items, 2};
    time_t now = 1000;
    CHECK(!retry_waiting(&t, "CUSA00001", now));
    CHECK(retry_remaining(&t, "CUSA00001", now) == 0); /* no entry: nothing pending */
    CHECK(!retry_reason(&t, "CUSA00001")[0]); /* no entry: no reason either */
    CHECK(retry_failed(&t, "CUSA00001", now, "This pkg could not be read.") == 1);
    CHECK(!strcmp(retry_reason(&t, "CUSA00001"), "This pkg could not be read."));
    CHECK(retry_remaining(&t, "CUSA00001", now) == RETRY_SECONDS);
    CHECK(retry_remaining(&t, "CUSA00001", now + RETRY_SECONDS) == 0);
    CHECK(retry_waiting(&t, "CUSA00001", now + RETRY_SECONDS - 1));
    CHECK(!retry_waiting(&t, "CUSA00001", now + RETRY_SECONDS));
    CHECK(retry_failed(&t, "CUSA00001", now, "The console didn't accept this game.") == 2 && !retry_given_up(&t, "CUSA00001"));
    CHECK(!strcmp(retry_reason(&t, "CUSA00001"), "The console didn't accept this game.")); /* latest reason wins */
    CHECK(retry_failed(&t, "CUSA00001", now, "The console didn't accept this game.") == RETRY_MAX_TRIES &&
          retry_given_up(&t, "CUSA00001"));
    CHECK(retry_remaining(&t, "CUSA00001", now) == 0); /* given up: no countdown */
    CHECK(retry_waiting(&t, "CUSA00001", now + 100000)); /* given up: not tried again by itself */
    CHECK(retry_failed(&t, "CUSA00002", now, "") == 1); /* an empty reason is allowed */
    CHECK(!retry_reason(&t, "CUSA00002")[0]);
    CHECK(retry_failed(&t, "CUSA00003", now, "x") == 0); /* table full */
    retry_clear_given_up(&t);
    CHECK(!retry_given_up(&t, "CUSA00001") && !retry_waiting(&t, "CUSA00001", now + 100000));
    CHECK(!retry_reason(&t, "CUSA00001")[0]); /* cleared along with the entry */
    CHECK(retry_waiting(&t, "CUSA00002", now)); /* still waiting: not given up */
    retry_clear(&t, "CUSA00002");
    CHECK(!retry_waiting(&t, "CUSA00002", now));
    CHECK(!retry_reason(&t, "CUSA00002")[0]);
```

- [ ] **Step 2: Run to verify failure** — `bash test/run.sh`; expected: compile error (`retry_failed` called with 4 args against the old 3-arg declaration; `retry_reason` undeclared).

- [ ] **Step 3: Implement** — `src/safety.h`, replace the `retry_t` struct and the two changed declarations:

```c
typedef struct {
    char tid[10];
    time_t until;
    int tries;
    char reason[96];
} retry_t;

/* A table of n entries, zeroed at the start. */
typedef struct {
    retry_t *items;
    int n;
} retry_table_t;

/* 1 while tid waits after a failure, or was given up. */
int retry_waiting(const retry_table_t *t, const char *tid, time_t now);
/* 1 when tid failed RETRY_MAX_TRIES times in a row. */
int retry_given_up(const retry_table_t *t, const char *tid);
/* Seconds until tid is tried again after a failure, 0 when none is pending or it was given up. */
long retry_remaining(const retry_table_t *t, const char *tid, time_t now);
/* Records a failure with why it happened; returns how many in a row (0 when the table
 * is full). reason is copied, so the caller's buffer does not need to outlive the call. */
int retry_failed(retry_table_t *t, const char *tid, time_t now, const char *reason);
/* The reason given to the most recent retry_failed() for tid, "" when none is on record
 * (never failed, or retry_clear() was called since). */
const char *retry_reason(const retry_table_t *t, const char *tid);
void retry_clear(retry_table_t *t, const char *tid);
/* Forgets the titles given up, so they are tried again. */
void retry_clear_given_up(retry_table_t *t);
```

`src/safety.c`, replace `retry_failed` and add `retry_reason`:

```c
int retry_failed(retry_table_t *t, const char *tid, time_t now, const char *reason) {
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
    snprintf(r->reason, sizeof(r->reason), "%s", reason);
    return r->tries;
}

const char *retry_reason(const retry_table_t *t, const char *tid) {
    const retry_t *r = find(t, tid);
    return r ? r->reason : "";
}
```

- [ ] **Step 4: Run to verify pass** — `bash test/run.sh`; expected: `... checks, 0 failed`, count up by 6 from the new CHECKs.

- [ ] **Step 5: Commit** — `git add src/safety.h src/safety.c test/unit.c && git commit -m "feat(safety): record why a title's registration failed"`

---

### Task 2: Give each failure point its own plain-English reason

**Files:**
- Modify: `src/main.c` (`title_failed` and its call sites, all within ~50 lines of `src/main.c:1646` and `src/main.c:1966-1988` as of this plan's writing — confirm with `grep -n "title_failed(" src/main.c` before editing, since line numbers drift)

**Interfaces:**
- Consumes: `retry_failed(t, tid, now, reason)` from Task 1.
- Produces: `title_failed(tid, name, reason)` (was `title_failed(tid, name)`).

- [ ] **Step 1: Update `title_failed`'s signature and body:**

```c
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
```

- [ ] **Step 2: Update the two call sites** (the batch-registration loop). The first call follows `src_open`/`register_prepare` failing — distinguish which of the two failed so the reason is accurate:

```c
            if (src_open(&src, root, t->tid)) {
                title_failed(t->tid, t->name, "This pkg could not be read. It may be damaged or incomplete.");
            } else if (register_prepare(&src)) {
                title_failed(t->tid, t->name,
                             "The files for this game could not be prepared. Check that the drive has enough free space.");
            } else {
                prepared[k] = 1;
                snprintf(ids[np++], 10, "%s", t->tid);
            }
```

The second call follows `register_finish` failing:

```c
            if (register_finish(&src)) {
                if (!g_halt[0]) /* paused: not the title's fault */
                    title_failed(t->tid, t->name, "The console didn't accept this game. This is usually temporary.");
            } else {
```

- [ ] **Step 3: Self-review** — `main.c` cannot be compiled on the host; read both edited blocks back carefully, confirming: no missing `else`, `src.app.fd > 0` close-on-fail path below is unaffected (it is a separate `if` after this block, not touched), and the three reason strings exactly match what Task 3's panel code will key off of (verbatim, not reworded) — Task 3 does not re-derive these strings, it only displays whatever `fail_reason` the JSON carries, so wording only needs to live in one place.

- [ ] **Step 4: Verify** — `bash test/run.sh` (unaffected suites stay green; `main.c` is not part of the host build, so this only guards against collateral damage elsewhere).

- [ ] **Step 5: Commit** — `git add src/main.c && git commit -m "feat(panel): give each registration failure a plain-English reason"`

---

### Task 3: Surface the reason in the API and the panel

**Files:**
- Modify: `src/main.c` (`app_fm_list`... no — `app_games_json`, search `buf_printf(out, ",\"retry_in\":%ld"` to find the exact spot), `web/index.html`
- Test: `test/web_unit.c` is not applicable here (`app_games_json` is stubbed there, not exercised for real) — verification is `bash test/run.sh` staying green plus a visual check.

**Interfaces:**
- Produces: a new `"fail_reason"` string field in each game object of `/api/games`, present (possibly empty) alongside the existing `"retry_in"`.

- [ ] **Step 1: Add the field in `app_games_json`** — immediately after the existing `retry_in` line:

```c
            buf_printf(out, ",\"retry_in\":%ld,\"fail_reason\":", retry_in);
            json_str(out, retry_reason(&g_retry, tid));
```

(`retry_in` is already computed a few lines above this point via `retry_remaining(&g_retry, tid, time(NULL))` — reuse that `tid`, do not re-fetch it.)

- [ ] **Step 2: Panel — show it under the existing retry warning.** In `web/index.html`, find the line added for the retrying countdown:

```js
      (g.state === 'retrying' && g.retry_in > 0 ? '<div class="warn">' + t('Failed; trying again in {n} s', {n: g.retry_in}) + '</div>' : '') + '</div>' +
```

Replace with a version that also covers the given-up `failed` state and shows the reason:

```js
      ((g.state === 'retrying' || g.state === 'failed') && g.fail_reason ?
        '<div class="warn">' + esc(g.fail_reason) +
        (g.state === 'retrying' && g.retry_in > 0 ? ' ' + t('Trying again in {n} s.', {n: g.retry_in}) : '') +
        '</div>' : '') + '</div>' +
```

`g.fail_reason` is daemon-authored English text from a fixed set of three sentences (Task 2) — not pkg-derived or attacker-controlled — but it still goes through `esc()` here, consistent with every other interpolation in this function.

- [ ] **Step 3: Add the translation key** — `t('Trying again in {n} s.', {n: ...})` is new (the old key was `'Failed; trying again in {n} s'`, a single combined sentence; it is now two sentences so the reason and the countdown can be shown independently of each other). Run `node test/i18n_check.js`, it will report the old key as unused and the new one as missing. Remove the three now-unused `'Failed; trying again in {n} s'` entries (one per language) and add `'Trying again in {n} s.'` ×3:
  - es: `'Trying again in {n} s.'` → `'Se reintenta en {n} s.'`
  - pt: `'Trying again in {n} s.'` → `'Tenta de novo em {n} s.'`
  - fr: `'Trying again in {n} s.'` → `'Nouvel essai dans {n} s.'`

  The three reason sentences themselves ("This pkg could not be read...", "The files for this game could not be prepared...", "The console didn't accept this game...") are **not** wrapped in `t(...)` in this task — they arrive pre-rendered from the daemon in English only (the daemon has no i18n). Leaving them English-only, un-translated, is the pragmatic choice for this phase (translating daemon-authored strings would need the panel to pattern-match and re-key English sentences, which is fragile); note this explicitly as a known limitation in the task's self-review rather than silently shipping it.

- [ ] **Step 4: Verify** — `bash test/run.sh` fully green including `i18n: ... all translated`.

- [ ] **Step 5: Commit** — `git add src/main.c web/index.html && git commit -m "feat(panel): show the plain-English reason a game failed"`

---

### Task 4: Reassuring copy on the already-harmless actions

**Files:**
- Modify: `web/index.html` (the `confirm()` text for `unmount`/`exclude` data-actions, in the `$('grid').addEventListener('click', ...)` handler)

- [ ] **Step 1: Check current copy** — `grep -n "Remove {tid} from the library\|Never add {tid}" web/index.html`. Both already mention "The pkg stays on the USB drive" / "The pkgs stay on the drive" — **re-read them before changing anything**; if the reassurance is already present and clear, this task is a no-op and should be marked complete with a one-line note in the commit message rather than inventing a change for its own sake (YAGNI — do not reword working copy just to have a diff).

- [ ] **Step 2: If genuinely missing anywhere** (e.g. the file manager's rename/move confirms, which currently say nothing about reversibility), add a short clause consistent with the existing tone. Do not touch the file manager's **delete** confirm here — that copy changes in Task 8, once trash actually exists; changing it now would promise something not yet true.

- [ ] **Step 3: Verify & commit** — `bash test/run.sh`; `git commit -m "fix(panel): confirm copy reflects that these actions don't delete anything"` (only if Step 2 found something to change).

---

## Part A — Reversible delete (the trash)

### Task 5: `.trash` is invisible to scans and browsing

**Files:**
- Modify: `src/catalog.c` (`walk()`), `src/main.c` (`app_fm_list`)
- Test: `test/unit.c` (catalog walk tests — `catalog.c` is host-tested)

**Interfaces:**
- No new functions; `.trash` is skipped the same unconditional way `.`/`..` already are.

- [ ] **Step 1: Write the failing test** — find the existing `catalog: walk of the drive` test in `test/unit.c` (search `printf("catalog: walk of the drive`). Add a case: create a `.trash` subfolder containing a `.pkg`-named file inside the test's temp drive tree, walk it, and assert the catalog does **not** contain it:

```c
    /* a .trash folder (the file manager's safety net) is never walked into */
    char trash_dir[PATH_MAX], trash_pkg[PATH_MAX];
    snprintf(trash_dir, sizeof(trash_dir), "%s/.trash", root);
    mkdir(trash_dir, 0777);
    snprintf(trash_pkg, sizeof(trash_pkg), "%s/somegame.pkg", trash_dir);
    FILE *tf = fopen(trash_pkg, "w");
    if (tf) {
        fputs("not a real pkg, just needs to exist", tf);
        fclose(tf);
    }
    catalog_t tc2 = {0};
    CHECK(!catalog_walk(&tc2, root, NULL, NULL, 0));
    for (int i = 0; i < tc2.npkgs; i++)
        CHECK(strcmp(tc2.pkgs[i].path, trash_pkg) != 0); /* never discovered */
    catalog_free(&tc2);
```

(Adapt the exact `root`/catalog-free-function names to whatever the surrounding test in `test/unit.c` already uses — read the ~30 lines above the insertion point first; this plan's job is the assertion shape, not re-deriving the existing test fixture's variable names.)

- [ ] **Step 2: Run to verify failure** — `bash test/run.sh`; expected: the new `CHECK` fails because `.trash/somegame.pkg` is currently walked and catalogued like any other file.

- [ ] **Step 3: Implement** — `src/catalog.c`, in `walk()`:

```c
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".trash"))
            continue;
```

`src/main.c`, in `app_fm_list`'s readdir loop:

```c
    while (count < 2000 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".trash"))
            continue;
```

- [ ] **Step 4: Run to verify pass** — `bash test/run.sh` fully green, new check(s) included in the `catalog` suite's count.

- [ ] **Step 5: Commit** — `git add src/catalog.c src/main.c test/unit.c && git commit -m "feat(fm): never scan or list a drive's .trash folder"`

---

### Task 6: `app_fm_delete` moves to trash instead of deleting

**Files:**
- Modify: `src/main.c` (`app_fm_delete`, just above/below `fm_rm_tree`)

**Interfaces:**
- Consumes: `fm_drive_of`, `drive_root` (existing), `mkdirs`, `write_file` (existing, signatures confirmed: `static void mkdirs(const char *path, mode_t mode)`, `static int write_file(const char *path, const void *data, size_t len, mode_t mode)`).
- Produces: `app_fm_delete` keeps its exact existing signature (`int app_fm_delete(const char *path, char *msg, size_t cap)`) — callers (the web.c route, Task 8's panel copy) do not change.
- `fm_rm_tree` is **kept**, unchanged — it becomes the function the purge tasks (7, 8-era... actually Task 7) call to do the real delete later, not something `app_fm_delete` calls directly anymore.

- [ ] **Step 1: Implement** — replace `app_fm_delete`'s body (keep `fm_rm_tree` itself untouched above it):

```c
#define TRASH_DIR_NAME ".trash"

int app_fm_delete(const char *path, char *msg, size_t cap) {
    const char *game = NULL;
    if (fm_protected(path, &game)) {
        snprintf(msg, cap, "Used by %s; remove the game from the library first", game ? game : "a game");
        return -1;
    }
    int di = fm_drive_of(path);
    char root[32], trash_dir[64];
    drive_root(di, root, sizeof(root));
    snprintf(trash_dir, sizeof(trash_dir), "%s/" TRASH_DIR_NAME, root);
    mkdirs(trash_dir, 0700);

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

    g_next_scan = 0;
    printf("[fm] trashed %s -> %s\n", path, dst);
    snprintf(msg, cap, "%s moved to Trash", base);
    return 0;
}
```

Self-review notes to apply while editing (this is console-only code, cannot compile locally — read it back twice):
- `fm_drive_of` can return `-1` in theory, but `app_fm_delete` is only ever called by `web.c` after `fm_path_ok(path)` passed, which guarantees `path` is on a drive — `di` is safe to use unchecked, consistent with how the sibling mutations (`app_fm_rename`, `app_fm_move`) already trust the same precondition without re-checking it.
- The `tries` loop mutates `ts + tries` rather than re-calling `time(NULL)`, so it is deterministic and cannot spin forever waiting for the clock to tick.
- `meta`'s `write_file` return value is intentionally ignored (best-effort) — a lost sidecar means that one item is never auto-purged by age, not that anything is unsafe; do not make the whole delete fail over a missing metadata file.

- [ ] **Step 2: Verify** — `bash test/run.sh` stays fully green (console-only code, not part of the host build; this only guards collateral damage).

- [ ] **Step 3: Commit** — `git add src/main.c && git commit -m "feat(fm): delete moves a file to Trash instead of removing it"`

---

### Task 7: Purge trash by age and by space pressure

**Files:**
- Modify: `src/main.c` (new `trash_purge_old`, new `trash_reclaim`, wired into `scan_all()` and the main loop)

**Interfaces:**
- Produces:
```c
/* Really deletes anything in every drive's .trash older than TRASH_KEEP_DAYS, reading each
 * item's deleted-at time from its .trashmeta sidecar (an item with no sidecar is left alone:
 * never silently delete something we cannot confirm the age of). */
static void trash_purge_old(void);
/* Really deletes the oldest items in root's .trash, one at a time, until drive_space(root)
 * reports avail/total no longer policy_space_low(), or .trash is empty. avail is updated
 * in place so the caller's low-space decision uses the post-reclaim figure. */
static void trash_reclaim(const char *root, unsigned long long *avail, unsigned long long total);
```

- [ ] **Step 1: Add a small file-reading helper.** `main.c` has `write_file` but nothing symmetrical for reading a short text file (the test suite's `file_text` in `test/unit.c` is test-only, not available here). Before writing `trash_purge_old`, add this next to `write_file`:

```c
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
```

- [ ] **Step 2: Implement `trash_purge_old`** — place it near `log_limit()` (same section, both are periodic-maintenance helpers):

```c
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
            /* strip the trailing ".trashmeta" from the FULL path: the precision must come from
             * strlen(meta), not from l (the bare dirent name) — using l here takes the first
             * l-10 bytes of the longer path instead, which for an ordinary short filename lands
             * exactly on trash_dir and would hand the whole trash folder to fm_rm_tree */
            size_t ml = strlen(meta);
            snprintf(item, sizeof(item), "%.*s", (int)(ml - 10), meta);
            struct stat ist;
            if (stat(item, &ist))
                continue; /* no item for this sidecar (or the "sidecar" is really a trashed
                           * file whose own name ends in .trashmeta): leave both alone */
            char buf[PATH_MAX + 64];
            read_file(meta, buf, sizeof(buf));
            char *nl = strchr(buf, '\n');
            long long deleted_at = nl ? atoll(nl + 1) : 0;
            /* > 0, not just non-zero: a corrupted sidecar parsing to a negative number would
             * otherwise make now - deleted_at exceed any threshold and force a blind delete */
            if (deleted_at > 0 && now - (time_t)deleted_at >= TRASH_KEEP_DAYS * 86400) {
                fm_rm_tree(item);
                unlink(meta);
            }
        }
        closedir(d);
    }
}
```

- [ ] **Step 3: Implement `trash_reclaim`** and wire both functions in:

```c
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
            char meta[PATH_MAX], buf[PATH_MAX + 64];
            snprintf(meta, sizeof(meta), "%s/%s", trash_dir, e->d_name);
            size_t ml = strlen(meta);
            char item[PATH_MAX];
            snprintf(item, sizeof(item), "%.*s", (int)(ml - 10), meta); /* see trash_purge_old */
            struct stat ist;
            if (stat(item, &ist))
                continue; /* no item for this sidecar: leave both alone */
            read_file(meta, buf, sizeof(buf));
            char *nl = strchr(buf, '\n');
            long long ts = nl ? atoll(nl + 1) : -1;
            /* > 0, not >= 0: atoll returns 0 for a garbled line, and 0 would always sort as
             * the oldest candidate, so a corrupted sidecar would be reclaimed first of all */
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
        unsigned long long total2;
        if (drive_space(root, avail, &total2))
            return;
    }
}
```

Wire `trash_reclaim` into `scan_all()`, immediately before the existing low-space decision (search `int low = present[i] && !drive_space(root, &avail, &total) && policy_space_low(avail, total);`):

```c
        unsigned long long avail = 0, total = 0;
        int have_space = present[i] && !drive_space(root, &avail, &total);
        if (have_space && policy_space_low(avail, total))
            trash_reclaim(root, &avail, total);
        int low = have_space && policy_space_low(avail, total);
```

The surrounding loop currently declares `unsigned long long avail, total;` fresh on the line right before the one being replaced (one declaration per `for` iteration, confirmed in the current source), feeding straight into a single short-circuit `&&` chain that never reads `avail`/`total` unless `drive_space` actually succeeded. Keep that same guarantee: do not read `avail`/`total` in the final `low` line without `have_space` gating it first — a naive rewrite that drops the short-circuit (e.g. checking `avail && total` directly instead of `have_space`) reads uninitialized memory whenever the drive is absent or `drive_space` fails.

Wire `trash_purge_old()` into the main loop beside the existing `log_limit()` call:

```c
            log_limit();
            trash_purge_old();
```

- [ ] **Step 4: Self-review** — confirm `fm_rm_tree` (private to `app_fm_delete` until now) is still declared `static` and visible at both new call sites (it must be defined textually above `trash_purge_old`/`trash_reclaim`, or forward-declared); confirm the `guard < 10000` cap in `trash_reclaim` cannot spin — each successful iteration either frees the oldest item (shrinking the directory) or returns, so the cap is a pure defense-in-depth backstop, not load-bearing logic.

- [ ] **Step 5: Verify** — `bash test/run.sh` fully green.

- [ ] **Step 6: Commit** — `git add src/main.c && git commit -m "feat(fm): purge trash by age, and reclaim it first when a drive runs low"`

---

### Task 8: Panel copy reflects that delete is now reversible

**Files:**
- Modify: `web/index.html` (the file manager's delete confirm, in the `fms-delete` click handler)

- [ ] **Step 1: Find and replace** the delete confirmation. Search `Delete {what}? This cannot be undone.`:

```js
  if (!confirm(t('Delete {what}? It moves to Trash and is kept for {days} days.', {what: what, days: 14}))) return;
```

- [ ] **Step 2: Translate** — add `'Delete {what}? It moves to Trash and is kept for {days} days.'` ×3, remove the old `'Delete {what}? This cannot be undone.'` ×3:
  - es: `'¿Borrar {what}? Se mueve a la Papelera y se guarda durante {days} días.'`
  - pt: `'Apagar {what}? Vai para a Lixeira e fica guardado por {days} dias.'`
  - fr: `'Supprimer {what} ? Il va dans la Corbeille et y reste {days} jours.'`

- [ ] **Step 3: Verify** — `bash test/run.sh` fully green including i18n.

- [ ] **Step 4: Commit** — `git add web/index.html && git commit -m "fix(panel): delete confirm reflects that it now goes to Trash"`

---

## Task 9: End-to-end verification and release

- [ ] **Step 1:** `bash test/run.sh` — every suite green, i18n fully translated.
- [ ] **Step 2:** Push, `gh workflow run "Build and release"`, confirm CI green (this is the first real compile of Tasks 2, 6, 7's console-only code — SDK build must succeed).
- [ ] **Step 3:** Update `README.md` with a short note under the Files section: delete moves to Trash, kept 14 days or until space is needed; and a one-line note that Failed/Retrying cards now show why.
- [ ] **Step 4:** Cut the next `v0.5.0-test.N` release for on-console testing. Test specifically: delete a junk file in the file manager, confirm it disappears from the listing but the drive's free space is unchanged (it moved, not vanished); manually inspect `.trash/` via FTP to confirm the file and its `.trashmeta` sidecar exist; force a registration failure (e.g. a deliberately corrupted pkg) and confirm the Failed card shows the plain-English reason, not just a bare label.

## Out of scope (future roadmap items, not this plan)

A "Recently Deleted" browse-and-restore panel view, database backup/restore, the corruption-recovery wizard, Problems-list fix-all, batch-add progress, named/trackable drives, and the pre-add integrity check — see the roadmap context section above. Each needs its own design pass.
