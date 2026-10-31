# Stability, Efficiency & Hardware Protection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the daemon back off scanning when idle or when a drive is struggling, stop walking a drive while a game is actively running from it, surface two silent limits (the 256-game library cap and the 64-add-on cap) instead of truncating without a trace, and surface a per-drive unreadable-pkg count as an early failing-drive signal.

**Architecture:** Every decision rule is a small, pure function in `src/policy.c` (no system calls, host-tested in `test/run.sh`'s `== unit` block) or a pure counter in `src/catalog.c`. `src/main.c` is the only place that reads `g_speed[]`, `g_managed[]`, and the scan loop's timers and calls into these functions — it stays the thin, untestable wiring layer the rest of the codebase already uses this split for. No new PS5 syscalls are introduced anywhere; every task uses state the daemon already collects (`g_speed[i].verdict`, `g_managed[].running`, `cat_pkg_t` fields already read by `catalog_read`).

**Tech Stack:** C (PS5 Payload SDK, compiled on-console by `build.sh`), host tests via `test/run.sh` (`cc`/`clang`, no SDK needed), `web/index.html` + 3-language I18N for the two panel-visible changes.

## Already covered — not part of this plan

Research for this plan initially proposed a "crash-quarantine marker" for stability: write a marker before the risky `sceAppInstUtilAppInstallAll` registration call, and on a stale marker at boot, hold back the titles that were mid-registration until the user retries them. **This is already shipped.** `src/main.c` has `doing_mark`/`doing_find`/`doing_clear` (`DOING_FILE`, around `src/main.c:230-280`), used exactly this way around `install_all()` at `src/main.c:2995` (`"adding"` action) and around a game launch at `src/main.c:3861` (`"launching"` action). At startup (`src/main.c:5968-5989`), a stale `"adding"` marker runs `retry_failed()` `RETRY_MAX_TRIES` times for every title in it and notifies the user — precisely the quarantine-and-let-the-user-retry behavior that was going to be proposed as new work. No task below duplicates it.

The deeper root cause the same research surfaced for the recurring "PS5 shuts down" reports (upstream issues #16, #30) — a `SCE_SYSCORE_EVENT_APP_CRASH` tied to `PlayGoCore GetStorageInfo` on a bad pkg, which `sfo_check` (merged in v0.7.1) does not catch — is a real gap, but fixing it needs its own investigation (what exactly in a pkg's PlayGo table triggers it, and whether it can be detected before registration) rather than a code change that can be specified today without guessing. It is intentionally left out of this plan rather than stubbed.

## Global Constraints

- No new PS5 syscalls beyond ones already used in the codebase.
- Pure decision logic goes in `src/policy.c` or `src/catalog.c` (no system calls, host-tested); `src/main.c` only wires it to real state.
- Every `src/policy.c`/`src/catalog.c` addition gets a `CHECK()`-based test in `test/unit.c`, in the existing style (see `test_safety`, `test_catalog_walk`).
- `bash test/run.sh` must pass after every task.
- Comments and any daemon-sent text (log lines, `notify()`) are in plain English, matching the existing tone (see `src/main.c`'s existing `notify()` calls).
- Any new panel-visible text goes into the `I18N` object in `web/index.html` for all three languages (`es`, `pt`, `fr`) and `node test/i18n_check.js` must still pass.
- SPDX header (`SPDX-License-Identifier: GPL-3.0-or-later`) on any new file.
- One commit per task, lowercase `type(scope): description` style matching this repo's log (e.g. `feat(policy): ...`, `fix(main): ...`).
- Do **not** bump `#define VERSION` in any task — that is a separate commit made once, after the whole set has landed and been tested, per this repo's existing release convention.
- `src/main.c` changes cannot be build-verified locally in this environment (no PS5 Payload SDK installed here) — they are verified by reading the diff carefully against the exact line numbers cited in each task, and by the `Build and release` GitHub Actions workflow once pushed.

## Review Focus

1. The per-drive scan-skip logic (Tasks 3-4) must never skip a drive on its first sighting, a manual "Scan for games", or right after it reconnects — `!seen[i] || manual[i]` must stay an unconditional override in every task that touches that line. Test: trace `scan_all()`'s per-drive branch by hand after each of Tasks 2-4.
2. Task 2's backoff must still let `g_scan_again` (the "scan at once" fast-path already used by `app_fm_copy` completions, settings changes, etc.) fire immediately — confirm it is only `g_cfg.interval` that gets multiplied, never the `g_scan_again` branch.
3. Task 6 (the add-on cap raise) must not change which add-ons are chosen as "in use" vs "off" — only how many are listed at all. Test: `test_catalog_group`'s existing add-on-ordering assertions must still pass unmodified.
4. Raising `MAX_TITLES` (Task 1) touches several `[MAX_TITLES]`-sized arrays in `src/main.c` — Task 1's steps include an explicit audit of all of them for any that are stack-allocated (not `static`) instead of a trivial constant bump.
5. Nothing in this plan should change `test/web_unit.c` or `test/http_test.c` behavior — those stub `app_*` and are unaffected by internal `src/main.c` scheduling/policy changes. If a task's steps end up touching them, stop and reconsider the task's scope.

---

### Task 1: Surface and raise the 256-game library cap

**Files:**
- Modify: `src/policy.h`, `src/policy.c` (new pure helper)
- Modify: `test/unit.c` (new test, registered in `main()`)
- Modify: `src/main.c:70` (raise `MAX_TITLES`), `src/main.c:1698-1704` (`manage()`, count the overflow), `src/main.c:3221-3263` (`scan_all()`, reset the counter once per cycle), `src/main.c:3922-4011` (`app_games_json`, add the field)
- Modify: `web/index.html` (summary chip), all 3 language blocks in the `I18N` object

**Interfaces:**
- Produces: `int policy_at_capacity(int n_used, int max);` in `policy.h`/`policy.c` — `1` when `n_used >= max`, else `0`.
- Produces (main.c only, not host-tested): `static int g_managed_full;` — count of games `manage()` could not give a slot to during the current scan cycle.

- [ ] **Step 1: Write the failing test**

Add to `test/unit.c`, inside `test_policy(void)` (find it at `test/unit.c:942`) right before its closing `}`:

```c
    printf("policy: at the title capacity\n");
    CHECK(!policy_at_capacity(254, 256));
    CHECK(!policy_at_capacity(255, 256));
    CHECK(policy_at_capacity(256, 256));
    CHECK(policy_at_capacity(300, 256)); /* already over: still reports full */
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash test/run.sh 2>&1 | grep -A3 "policy: at the title"`
Expected: a compile error (`policy_at_capacity` undeclared), since `test/run.sh` stops the whole suite at a compile failure.

- [ ] **Step 3: Write minimal implementation**

In `src/policy.h`, right after the existing `int policy_speed_verdict(double mbps, double best);` declaration (and its comment block above it):

```c
/* 1 when n_used has already reached max: no free slot for one more managed title. */
int policy_at_capacity(int n_used, int max);
```

In `src/policy.c`, right after `policy_speed_verdict`'s definition:

```c
int policy_at_capacity(int n_used, int max) { return n_used >= max; }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash test/run.sh 2>&1 | grep -B1 -A1 "policy: at the title"`
Expected: no `FAIL` line, and the suite's final `X checks, 0 failed` count goes up by 4.

- [ ] **Step 5: Commit**

```bash
git add src/policy.h src/policy.c test/unit.c
git commit -m "feat(policy): add policy_at_capacity for the title-count cap"
```

- [ ] **Step 6: Audit every `[MAX_TITLES]`-sized array before raising the constant**

Run: `grep -n "MAX_TITLES\]" src/main.c`

For each result, confirm it is either `static` (fine at any size) or a small per-element size (fine as a stack array up to a few tens of KB). As of this plan, these are the sites to check by hand:
- `src/main.c:849` `static char g_ac_left[MAX_TITLES][10];` — static, fine.
- `src/main.c:1089` `static char g_ac_drop[MAX_TITLES][10];` — static, fine.
- `src/main.c:1390` `static managed_t g_managed[MAX_TITLES];` — static; at 1024 entries and `managed_t`'s current size (`used, has_patch, running, ndlc, ndlc_tids` ints + `dlc_tids[MAX_DLC_TIDS][10]` + `tid[10]+cid[37]+name[128]+dir[PATH_MAX]`), this is roughly 1.3 KB/entry, about 1.3 MB total — fine for PS5 RAM.
- `src/main.c:1826` `} g_upd[MAX_TITLES];` — confirm `static` at its declaration; fine either way given its small per-entry size.
- `src/main.c:1967` `static char g_install_tids[MAX_TITLES][10];` — static, fine.
- `src/main.c:2109` `static char tids[MAX_TITLES][10];` (inside `unmask_leftover`) — static, fine.
- `src/main.c:2133` `char new_tids[MAX_TITLES][10];` (inside `mask_user_act`/`mask_user_app`) — **not** `static`: a real stack array. At `[1024][10]` this is 10 KB, well within a normal thread stack; note it in the commit message as checked, do not change its storage class unless a reviewer disagrees.
- `src/main.c:232` `#define DOING_MAX (MAX_TITLES * 11 + 32)`, used for `char tids[DOING_MAX];`-style buffers (e.g. in `main()` at `src/main.c:5969`) — at 1024 this is ~11.3 KB; confirm every buffer sized with `DOING_MAX` is `static` or small enough on the stack the same way.

Also run: `grep -n "const cat_pkg_t \*list\[CAT_MAX_DLC\]\|const cat_pkg_t \*dlc\[2 \* CAT_MAX_DLC\]" src/main.c` — these are unrelated to `MAX_TITLES` (they scale with `CAT_MAX_DLC`, changed in Task 6, not this one); confirm none of them also depend on `MAX_TITLES`.

- [ ] **Step 7: Raise the constant and wire the overflow counter**

In `src/main.c:70`, change:

```c
#define MAX_TITLES 256
```

to:

```c
#define MAX_TITLES 1024
```

In `src/main.c`, right after the `static managed_t g_managed[MAX_TITLES];` declaration (`src/main.c:1390`), add:

```c
static int g_managed_full; /* games manage() could not give a slot this scan cycle */
```

In `manage()` (`src/main.c:1698-1704`), change:

```c
static void manage(const title_src_t *t) {
    managed_t *m = managed_by(t->app.title_id, NULL);
    for (int i = 0; !m && i < MAX_TITLES; i++)
        if (!g_managed[i].used)
            m = &g_managed[i];
    if (!m)
        return;
```

to:

```c
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
```

(`policy_at_capacity(MAX_TITLES, MAX_TITLES)` is always true here — this keeps the call site going through the pure function rather than hand-rolling the comparison again in `main.c`, matching how `policy_space_low` etc. are used elsewhere.)

In `scan_all()` (`src/main.c:3221`), right after `release_stale();`, add:

```c
    g_managed_full = 0;
```

- [ ] **Step 8: Expose it in the panel JSON**

In `app_games_json` (`src/main.c:3922-3927`), change:

```c
void app_games_json(buf_t *out) {
    buf_printf(out, "{\"version\":");
    json_str(out, VERSION);
    buf_printf(out, ",\"paused\":");
    json_str(out, g_halt);
    buf_printf(out, ",\"games\":[");
```

to:

```c
void app_games_json(buf_t *out) {
    buf_printf(out, "{\"version\":");
    json_str(out, VERSION);
    buf_printf(out, ",\"paused\":");
    json_str(out, g_halt);
    buf_printf(out, ",\"games_capped\":%d,\"games\":[", g_managed_full);
```

- [ ] **Step 9: Show it in the panel**

In `web/index.html`, find where `drives` and the summary chips are rendered from the `/api/games` response (around `web/index.html:1054`, `games = j.games; drives = j.drives || [];`). Add, right after that line:

```js
  gamesCapped = j.games_capped || 0;
```

Declare `gamesCapped` alongside the existing `var games = [], busy = {}, drives = [], ...` line (`web/index.html:301`):

```js
var games = [], busy = {}, drives = [], gamesCapped = 0, selected = {}, openAddons = {}, stateFilter = '';
```

Find the summary chip rendering (where the existing `"N game(s)"`/`"Ready: N"` chips are built from `games`) and add one more chip when `gamesCapped > 0`, using the existing chip-building helper in that block, with text:

```js
t('{n} game(s) could not be added: the library is at its {max}-title limit', {n: gamesCapped, max: 1024})
```

Add this new key to the `I18N` object in all three language blocks (`es` around `web/index.html:315-395`, `pt` around `web/index.html:451-531`, `fr` around `web/index.html:586-666` — exact line numbers shift as earlier tasks in this file are not touching these blocks, but string order is preserved, so search for the neighboring `'All drives'` key in each block to find the right spot):

```
'{n} game(s) could not be added: the library is at its {max}-title limit': '<es>',
'{n} game(s) could not be added: the library is at its {max}-title limit': '<pt>',
'{n} game(s) could not be added: the library is at its {max}-title limit': '<fr>',
```

Write real translations (not placeholders) matching this repo's existing tone for similar strings (see `'{name} is almost full: {free} free'`-style entries nearby for phrasing conventions):
- es: `'{n} juego(s) no se pudieron añadir: la biblioteca alcanzó su límite de {max} títulos'`
- pt: `'{n} jogo(s) não puderam ser adicionados: a biblioteca atingiu o limite de {max} títulos'`
- fr: `'{n} jeu(x) n’ont pas pu être ajoutés : la bibliothèque a atteint sa limite de {max} titres'`

- [ ] **Step 10: Run the full test suite**

Run: `bash test/run.sh`
Expected: every suite still prints `0 failed`, and `node test/i18n_check.js` (run as the last step of `test/run.sh`) prints `all translated`.

- [ ] **Step 11: Commit**

```bash
git add src/main.c web/index.html
git commit -m "feat(main): raise the title cap to 1024 and surface it when still hit"
```

---

### Task 2: Global idle-adaptive scan cadence

**Files:**
- Modify: `src/policy.h`, `src/policy.c` (new pure helper)
- Modify: `test/unit.c`
- Modify: `src/main.c:2792` area (`catalog_update`, set a changed flag), `src/main.c:3221` (`scan_all`, reset/read it), `src/main.c:6095-6107` (main loop, use the backoff)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `#define SCAN_BACKOFF_MAX 6` and `int policy_scan_backoff(int idle_in_a_row);` in `policy.h`/`policy.c`, used by Task 3 as a reference for the same doubling-with-cap style (Task 3 defines its own, differently-shaped function — they do not share code, just a pattern).
- Produces (main.c only): `static int g_scan_changed;`, `static int g_idle_scans;`.

- [ ] **Step 1: Write the failing test**

Add to `test/unit.c`, inside `test_policy(void)`, after the `policy_at_capacity` block added in Task 1:

```c
    printf("policy: scan backoff after idle rounds\n");
    CHECK(policy_scan_backoff(0) == 1);
    CHECK(policy_scan_backoff(1) == 1);
    CHECK(policy_scan_backoff(2) == 2);
    CHECK(policy_scan_backoff(3) == 4);
    CHECK(policy_scan_backoff(4) == SCAN_BACKOFF_MAX); /* would be 8: capped at 6 */
    CHECK(policy_scan_backoff(50) == SCAN_BACKOFF_MAX);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash test/run.sh 2>&1 | grep -A3 "policy: scan backoff"`
Expected: compile error, `policy_scan_backoff`/`SCAN_BACKOFF_MAX` undeclared.

- [ ] **Step 3: Write minimal implementation**

In `src/policy.h`, after the `policy_at_capacity` declaration added in Task 1:

```c
/* How many scan rounds to let pass before the next one, once idle_in_a_row consecutive
 * scans found nothing new anywhere: 1 (no backoff) for 0 or 1, doubling after that, capped
 * at SCAN_BACKOFF_MAX so a newly plugged drive or added pkg is still found within
 * SCAN_BACKOFF_MAX * the configured interval. */
#define SCAN_BACKOFF_MAX 6
int policy_scan_backoff(int idle_in_a_row);
```

In `src/policy.c`, after `policy_at_capacity`:

```c
int policy_scan_backoff(int idle_in_a_row) {
    if (idle_in_a_row <= 1)
        return 1;
    int b = 1;
    for (int i = 1; i < idle_in_a_row && b < SCAN_BACKOFF_MAX; i++)
        b *= 2;
    return b > SCAN_BACKOFF_MAX ? SCAN_BACKOFF_MAX : b;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash test/run.sh 2>&1 | grep -B1 -A1 "policy: scan backoff"`
Expected: no `FAIL`.

- [ ] **Step 5: Commit**

```bash
git add src/policy.h src/policy.c test/unit.c
git commit -m "feat(policy): add policy_scan_backoff for idle scan cadence"
```

- [ ] **Step 6: Track whether anything changed this scan**

In `src/main.c`, right after `static time_t g_next_scan;` (`src/main.c:3465`), add:

```c
static int g_scan_changed; /* a drive had unread pkgs the most recent scan_all() call */
static int g_idle_scans;   /* consecutive scan_all() calls with nothing new anywhere */
```

In `catalog_update()` (`src/main.c:2792-2804`), find:

```c
    int unread = catalog_walk(cat, root, walk_skip, ignore, g_cfg.nignore);
    if (fresh && cat->npkgs)
        notify("Scanning %s for PS4 games...", root + 5);
    if (unread) {
        int read = catalog_read(cat);
        printf("[scan] %s: %d pkg(s), %d new or changed read\n", root, cat->npkgs, read);
    }
```

and change it to:

```c
    int unread = catalog_walk(cat, root, walk_skip, ignore, g_cfg.nignore);
    if (fresh && cat->npkgs)
        notify("Scanning %s for PS4 games...", root + 5);
    if (unread) {
        g_scan_changed = 1;
        int read = catalog_read(cat);
        printf("[scan] %s: %d pkg(s), %d new or changed read\n", root, cat->npkgs, read);
    }
```

In `scan_all()` (`src/main.c:3221`), right after the `g_managed_full = 0;` line added in Task 1 (or right after `release_stale();` if Task 1 was skipped), add:

```c
    g_scan_changed = 0;
```

At the end of `scan_all()`, change its signature from `static void scan_all(void) {` to `static int scan_all(void) {`, and change its final statement (currently `speed_poll(present, fresh);`) to:

```c
    speed_poll(present, fresh);
    return g_scan_changed;
}
```

- [ ] **Step 7: Use the backoff in the main loop**

In `main()`'s loop (`src/main.c:6095-6107`), find:

```c
        if (time(NULL) >= g_next_scan && !g_deleting) {
            /* not 0 during the scan: a panel request served meanwhile sets 0 and is kept */
            g_next_scan = time(NULL) + g_cfg.interval;
            if (g_reg_mode != REG_NONE)
                scan_all();
            else if (g_manual_scan)
                notify("Firmware %s is not supported yet: no games are added", g_fw_text);
            g_manual_scan = 0;
            g_scan_only = -1;
            log_limit();
            trash_purge_old();
            if (g_next_scan)
                g_next_scan = time(NULL) + (g_scan_again ? 0 : g_cfg.interval);
            g_scan_again = 0;
        }
```

and change it to:

```c
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
```

- [ ] **Step 8: Run the full test suite**

Run: `bash test/run.sh`
Expected: every suite `0 failed`. (`src/main.c` itself is not compiled by `test/run.sh`; this step confirms nothing in the host-tested modules regressed.)

- [ ] **Step 9: Commit**

```bash
git add src/main.c
git commit -m "feat(main): back off the scan interval after idle rounds"
```

---

### Task 3: Per-drive adaptive scan backoff (idle rounds and a struggling drive)

**Files:**
- Modify: `src/policy.h`, `src/policy.c`
- Modify: `test/unit.c`
- Modify: `src/main.c:2792` (`catalog_update`, record per-drive), `src/main.c:3221-3263` (`scan_all`, per-drive skip)

**Interfaces:**
- Consumes: `g_scan_changed` (Task 2) is extended, not replaced — this task adds a parallel per-drive array next to it in the same function.
- Produces: `#define DRIVE_IDLE_ROUNDS 6` and `int policy_drive_walk_due(int idle_in_a_row, int speed_drop);` in `policy.h`/`policy.c`, consumed by Task 4.
- Produces (main.c only): `static int g_drive_changed[NUM_DRIVES];`, `static int g_drive_idle[NUM_DRIVES];`.

- [ ] **Step 1: Write the failing test**

Add to `test/unit.c`, inside `test_policy(void)`, after Task 2's block:

```c
    printf("policy: per-drive walk due\n");
    CHECK(policy_drive_walk_due(0, 0));  /* always walk while still inside the window */
    CHECK(policy_drive_walk_due(5, 0));
    CHECK(policy_drive_walk_due(6, 0));  /* window edge: a checkpoint walk */
    CHECK(!policy_drive_walk_due(7, 0)); /* backed off: not a multiple of 6 */
    CHECK(!policy_drive_walk_due(11, 0));
    CHECK(policy_drive_walk_due(12, 0)); /* 12 % 6 == 0 */
    CHECK(policy_drive_walk_due(0, 1));  /* speed_drop: still inside its shorter window */
    CHECK(policy_drive_walk_due(1, 1));
    CHECK(!policy_drive_walk_due(2, 1)); /* speed_drop backs off sooner than a healthy drive */
    CHECK(!policy_drive_walk_due(11, 1));
    CHECK(policy_drive_walk_due(12, 1)); /* 12 % 12 == 0 */
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash test/run.sh 2>&1 | grep -A5 "policy: per-drive walk"`
Expected: compile error, `policy_drive_walk_due` undeclared.

- [ ] **Step 3: Write minimal implementation**

In `src/policy.h`, after the `SCAN_BACKOFF_MAX`/`policy_scan_backoff` block from Task 2:

```c
/* Whether to re-walk one drive's folders this scan. idle_in_a_row: consecutive scans with
 * nothing new found on this specific drive. speed_drop: its last read-speed test verdict
 * was SPEED_DROP (policy_speed_verdict) -- walked even less often once idle, since repeated
 * full walks add needless wear to an already struggling drive. Stays true for the first few
 * idle rounds either way (DRIVE_IDLE_ROUNDS, or half that for a dropped drive) so a drive
 * that just went idle is still fully responsive, then backs off to walking only every
 * DRIVE_IDLE_ROUNDS-th scan (every 2*DRIVE_IDLE_ROUNDS-th when speed-dropped). */
#define DRIVE_IDLE_ROUNDS 6
int policy_drive_walk_due(int idle_in_a_row, int speed_drop);
```

In `src/policy.c`, after `policy_scan_backoff`:

```c
int policy_drive_walk_due(int idle_in_a_row, int speed_drop) {
    int window = speed_drop ? 2 : DRIVE_IDLE_ROUNDS;
    int every = speed_drop ? DRIVE_IDLE_ROUNDS * 2 : DRIVE_IDLE_ROUNDS;
    return idle_in_a_row < window || idle_in_a_row % every == 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash test/run.sh 2>&1 | grep -B1 -A1 "policy: per-drive walk"`
Expected: no `FAIL`.

- [ ] **Step 5: Commit**

```bash
git add src/policy.h src/policy.c test/unit.c
git commit -m "feat(policy): add policy_drive_walk_due for per-drive scan backoff"
```

- [ ] **Step 6: Track per-drive change and idle state**

In `src/main.c`, right after the `g_scan_changed`/`g_idle_scans` declarations added in Task 2, add:

```c
static int g_drive_changed[NUM_DRIVES]; /* this drive had unread pkgs the most recent scan */
static int g_drive_idle[NUM_DRIVES];    /* consecutive scans with nothing new on this drive */
```

In `catalog_update()`, find the line added in Task 2 (`g_scan_changed = 1;`, right after `if (unread) {`) and add the per-drive set next to it, using the `idx` already computed earlier in the function (`int idx = (int)(cat - g_cat);`, `src/main.c:2797`):

```c
    if (unread) {
        g_scan_changed = 1;
        g_drive_changed[idx] = 1;
        int read = catalog_read(cat);
        printf("[scan] %s: %d pkg(s), %d new or changed read\n", root, cat->npkgs, read);
    }
```

- [ ] **Step 7: Reset per-drive state and skip the walk when not due**

In `scan_all()`, right after the `g_scan_changed = 0;` line added in Task 2, add a loop clearing this round's per-drive change flags (they get set again during this call if anything changes):

```c
    for (int i = 0; i < NUM_DRIVES; i++)
        g_drive_changed[i] = 0;
```

Find the per-drive loop in `scan_all()` (`src/main.c:3227-3240`):

```c
    for (int i = 0; i < NUM_DRIVES; i++) {
        drive_root(i, root, sizeof(root));
        present[i] = is_mountpoint(root);
        manual[i] = g_manual_scan && (only < 0 || i == only);
        if (present[i] && only >= 0 && i != only && seen[i])
            /* scan of one drive: the others keep their catalog; their games are checked
             * again only when they may use pkgs of the scanned drive */
            ready[i] = g_cfg.cross && g_cat[i].ngames;
        else if (present[i])
            ready[i] = scan_prepare(root, !seen[i] || manual[i], !seen[i] && !first);
        else if (g_cat_loaded[i]) {
            catalog_free(&g_cat[i]); /* checked again when the drive is back: it may have changed */
            g_cat_loaded[i] = 0;
        }
    }
```

and change the `else if (present[i])` branch to:

```c
        else if (present[i]) {
            int first_or_manual = !seen[i] || manual[i];
            int speed_drop = g_speed[i].status == SPEED_DONE && g_speed[i].verdict == SPEED_DROP;
            if (first_or_manual || policy_drive_walk_due(g_drive_idle[i], speed_drop))
                ready[i] = scan_prepare(root, first_or_manual, !seen[i] && !first);
            else
                ready[i] = g_cat[i].ngames > 0; /* reuse the catalog already loaded for it */
        } else if (g_cat_loaded[i]) {
```

(the trailing `else if (g_cat_loaded[i]) {` already exists right after — only the `else if (present[i])` branch's body changes; its closing is unchanged).

- [ ] **Step 8: Update the per-drive idle counter**

In the second per-drive loop in `scan_all()` (`src/main.c:3242-3260`, the one computing `fresh[i]`/`seen[i]` and low-space warnings), find:

```c
        fresh[i] = present[i] && !seen[i];
        seen[i] = present[i];
    }
```

and change it to:

```c
        fresh[i] = present[i] && !seen[i];
        if (!present[i] || fresh[i] || g_drive_changed[i])
            g_drive_idle[i] = 0;
        else if (ready[i])
            g_drive_idle[i]++;
        seen[i] = present[i];
    }
```

- [ ] **Step 9: Run the full test suite**

Run: `bash test/run.sh`
Expected: every suite `0 failed`.

- [ ] **Step 10: Commit**

```bash
git add src/main.c
git commit -m "feat(main): back off re-walking a drive's folders when idle or struggling"
```

---

### Task 4: Skip re-walking a drive while a game from it is actively running

**Files:**
- Modify: `src/main.c:3227-3245` (`scan_all()`, extend Task 3's branch)

**Interfaces:**
- Consumes: `policy_drive_walk_due` (Task 3), `fm_drive_of` (already in `src/fm.h`), `managed_t`/`g_managed[]` (already in `src/main.c`).
- Produces: nothing new for later tasks — this is the last task touching the scan-skip branch.

This task is main.c-only wiring: whether a game is running on a given drive depends on `g_managed[]`, which is daemon runtime state, not something `src/policy.c`'s "no system calls, host-tested" module can see without being handed the whole table — not worth a new pure function for one boolean check that's already a simple loop over existing fields.

- [ ] **Step 1: Add a running-on-this-drive check**

In `src/main.c`, right before `scan_all()` (`src/main.c:3221`), add:

```c
/* 1 when a managed title is open and its pkg is on drive i: that drive's folders should
 * not be re-walked while it is in active use (I/O contention, and the point of letting a
 * drive idle is defeated by scanning it mid-game). */
static int game_running_on_drive(int i) {
    for (int k = 0; k < MAX_TITLES; k++)
        if (g_managed[k].used && g_managed[k].running && fm_drive_of(g_managed[k].dir) == i)
            return 1;
    return 0;
}
```

- [ ] **Step 2: Wire it into the per-drive decision**

In `scan_all()`'s per-drive loop, in the branch Task 3 wrote:

```c
        else if (present[i]) {
            int first_or_manual = !seen[i] || manual[i];
            int speed_drop = g_speed[i].status == SPEED_DONE && g_speed[i].verdict == SPEED_DROP;
            if (first_or_manual || policy_drive_walk_due(g_drive_idle[i], speed_drop))
                ready[i] = scan_prepare(root, first_or_manual, !seen[i] && !first);
            else
                ready[i] = g_cat[i].ngames > 0;
        } else if (g_cat_loaded[i]) {
```

change the `if` condition to also require no game is running from that drive, unless it's a first-sight/manual scan:

```c
        else if (present[i]) {
            int first_or_manual = !seen[i] || manual[i];
            int speed_drop = g_speed[i].status == SPEED_DONE && g_speed[i].verdict == SPEED_DROP;
            int due = !game_running_on_drive(i) && policy_drive_walk_due(g_drive_idle[i], speed_drop);
            if (first_or_manual || due)
                ready[i] = scan_prepare(root, first_or_manual, !seen[i] && !first);
            else
                ready[i] = g_cat[i].ngames > 0;
        } else if (g_cat_loaded[i]) {
```

- [ ] **Step 3: Run the full test suite**

Run: `bash test/run.sh`
Expected: every suite `0 failed` (this task adds no host-testable logic; this step just confirms the surrounding file still builds the host tests cleanly since `fm.c`/`fm.h` are shared).

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "fix(main): don't re-walk a drive a running game is on"
```

---

### Task 5: Surface a per-drive unreadable-pkg count

**Files:**
- Modify: `src/catalog.h`, `src/catalog.c` (new pure helper)
- Modify: `test/unit.c`
- Modify: `src/main.c:4003-4007` (`app_games_json`, the per-drive JSON object)
- Modify: `web/index.html` (drive chip), all 3 language blocks

**Interfaces:**
- Consumes: `cat_pkg_t.state` (already in `catalog.h`), nothing from Tasks 1-4.
- Produces: `int catalog_bad_count(const catalog_t *c);` in `catalog.h`/`catalog.c`.

This counts pkgs the catalog currently cannot read at all (`CAT_BAD`): a real failure to open/parse the file, not pkgs still copying (`CAT_SHORT`, already surfaced separately) and not simply "not a PS4 pkg" content living in a scanned folder being treated as abnormal — `CAT_BAD` already covers both, so this is a coarse but honest "N pkg(s) unreadable here" signal, not a claim of hardware failure. It is meant to be read by a human watching the panel over time, not auto-acted on.

- [ ] **Step 1: Write the failing test**

Add to `test/unit.c`, inside `test_catalog_walk(void)` (`test/unit.c:598`), right after the existing block that already creates `CAT_BAD` entries:

```c
    CHECK(catalog_read(&c) == 3); /* not real pkgs */
    CHECK(c.pkgs[0].state == CAT_BAD);
    CHECK(catalog_bad_count(&c) == 3); /* none of the 3 are real pkgs: all CAT_BAD */
    CHECK(catalog_walk(&c, root, skip_named, NULL, 0) == 0); /* nothing new: nothing to read again */
```

(this inserts one new `CHECK` line right after the existing `CHECK(c.pkgs[0].state == CAT_BAD);` line and before the existing `CHECK(catalog_walk(...) == 0);` line — do not duplicate the two existing lines, only add the new one between them).

- [ ] **Step 2: Run test to verify it fails**

Run: `bash test/run.sh 2>&1 | grep -A3 "catalog: walk of the drive"`
Expected: compile error, `catalog_bad_count` undeclared.

- [ ] **Step 3: Write minimal implementation**

In `src/catalog.h`, right after the `catalog_save`/`catalog_load` declarations (`src/catalog.h:111-112` area), add:

```c
/* How many pkgs of c are currently unreadable (CAT_BAD): a real open/parse failure, not
 * one still copying (CAT_SHORT) and not simply content that was never a PS4Mount pkg.
 * A rising count on one drive over time is worth a look, but this does not itself claim
 * the drive is failing. */
int catalog_bad_count(const catalog_t *c);
```

In `src/catalog.c`, right after `catalog_read` (`src/catalog.c:226-241`):

```c
int catalog_bad_count(const catalog_t *c) {
    int n = 0;
    for (int i = 0; i < c->npkgs; i++)
        n += c->pkgs[i].state == CAT_BAD;
    return n;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash test/run.sh 2>&1 | grep -A6 "catalog: walk of the drive"`
Expected: no `FAIL`.

- [ ] **Step 5: Commit**

```bash
git add src/catalog.h src/catalog.c test/unit.c
git commit -m "feat(catalog): add catalog_bad_count for a per-drive unreadable-pkg count"
```

- [ ] **Step 6: Expose it in the panel JSON**

In `app_games_json` (`src/main.c:4003-4007`), find:

```c
        buf_printf(out, "%s{\"name\":\"%s\",\"games\":%d,\"pkgs\":%d,\"free\":%llu,\"total\":%llu,\"low\":%s",
                   first ? "" : ",", root + 5, g_cat[i].ngames, g_cat[i].npkgs, avail, total,
                   policy_space_low(avail, total) ? "true" : "false");
```

and change it to:

```c
        buf_printf(out, "%s{\"name\":\"%s\",\"games\":%d,\"pkgs\":%d,\"bad\":%d,\"free\":%llu,\"total\":%llu,\"low\":%s",
                   first ? "" : ",", root + 5, g_cat[i].ngames, g_cat[i].npkgs, catalog_bad_count(&g_cat[i]), avail,
                   total, policy_space_low(avail, total) ? "true" : "false");
```

- [ ] **Step 7: Show it in the panel**

In `web/index.html`, find the drive chip rendering that already builds the `"{free} free of {total}"` text (the `'{name}: {free} free of {total}'` key is around `web/index.html:367`). Where each drive's chip text is assembled from the `drives` array, append (only when `d.bad > 0`, `d` being that drive's object):

```js
' · ' + t('{n} pkg(s) unreadable', {n: d.bad})
```

(matching the existing `' · '` separator already used between the free-space and speed segments of that same chip, visible in the live panel as the `·` character).

Add the new key to the `I18N` object in all three language blocks, next to the existing `'{name}: {free} free of {total}'` key:

```
'{n} pkg(s) unreadable': '<es>',
'{n} pkg(s) unreadable': '<pt>',
'{n} pkg(s) unreadable': '<fr>',
```

Real translations:
- es: `'{n} pkg(s) ilegible(s)'`
- pt: `'{n} pkg(s) ilegível(eis)'`
- fr: `'{n} pkg(s) illisible(s)'`

- [ ] **Step 8: Run the full test suite**

Run: `bash test/run.sh`
Expected: every suite `0 failed`, `i18n: ... all translated`.

- [ ] **Step 9: Commit**

```bash
git add src/main.c web/index.html
git commit -m "feat(main): surface a per-drive unreadable-pkg count in the panel"
```

---

### Task 6: Raise the add-on cap, with visibility when still truncated

**Files:**
- Modify: `src/catalog.h:9` (`CAT_MAX_DLC`), `src/main.c:71` (`MAX_DLC`)
- Modify: `src/main.c:454-457` (`dlc_open`, count and surface truncation)
- Modify: `test/unit.c` (adjust/extend the existing add-on-count assumptions if any assume 64)
- Modify: `web/index.html`, 3 language blocks

**Interfaces:**
- Consumes: nothing from Tasks 1-5.
- Produces: nothing consumed by later tasks (this is the last task in the plan).

This deliberately does **not** attempt to avoid the per-add-on `pkg_open`+`read_sfo` cost in `dlc_open` (`src/main.c:450-502`) for every add-on beyond the ones actually in use — each `dlc_src_t` costs roughly 100 KB (it holds a full `pkg_t` + `sfo_t`), so raising the cap to 256 adds up to ~25 MB of transient per-game memory in the worst case, which is acceptable headroom on the PS5, but raising it to cover an outlier case like upstream issue #35's 888-add-on game (~88 MB for that one game alone) is not something to do blindly. 256 (a 4x increase from 64) covers the overwhelming majority of over-cap reports without that risk. A redesign that avoids opening every add-on's pkg at all (using the catalog's already-cached `content_id`/`name` fields and only opening the ones actually toggled on) would remove this ceiling entirely, but it touches roughly a dozen call sites in `src/main.c` that currently assume `dlc_src_t.pkg`/`.sfo` are fully populated for every add-on (see `src/main.c:596`, `796-797`, `1298`, `1584-1585`, `2201-2209`, `3735-3738`, `3832-3835`) and needs its own plan, not a rushed addition here.

- [ ] **Step 1: Raise the constants**

In `src/catalog.h:9`, change:

```c
#define CAT_MAX_DLC 64
```

to:

```c
#define CAT_MAX_DLC 256
```

In `src/main.c:71`, change:

```c
#define MAX_DLC 64
```

to:

```c
#define MAX_DLC 256
```

- [ ] **Step 2: Surface it when still truncated**

In `dlc_open()` (`src/main.c:454-457`):

```c
static void dlc_open(title_src_t *t, const catalog_t *c, const cat_game_t *g) {
    const cat_pkg_t *list[CAT_MAX_DLC];
    int nlist = catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC);
    for (int k = 0; k < nlist && t->ndlc_all < MAX_DLC; k++) {
```

change it to:

```c
static void dlc_open(title_src_t *t, const catalog_t *c, const cat_game_t *g) {
    const cat_pkg_t *list[CAT_MAX_DLC];
    int nlist = catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC);
    if (nlist == CAT_MAX_DLC)
        printf("[scan] %s: has %d or more add-ons, only the first %d are listed\n", t->app.title_id, CAT_MAX_DLC,
               MAX_DLC);
    for (int k = 0; k < nlist && t->ndlc_all < MAX_DLC; k++) {
```

(`nlist == CAT_MAX_DLC` is the signal: `catalog_dlc_of` filled its `out` array completely, meaning there may be more add-ons than `CAT_MAX_DLC` that were never even considered — a log line is enough here since this is a startup-log-visible, rare case, not worth new panel JSON plumbing the way Tasks 1 and 5 were for much more commonly hit limits.)

- [ ] **Step 3: Check for any test assuming the old cap**

Run: `grep -n "MAX_DLC\|CAT_MAX_DLC" test/unit.c`

If any test builds exactly 64 (or `CAT_MAX_DLC`/`MAX_DLC`) add-ons and asserts truncation at that exact count, update the asserted count to match the new 256 — do not leave a test asserting stale behavior. If no such test exists (add-on-count tests in this codebase typically use small numbers like 2-6 add-ons, well under either cap), this step is a no-op; confirm that and move on.

- [ ] **Step 4: Run the full test suite**

Run: `bash test/run.sh`
Expected: every suite `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/catalog.h src/main.c
git commit -m "feat(main): raise the add-on cap to 256 and log when still truncated"
```

---

## Self-Review

**Spec coverage:** Covers 6 of the 7 research proposals (idle-adaptive scan interval, speed/space-aware scan scheduling, drive spin-down via idle skip, don't-scan-a-running-game's-drive, read-error-trend surfacing, add-on cap). The 7th (crash-quarantine marker) was found already implemented during research for this plan (`doing_mark`/`doing_find`, see "Already covered" above) and correctly dropped rather than duplicated.

**Placeholder scan:** No step says "add error handling" or "similar to Task N" without code; every step has the literal diff to make. The one open-ended item (the add-on full-storage redesign) is explicitly called out as **out of scope** with a reason, not left as a vague future step inside a task.

**Type consistency:** `policy_drive_walk_due(int idle_in_a_row, int speed_drop)` (Task 3) and `game_running_on_drive(int i)` (Task 4) signatures are used identically at their one call site in `scan_all()`'s per-drive branch, which Task 4 edits in place rather than redefining. `catalog_bad_count(const catalog_t *c)` matches `catalog_t *` used throughout `catalog.h`. `g_scan_changed`/`g_drive_changed[]` are set in exactly one place (`catalog_update`) and read in exactly one place (`scan_all`) across both tasks that touch them.
