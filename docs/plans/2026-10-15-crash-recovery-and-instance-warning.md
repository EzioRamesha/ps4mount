# Crash Recovery & Instance Warning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop two real, reported failure modes: (1) a game that crashed the console while being added or started gets tried again on every single boot, forever, because nothing remembers it happened; (2) a second copy of PS4Mount silently refuses to start with no visible explanation, so an old autoload entry looks like the payload just isn't working.

**Architecture:** A single marker file (`STATE_DIR "/doing.txt"`) is written immediately before each risky operation (the group registration call, a launch) and cleared immediately after it returns — "returns" is the signal the console survived, regardless of success or failure. On the next boot, if the marker is still there, the console didn't come back from whatever it named, so those titles are held back: add-side holds reuse the existing retry/given-up mechanism and its "Try again" button and (once the companion comfort-and-safety plan lands) its plain-language reason display, so no new panel state is needed; launch-side holds are a new small `kv_*`-backed file, cleared by the ordinary act of pressing Play again. The instance-warning fix is a two-line change to an existing, already-silent code path.

**Tech Stack:** C, no new dependencies. `kv_*` helpers already exist in `src/policy.c`.

## Global Constraints

- Every modified file keeps its SPDX header.
- The marker file's job is narrow: remember what was in flight, nothing else. It is not a general crash log.
- Clearing the marker must happen on every return path of the bracketed call (success, failure, or a timeout that still returned) — the only case that must leave it behind is the process never reaching the clear at all, i.e. the console actually went down.
- Conventional commits, one purpose per commit.
- This plan assumes `docs/plans/2026-10-15-comfort-and-safety-phase1.md`'s Task 1-3 (the `fail_reason` field and its panel display) may or may not have landed yet. Task 2 below is written to work either way: it calls `title_failed` with whatever signature currently exists in `main.c` at edit time — check with `grep -n "static void title_failed" src/main.c` before editing, and pass a reason argument only if that plan's Task 2 is already in place.

---

## Part 1 — Skip the game that crashed last time

### Task 1: The marker file primitive

**Files:**
- Modify: `src/main.c` (near `write_file`/`read_file` — if `read_file` already exists from the comfort-and-safety plan's Task 7, skip re-adding it and only add the two functions below)

**Interfaces:**
- Produces:
```c
#define DOING_FILE STATE_DIR "/doing.txt"

/* Marks action (e.g. "adding", "launching") as in progress for tids (comma-separated,
 * no spaces). Overwrites any previous marker: only one risky operation is ever tracked
 * at a time, matching how the daemon actually runs them (one batch, or one launch). */
static void doing_mark(const char *action, const char *tids);
/* Clears the marker. Called right after the bracketed operation returns, on every path:
 * success, failure, or a timeout that still returned. Only a console that did not come
 * back at all leaves this uncleared. */
static void doing_clear(void);
/* Reads the marker left by a previous run that did not clear it. action/tids are sized
 * for the same things doing_mark takes. Returns 0 (nothing was in progress) or 1 (action
 * and tids are filled in; the caller must still doing_clear() once it has acted on this). */
static int doing_find(char *action, size_t asz, char *tids, size_t tsz);
```

- [ ] **Step 1: Implement** — place these near `write_file`/`read_file`:

```c
#define DOING_FILE STATE_DIR "/doing.txt"

static void doing_mark(const char *action, const char *tids) {
    char body[512];
    int n = snprintf(body, sizeof(body), "%s\n%s\n", action, tids);
    write_file(DOING_FILE, body, (size_t)n, 0600);
}

static void doing_clear(void) {
    unlink(DOING_FILE);
}

static int doing_find(char *action, size_t asz, char *tids, size_t tsz) {
    char buf[512];
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
```

`read_file`'s contract (from the comfort-and-safety plan, or add it fresh here if that plan has not landed): "reads up to cap-1 bytes of path into out, 0-terminated, returns the byte count, 0 on failure." If adding it fresh:

```c
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

- [ ] **Step 2: Verify** — `bash test/run.sh` stays green (console-only code, no host-testable surface; this guards against collateral syntax damage elsewhere).

- [ ] **Step 3: Commit** — `git add src/main.c && git commit -m "feat(safety): a marker file for the risky operation currently in progress"`

---

### Task 2: Bracket the add-batch registration call, and hold back on the next boot

**Files:**
- Modify: `src/main.c` (the batch-registration loop around `install_all(ids, np);`, and the startup sequence in `main()`)

**Interfaces:**
- Consumes: `doing_mark`, `doing_clear`, `doing_find` (Task 1); `retry_failed` (existing, or the 4-arg form from the comfort-and-safety plan's Task 1 if that has landed — check `grep -n "int retry_failed" src/safety.h` before editing and match whichever signature is live).

- [ ] **Step 1: Bracket `install_all`.** Find the exact current line (confirm with `grep -n "install_all(ids, np);" src/main.c` — context as of this writing is immediately preceded by the per-title `src_open`/`register_prepare` loop and followed by `wait_db_quiet(3, 30);`). `ids` at this point is declared `char (*ids)[10] = malloc(...)` (a pointer to an array, not an array itself), so `sizeof(ids)` would only be a pointer's size — the buffer below is instead sized from `size`, the per-group title count already in scope a few lines above (bounded by the config's `batch_size`, itself capped at 50 by both `config_load` and `app_config_set`):

```c
        /* sized for the WHOLE group, not for batch_size: with batching switched off,
         * size is `pending` and can reach MAX_TITLES, so a 50-title buffer would truncate
         * mid-title-id and the recovered list would carry a mangled fragment */
        char *doing_tids = malloc((size_t)size * 11 + 1);
        if (doing_tids)
            doing_tids[0] = 0;
        for (int k = 0; k < np && doing_tids; k++)
            snprintf(doing_tids + strlen(doing_tids), (size_t)size * 11 + 1 - strlen(doing_tids), "%s%s", k ? "," : "",
                     ids[k]);
        if (np)
            doing_mark("adding", doing_tids);
        install_all(ids, np);
        if (np)
            doing_clear();
        /* own rows only after ShellCore stopped writing to its databases */
        wait_db_quiet(3, 30);
```

- [ ] **Step 2: On startup, read the marker and hold back whatever it names.** In `main()`, find where `g_retry`/the retry table becomes usable (it is a file-scope static, usable as soon as the function it is declared in is reachable — place this call early in `main()`, after `mkdirs(STATE_DIR, 0777)` so `DOING_FILE`'s directory is guaranteed to exist, and before the first `scan_all()` can run):

```c
    {
        char action[16], tids[DOING_MAX]; /* DOING_MAX = MAX_TITLES * 11 + 32: a whole
                                           * batch's worth of ids, not just batch_size's */
        if (doing_find(action, sizeof(action), tids, sizeof(tids))) {
            if (!strcmp(action, "adding")) {
                for (char *tid = strtok(tids, ","); tid; tid = strtok(NULL, ",")) {
                    for (int i = 0; i < RETRY_MAX_TRIES; i++)
                        retry_failed(&g_retry, tid, time(NULL),
                                      "Held back: the console may have shut down while this was being added "
                                      "last time. Tap Try again once you're ready to retry.");
                    printf("[safety] %s: held back, was being added when the console last stopped responding\n", tid);
                }
                notify("Some games were held back: the console may have shut down while they were being added "
                       "last time. See the panel to try again.");
            }
            doing_clear();
        }
    }
```

(The `for (int i = 0; i < RETRY_MAX_TRIES; i++) retry_failed(...)` loop is the straightforward way to reach the given-up state immediately using the existing counter-based mechanism without adding a second "mark as given up directly" entry point to `safety.c` — `retry_failed` already saturates correctly at `RETRY_MAX_TRIES` and `retry_given_up` reads the count, so three synthetic failures in a row produces exactly the same state a title reaches by actually failing three times. If the 4-arg `retry_failed` from the comfort-and-safety plan has not landed yet, drop the `, "Held back: ..."` reason argument and keep the rest — the hold-back still works, it just will not show a reason until that plan's Task 1-3 land.)

If `tids` is empty (`doing_find` returned 1 but the stored action had an empty list, which should not normally happen since `doing_mark` is only called when `np > 0`), `strtok` on an empty string simply returns `NULL` immediately and the loop body never runs — no special-case needed.

- [ ] **Step 3: Self-review.** `main.c` cannot be compiled on the host; read both edits back carefully:
  - `install_all` always returns to its caller on every path, including its own early `if (!need) return;` (every title in the group is already registered, so there is nothing risky to do) — `doing_clear()` after it is therefore always reached when the console is still alive, with one accepted, harmless case: when `!need` is true, the marker is written and cleared within microseconds around a call that touched nothing risky at all. Note this in the commit message as an accepted minor inefficiency, not a bug.
  - `strlen(doing_tids)` is recomputed every loop iteration (O(n²) in the group size) rather than tracked in a running offset — at a batch-size cap of 50 this is a few hundred byte-compares total, not worth the extra variable.

- [ ] **Step 4: Verify** — `bash test/run.sh` stays green.

- [ ] **Step 5: Commit** — `git add src/main.c && git commit -m "feat(safety): hold back games that were being added when the console last stopped responding"`

---

### Task 3: Bracket the launch call, and warn (not block) on the next boot

**Files:**
- Modify: `src/main.c` (`app_launch`, and the same startup block from Task 2)
- Modify: `src/policy.c`/`src/policy.h` is **not** touched — this uses a dedicated small state file, the same pattern as `EXCLUDED_FILE`, not the retry table (a held-back launch applies to an already-`ready` game, a state the retry table has no notion of).

**Interfaces:**
- Produces:
```c
#define LAUNCH_HELD_FILE STATE_DIR "/launch-held.txt"
```
- `app_games_json` gains a `"launch_held"` boolean per game (alongside the existing `"protected"`-style booleans), read via `kv_get`.

- [ ] **Step 1: Mark and clear around the actual launch call** in `app_launch` (find the exact line with `grep -n "sceSystemServiceLaunchApp" src/main.c`):

```c
    doing_mark("launching", tid);
    int rc = sceSystemServiceLaunchApp(tid, noargs, &ctx);
    doing_clear();
```

- [ ] **Step 2: On the next boot, record the hold instead of auto-retrying anything** (launching is never automatic, so there is nothing to "retry" at startup the way the add-case retries at the next scan — the daemon's only job here is to remember, for when the user next opens the panel). Extend the Task 2 startup block's `if (!strcmp(action, "adding"))` with an `else if`:

```c
            } else if (!strcmp(action, "launching")) {
                kv_set(LAUNCH_HELD_FILE, tids, "1");
                printf("[safety] %s: held, starting it may have caused the console to stop responding last time\n", tids);
            }
```

(`tids` here holds a single title id for the launch case, same variable as the add case reuses — `kv_set`'s key is just that one tid.)

- [ ] **Step 3: `app_launch` checks and clears the hold on its own** — the act of pressing Play again *is* "Try again," no separate button:

```c
int app_launch(const char *tid, char *msg, size_t cap) {
    char app[64];
    snprintf(app, sizeof(app), "/user/app/%s", tid);
    if (!db_is_registered(tid) || !mounted_from(app, NULL, 0)) {
        snprintf(msg, cap, "%s is not ready to play", tid);
        return -1;
    }
    kv_set(LAUNCH_HELD_FILE, tid, NULL); /* pressing Play again is the user's own "try again" */
    ...
```

(Insert this `kv_set(..., NULL)` call — which the existing `kv_set` contract already treats as "remove this key," matching how `EXCLUDED_FILE` is cleared elsewhere — right after the existing readiness check and before the existing `g_install_busy`/`own_game_running` guards further down, so a held game still correctly refuses to launch while busy or while another game is running, exactly as an unheld game would; only the hold itself is unconditionally cleared by the attempt.)

- [ ] **Step 4: Expose the hold to the panel.** In `app_games_json`, alongside the existing per-game fields, add:

```c
            char held[2];
            buf_printf(out, ",\"launch_held\":%s", kv_get(LAUNCH_HELD_FILE, tid, held, sizeof(held)) ? "false" : "true");
```

(`kv_get` returns 0 when the key is found, -1 otherwise — confirmed against its implementation in `src/policy.c` and the established call pattern at `src/main.c:996`, `!kv_get(EXCLUDED_FILE, tid, v, sizeof(v))` — so the ternary above is correct as written: found (0) → `"true"`, not found (-1) → `"false"`.)

- [ ] **Step 5: Panel — a warning line on a held, ready game.** In `web/index.html`'s card-rendering `render()` function, add alongside the existing `update_issue` warning div:

```js
      (g.launch_held ? '<div class="warn">' + t('Starting this last time may have caused the console to stop responding. Press Play to try again.') + '</div>' : '') +
```

Translate ×3:
- es: `'Starting this last time may have caused the console to stop responding. Press Play to try again.'` → `'Iniciar esto la última vez pudo hacer que la consola dejara de responder. Pulsa Jugar para intentarlo de nuevo.'`
- pt: → `'Iniciar isto da última vez pode ter feito o console parar de responder. Aperte Jogar para tentar de novo.'`
- fr: → `'Démarrer ceci la dernière fois a peut-être fait que la console ne réponde plus. Appuyez sur Jouer pour réessayer.'`

- [ ] **Step 6: Self-review** — confirm `kv_set(LAUNCH_HELD_FILE, tid, NULL)` in `app_launch` runs on *every* call, including one that is about to fail the readiness check above it — re-read the ordering: the readiness check (`if (!db_is_registered... return -1;`) happens *before* the clear, so a launch attempt on a game that is not even ready does not touch the hold at all (correct — there is nothing to clear for a game that was never held in the first place, and a not-ready game cannot be the one that was launched last time anyway, since launching requires it to already be ready).

- [ ] **Step 7: Verify** — `bash test/run.sh` fully green including i18n.

- [ ] **Step 8: Commit** — `git add src/main.c web/index.html && git commit -m "feat(safety): warn when starting a game may have caused the console to stop responding"`

---

## Part 2 — Warn on an old copy already running

### Task 4: Notify instead of silently exiting

**Files:**
- Modify: `src/main.c` (`main()`, the `single_instance()` check at the very top — confirmed at the time of writing to be `src/main.c:4098-4101`, but re-confirm with `grep -n "single_instance()" src/main.c` since line numbers drift as other plans land first)

- [ ] **Step 1: Implement:**

```c
int main(void) {
    int lock = single_instance();
    if (lock < 0) {
        printf(VERSION " already running\n");
        notify(VERSION " is already running. Check the autoload list or payload manager for an older copy.");
        return 0;
    }
```

- [ ] **Step 2: Flag the open risk for on-console testing** (do not treat this as done until verified on real hardware): `notify()` is called here *before* `sceUserServiceInitialize(0)` and `sceAppInstUtilInitialize()` run a few lines below. Every other call site of `notify()` in this codebase runs after those two init calls. This plan has no evidence either way whether `sceKernelSendNotificationRequest` (what `notify()` wraps) has any dependency on them — read `notify()`'s body (`grep -n "^static void notify" src/main.c`) to confirm it only calls `sceKernelSendNotificationRequest` directly with no dependency on the two services above, then the first real-console test of this change must specifically confirm the notification actually appears when a second copy is started, not just that the daemon exits cleanly.

- [ ] **Step 3: Verify** — `bash test/run.sh` stays green (collateral-damage guard only; this exact line is not host-testable).

- [ ] **Step 4: Commit** — `git add src/main.c && git commit -m "feat(safety): notify when a second copy of PS4Mount finds one already running"`

---

## Task 5: End-to-end verification and release

- [ ] **Step 1:** `bash test/run.sh` — every suite green, i18n fully translated.
- [ ] **Step 2:** Push, trigger CI, confirm the SDK build compiles (first real compile of all of `main.c`'s new code in this plan).
- [ ] **Step 3:** Cut the next `v0.5.0-test.N` release. On-console tests, in order of how hard they are to verify:
  - Start a second copy of PS4Mount while one is already running (easy): confirm a console notification appears, not just silence.
  - Simulate the crash-recovery path without an actual crash: manually write `/data/ps4mount/doing.txt` with `adding\nCUSA00001\n` via FTP while the daemon is stopped, then start it; confirm `CUSA00001` shows as Failed with the held-back reason, and that `doing.txt` is gone afterward.
  - The same for `launching\nCUSA00001\n`: confirm the ready game's card shows the warning, and that pressing Play both clears the warning and attempts the launch.
  - The real test, which needs a console that actually reproduces one of #16/#13/#10's crashes, is confirming a title held back this way is genuinely skipped on the next boot's scan rather than retried — this cannot be verified without reproducing the original crash, so note in the release notes that this is believed-correct from the mechanism's design but not yet confirmed against a real crash in the wild.

## Out of scope (next up, not this plan)

Detecting a title already mounted by another tool (ShadowMountPlus or similar) — roadmap item #13 — needs its own design pass extending `mounted_from()`/`HOLD_DIR` comparison into `policy_decide`'s skip reasons. Not started here.

---

## Post-implementation corrections (found by a 4-lens adversarial review)

The plan as originally written had two defects that only surfaced once the code existed
and four independent reviewers read it. Both are corrected in the task text above; they are
recorded here so the reasoning is not lost:

1. **The marker was last-writer-wins, and the daemon is re-entrant.** `install_all`'s wait
   loop calls `pump_events()`, which serves the web panel on the same thread. A panel
   "Play" arriving mid-batch therefore ran `doing_mark("launching", ...)` followed by
   `doing_clear()`, erasing the live "adding" marker — so a crash during registration left
   nothing to recover from, defeating the feature in exactly the case it exists for. The
   marker now tracks which action owns it (`g_doing`): `doing_mark` refuses to overwrite a
   different action's marker, and `doing_clear(action)` only clears its own. A `g_adding`
   flag additionally keeps `app_launch` out of the window that `g_install_busy` does not
   cover (it is cleared the moment the install call returns, before the batch is done).

2. **`doing_tids` was sized from `batch_size`, which is not the group size.** With batching
   switched off, the group is `pending` — up to `MAX_TITLES` — so a 50-title buffer
   truncated mid-title-id, and the recovered list ended in a fragment like `CUS` that
   became a bogus retry row while the real titles went un-held. The buffer is now allocated
   for the actual group size, the shared `DOING_MAX` covers the file and parse buffers, and
   the recovery loop skips any token that is not exactly 9 characters.
