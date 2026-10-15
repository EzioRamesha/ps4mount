# File Manager Phase 2 — a real file manager behind the Files button

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the cramped Files dialog with a real file manager — full-screen, with a toolbar, sortable columns, multi-select, bulk actions, and copy / cross-drive move running as a cancellable background task with progress — without putting the game library, the scan loop, or registration at risk.

**Architecture:** A new module `src/fmgr.[ch]` owns everything the file manager does that is not a pure path rule: the background task state machine and the copy engine. Pure decisions (sorting, conflict resolution, name generation) go into the existing host-tested `src/fm.c` so they get real tests. The file manager's contract with the rest of PS4Mount is one-directional: it READS the catalog for display and protection, and it REQUESTS a rescan after a change through the existing coalesced path. It never writes a catalog, never registers or mounts a title, never touches the retry table. Long operations run on one worker thread, exactly as `install_thread` and `speed_thread` already do; the daemon's main loop only polls a task struct. The panel's Files view becomes a full-screen view that replaces the game grid rather than a `<dialog>`, which removes the modal-on-modal problems the current one has and makes room for a toolbar.

**Tech Stack:** C (no new dependencies — pthreads are already used in this file), vanilla JS in the single embedded `web/index.html`, existing test harness.

## Global Constraints

- Every new source file starts with `/* SPDX-License-Identifier: GPL-3.0-or-later` and ` * Copyright (C) 2026 gasb07`.
- **Isolation is the point of this plan.** The file manager must not write to `g_cat`, must not call registration/mount/unmount, must not touch `g_retry`, and must not add shared mutable state to the scan or klog paths. Its only outward effects are: the filesystem operations the user asked for, and `fm_queue_scan()` to request a rescan.
- `src/fm.c` stays system-call-free so it remains host-tested. The task machinery and anything touching the filesystem lives in `src/fmgr.c` or `src/main.c`.
- One background task at a time, daemon-wide. A second request while one runs is refused with a clear message (this matches the reference implementation and suits a single-threaded daemon).
- Never destroy data on a cancel: a cancelled single-file copy removes its partial temp file; a cancelled folder copy leaves what it already wrote (deleting it could remove files that were already in the destination).
- A copy or move never overwrites without an explicit decision from the user (overwrite / skip / keep both).
- Comments and user-facing text in plain, simple English; every new panel string added to the `I18N` object for **es, pt, fr** or `test/i18n_check.js` fails.
- Conventional commits, one purpose per commit.

## Credit

This design follows the UI and task model of **ps5-web-file-manager** by owendswang (<https://github.com/owendswang/ps5-web-file-manager>), which is GPL-3.0 like PS4Mount. No code is copied verbatim by this plan — the implementation is written against PS4Mount's own HTTP server and panel — but the toolbar/columns/multi-select/task-overlay shape and the "choose sources, then paste into the current folder" interaction are taken from it. **Task 10 adds the credit to README.md; that task is not optional.**

## What is deliberately NOT in this plan

- **Download / upload**: PS4Mount's HTTP server cannot stream — `reply()` takes a complete in-memory body and the log download caps at 4 MB by buffering. Serving a 50 GB pkg needs new streaming support in `src/web.c`, and upload needs multipart parsing. Both are real features but they are their own project; a later phase.
- **Archive extraction**: the reference needs a separately distributed helper ELF (`wfm-7zip-helper`) for this.
- **Text editor, chmod, SMB, launching .elf files**: no use for managing pkgs (and exFAT ignores permissions anyway, as the reference's own README notes).

---

## Part 1 — Pure logic, host-tested (`src/fm.c`)

### Task 1: Sorting and conflict rules

**Files:**
- Modify: `src/fm.h`, `src/fm.c`
- Test: `test/unit.c` (extend the existing `test_fm` — find it with `grep -n "file manager path rules" test/unit.c`)

**Interfaces:**
- Produces:
```c
/* How a listing is ordered. Folders always come before files whatever the key is. */
enum { FM_SORT_NAME = 0, FM_SORT_SIZE = 1, FM_SORT_TIME = 2 };

/* One row of a listing, as far as sorting cares about it. */
typedef struct {
    const char *name;
    int dir;
    unsigned long long size;
    long long mtime;
} fm_row_t;

/* Orders a listing: folders first, then by key (name is case-insensitive; size and time
 * are largest/newest first when desc is 1). Returns nothing; sorts in place. */
void fm_sort(fm_row_t *rows, int n, int key, int desc);

/* Builds the name a "keep both" copy should get: "x.pkg" -> "x (2).pkg", "x (2).pkg" ->
 * "x (3).pkg", a name with no dot gets the number before the end. out must hold at least
 * FM_NAME_MAX bytes. Returns 0, or -1 when no free name fits the buffer. */
#define FM_NAME_MAX 256
int fm_keep_both_name(const char *name, int attempt, char *out, size_t cap);
```

- [ ] **Step 1: Write the failing tests** — append to `test/unit.c`'s `test_fm`:

```c
    printf("file manager sorting and names\n");
    fm_row_t rows[5] = {
        {"beta.pkg", 0, 300, 100},
        {"Alpha", 1, 0, 50},
        {"gamma.pkg", 0, 100, 300},
        {"delta", 1, 0, 200},
        {"Epsilon.pkg", 0, 200, 150},
    };
    fm_sort(rows, 5, FM_SORT_NAME, 0);
    CHECK(rows[0].dir && rows[1].dir);                       /* folders first */
    CHECK(!strcmp(rows[0].name, "Alpha") && !strcmp(rows[1].name, "delta")); /* case-insensitive */
    CHECK(!strcmp(rows[2].name, "beta.pkg") && !strcmp(rows[4].name, "gamma.pkg"));
    fm_sort(rows, 5, FM_SORT_SIZE, 1);
    CHECK(rows[0].dir && rows[1].dir);                       /* folders still first */
    CHECK(rows[2].size == 300 && rows[4].size == 100);       /* biggest file first */
    fm_sort(rows, 5, FM_SORT_TIME, 1);
    CHECK(rows[2].mtime == 300 && rows[4].mtime == 100);     /* newest file first */

    char kb[FM_NAME_MAX];
    CHECK(!fm_keep_both_name("x.pkg", 2, kb, sizeof(kb)) && !strcmp(kb, "x (2).pkg"));
    CHECK(!fm_keep_both_name("x (2).pkg", 3, kb, sizeof(kb)) && !strcmp(kb, "x (3).pkg"));
    CHECK(!fm_keep_both_name("noext", 2, kb, sizeof(kb)) && !strcmp(kb, "noext (2)"));
    CHECK(!fm_keep_both_name("a.tar.gz", 2, kb, sizeof(kb)) && !strcmp(kb, "a.tar (2).gz")); /* last dot wins */
    char tiny[8];
    CHECK(fm_keep_both_name("averylongname.pkg", 2, tiny, sizeof(tiny)) == -1); /* no room */
```

- [ ] **Step 2: Run to verify failure** — `bash test/run.sh`; expected: compile error (`fm_sort` / `fm_row_t` undeclared).

- [ ] **Step 3: Implement** in `src/fm.h` and `src/fm.c`. `fm_sort` may use `qsort` from `<stdlib.h>` (that is not a system call; `fm.c` stays host-testable). Case-insensitive compare without `strcasecmp` if the host lacks it: compare with a small `tolower` loop. For `fm_keep_both_name`, split at the LAST '.' that is not the first character, insert `" (N)"` before it, and return -1 if the result does not fit `cap`.

- [ ] **Step 4: Run to verify pass** — `bash test/run.sh` green, unit count up.

- [ ] **Step 5: Commit** — `git commit -m "feat(fm): sorting and keep-both naming, host-tested"`

---

## Part 2 — The background task engine (`src/fmgr.[ch]`)

### Task 2: The task state machine

**Files:**
- Create: `src/fmgr.h`, `src/fmgr.c`
- Modify: `build.sh` (add `src/fmgr.c` to the payload compile line), `test/run.sh` (add it to the `unit` line only if you add host tests; the copy engine itself is not host-testable)

**Interfaces:**
- Produces:
```c
/* What a background file operation is doing. One task runs at a time, daemon-wide. */
enum { FMGR_IDLE = 0, FMGR_RUNNING, FMGR_DONE, FMGR_FAILED, FMGR_CANCELLED };
/* What to do when the destination already has an item with that name. */
enum { FMGR_ASK = 0, FMGR_OVERWRITE, FMGR_SKIP, FMGR_KEEP_BOTH };

typedef struct {
    int state;                 /* FMGR_* above */
    int copy;                  /* 1 copy, 0 move */
    int conflict;              /* FMGR_* policy chosen for this task */
    char what[256];            /* the item being worked on right now, for the panel */
    int done_items, total_items;
    unsigned long long done_bytes, total_bytes;
    unsigned long long bytes_per_sec;
    long eta_seconds;
    char msg[256];             /* why it failed, or what it finished */
} fmgr_status_t;

#define FMGR_MAX_SRCS 256
/* Starts a copy (copy=1) or move (copy=0) of n srcs into dst. dst_free is the destination
 * drive's free bytes, measured by the caller (fmgr.c deliberately knows nothing about
 * drives or mounts). Returns 0 when the task started, -1 when one is already running or
 * the request is refused (msg says why). */
int fmgr_start(char (*srcs)[1024], int n, const char *dst, int copy, int conflict,
               unsigned long long dst_free, char *msg, size_t cap);
/* Copies the current status for the panel. Safe to call from the main loop at any time. */
void fmgr_status(fmgr_status_t *out);
/* Asks the running task to stop. It stops at the next file boundary. */
void fmgr_cancel(void);
/* 1 while a task is running: the daemon refuses other file changes meanwhile. */
int fmgr_busy(void);
```

- [ ] **Step 1: Implement the state and the thread shell.** A file-scope `static fmgr_status_t g_task;` plus `static pthread_mutex_t g_lock;` and a `static volatile int g_cancel;`. `fmgr_start` refuses when `fmgr_busy()`, otherwise fills the task, sets `FMGR_RUNNING`, and `pthread_create`s a worker exactly like `install_all_start` does in `main.c` (including the "no thread: run it inline" fallback that function uses — read it first and match that shape). `fmgr_status` takes the lock and copies the struct out. Every write to `g_task` inside the worker takes the lock; the main loop only ever reads through `fmgr_status`.

- [ ] **Step 2: Implement the copy engine.** Walk each source; for a folder, recurse. For each file:
  - Check `g_cancel` at the top of every file AND every 4 MB block inside a big file, so a cancel on a 50 GB pkg does not wait for the whole file.
  - Copy through a temp name in the destination folder (`.<name>.part`), `fsync` it, then `rename` into place. A cancelled or failed single file removes its temp; a partly-done FOLDER is left alone (deleting it could remove files that were already there before this task).
  - For a MOVE on the same drive, use `rename()` directly (instant) rather than copy-then-delete. Only fall back to copy-then-delete when `fm_drive_of(src) != fm_drive_of(dst)` (`rename` cannot cross filesystems).
  - Update `done_bytes`/`bytes_per_sec`/`eta_seconds` under the lock as you go, but not more than about once a second — taking the lock per 4 MB block is fine, recomputing the ETA per block is wasteful.
  - On a name clash apply the task's `conflict` policy: `FMGR_SKIP` skips, `FMGR_OVERWRITE` replaces, `FMGR_KEEP_BOTH` uses `fm_keep_both_name` (Task 1) trying attempts 2,3,4… until a free name is found. `FMGR_ASK` must never reach the worker — the panel resolves it before starting (Task 8); if it does, treat it as `FMGR_SKIP` and say so in `msg`.
- [ ] **Step 3: Pre-flight checks in `fmgr_start`** — before starting: every src must pass `fm_path_ok`; dst must be an existing folder; no src may BE the dst or contain it (`fm_covers`); and the destination drive must have room.
  For the space check, `fmgr_start` takes the destination's free bytes as a parameter rather than calling `drive_space` itself — `drive_space` is `static` in `main.c`, and keeping `fmgr.c` free of drive/mount knowledge is what keeps this module isolated. So the signature is:
  ```c
  int fmgr_start(char (*srcs)[1024], int n, const char *dst, int copy, int conflict,
                 unsigned long long dst_free, char *msg, size_t cap);
  ```
  `main.c` computes `dst_free` with its own `drive_space` before calling. Cap `n` at `FMGR_MAX_SRCS` (256 — more than anyone selects by hand, and it bounds the 1024-byte-per-path array at 256 KB); refuse a larger request with a clear message rather than truncating the list.

- [ ] **Step 4: Verify** — `bash test/run.sh` stays green. `src/fmgr.c` is console-only in practice; self-review the thread safety carefully: every `g_task` access under the lock, `g_cancel` only ever set by `fmgr_cancel` and read by the worker.

- [ ] **Step 5: Commit** — `git commit -m "feat(fmgr): background copy and move with progress and cancel"`

---

### Task 3: Wire the task into the daemon, and keep it isolated

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Produces: `app_fm_task_json(buf_t *out)`, `app_fm_copy(...)`, `app_fm_cancel(...)` — declared in `src/web.h` in Task 4.

- [ ] **Step 1: Refuse conflicting work while a task runs.** In `app_fm_rename`, `app_fm_delete`, `app_fm_move` and `app_fm_mkdir`, return -1 with "A file operation is already running; wait for it to finish" when `fmgr_busy()`. Also refuse to START a task when `g_install_busy || g_adding` (games being added), reusing the wording already in those functions.
- [ ] **Step 2: Isolation check — do NOT do any of these.** The task must not call `catalog_*` write functions, must not touch `g_cat` except through the existing read-only protection helpers, must not call registration/mount/unmount, and must not touch `g_retry`. After a task finishes, call the existing `fm_queue_scan()` (the coalesced rescan) and nothing else. Write a one-line comment at the call site saying why only that.
- [ ] **Step 3: Protection still applies.** Before starting a task, every source must pass the same `fm_protected()` check the single-file operations use, and the destination folder must not be inside a protected game's folder. Refuse with the existing "Used by ..." wording.
- [ ] **Step 4: Commit** — `git commit -m "feat(fm): start, report and cancel file tasks from the panel"`

---

### Task 4: The HTTP endpoints

**Files:**
- Modify: `src/web.h`, `src/web.c`
- Test: `test/web_unit.c` (stubs + endpoint tests, following the existing `test_fm_api` pattern)

**Interfaces:**
```c
/* {"state":"idle|running|done|failed|cancelled","what":"...","done_items":N,"total_items":N,
 *  "done_bytes":N,"total_bytes":N,"speed":N,"eta":N,"msg":"..."} */
void app_fm_task_json(buf_t *out);
/* srcs is one or more paths separated by '\n'. copy: 1 copy, 0 move. 0 when started. */
int app_fm_copy(const char *srcs, const char *dst, int copy, int conflict, char *msg, size_t cap);
int app_fm_cancel(char *msg, size_t cap);
```

- [ ] **Step 1: Write the failing endpoint tests** in `test/web_unit.c`: `GET /api/fm/task` returns the stub JSON; `POST /api/fm/copy?dst=...&copy=1&conflict=0` with a body of newline-separated sources echoes them; an invalid dst gives "invalid path"; `POST /api/fm/cancel` reaches the stub; `GET` on a POST-only route 404s.
- [ ] **Step 2: Add the routes** in `src/web.c` next to the existing `/api/fm/*` ones, matching their exact validation style (`fm_param` for dst, `fm_path_ok`, POST-only for mutations). The source list comes in the request BODY (not the query) because many long paths will not fit a query string — read how `/api/config` and `/api/restore` already read `body`, and follow that. Validate EVERY source path with `fm_path_ok` before passing them on; refuse the whole request if any one fails.
- [ ] **Step 3: Verify** — `bash test/run.sh` green with the new web checks.
- [ ] **Step 4: Commit** — `git commit -m "feat(fm): endpoints for copy, move, task status and cancel"`

---

## Part 3 — The panel: a real file manager

### Task 5: The full-screen view and its toolbar

**Files:**
- Modify: `web/index.html`

**Context the implementer needs:** the current Files UI is `<dialog id="fmdlg">` plus `<dialog id="fmsheet">`, with JS functions `fmPkgName`, `fmOpen`, `fmRender`, `fmSheet` and the handlers below them (find them with `grep -n "^function fm\|filesbtn" web/index.html`). This task REPLACES the dialog with a full-screen view. Keep `fmPkgName` (the pkg display-name fallback) — it is still correct and still needed.

- [ ] **Step 1: Markup.** Add a `<div class="fmview" id="fmview" hidden>` as a sibling of `<main>` (not inside it), holding: a toolbar row (Back to library · Up · Refresh · New folder · Sort control · Select all · and the bulk action buttons, initially hidden) · the breadcrumbs · a column header row (Name · Size · Modified, each clickable to sort) · the list · and a task overlay (Task 9). When the view is shown, hide `<main>`; when hidden, show it again. Delete the `#fmdlg`/`#fmsheet` markup and the old handlers in the same commit so there is never a half-migrated state.
- [ ] **Step 2: CSS.** Full-width rows with columns that collapse to name-only on a narrow screen (the existing panel already uses this pattern for the game grid — match it). Keep the existing `.fmrow`/`.fmicon` look where it still fits. Touch targets at least the 40 px the panel standardised on.
- [ ] **Step 3: Open/close.** `#filesbtn` shows the view and calls `fmOpen('')` (the drive list root, unchanged). "Back to library" hides it. Keep the existing behaviour that the root needs no API call.
- [ ] **Step 4: Up button.** Navigates to the parent of `fm.path`; at a drive root it goes to the drive list; hidden/disabled at the drive list itself.
- [ ] **Step 5: Verify visually** with the mock server (`scratchpad/mock_panel.py`, route `/fmtest`) and headless Chrome at 1280px AND inside a 390px iframe (headless Chrome clamps windows to 500px, so a direct narrow `--window-size` screenshot lies — use an iframe wrapper page). Read the PNGs yourself.
- [ ] **Step 6: i18n + verify + commit** — every new string in es/pt/fr; `bash test/run.sh` green including i18n.

---

### Task 6: Sortable columns, remembered

**Files:** `web/index.html`

- [ ] **Step 1:** Clicking a column header sorts by it; clicking the active one flips direction; an arrow marks the active column and direction. Folders always stay first (that is `fm_sort`'s rule on the daemon side — the panel must match it, since the panel sorts the rows it already has rather than re-fetching).
- [ ] **Step 2:** Persist the choice in `localStorage` under a `ps4mount-fm-sort` key (the panel already uses `localStorage` for the PIN and language — follow that style, including the try/catch those use, since the PS5 browser can have storage disabled).
- [ ] **Step 3:** Default: name ascending, folders first — the current behaviour, so nothing changes for someone who never touches it.
- [ ] **Step 4:** i18n, verify, commit.

---

### Task 7: Multi-select and bulk actions

**Files:** `web/index.html`

- [ ] **Step 1:** A checkbox per row plus a select-all in the header. Selection lives in `fm.sel` (already exists, currently unused). Clicking a row's NAME still navigates/opens; only the checkbox selects — do not make the whole row toggle selection, or navigating becomes impossible.
- [ ] **Step 2:** A selection bar appears when anything is selected, showing the count and total size, with buttons: Copy · Move · Delete · Clear. Model it on the existing `#selbar` for games (same CSS class, same sticky-bottom behaviour).
- [ ] **Step 3:** Selection clears on navigation (a selection spanning folders cannot be acted on coherently) and when the view closes.
- [ ] **Step 4:** Bulk delete sends one request per item (the daemon's delete is per-path and already fast, being a rename into `.trash`), shows progress as "n of m", and refreshes once at the end — follow the existing bulk pattern in the game grid's `data-bulk` handler, which already does one-at-a-time with a counter.
- [ ] **Step 5:** i18n, verify (screenshots at both widths), commit.

---

### Task 8: Copy / move with the clipboard model and conflict prompts

**Files:** `web/index.html`

- [ ] **Step 1: Clipboard model** (this is the reference's interaction, and it replaces the current one-file "Move here" bar): selecting items and pressing **Copy** or **Move** puts them on a clipboard shown in a bar ("3 items ready to copy — open a folder and press Paste"); navigating anywhere and pressing **Paste** starts the task into the current folder. Cancel clears the clipboard. The clipboard must clear when the view closes (the current code had a bug where a pending move survived a close — do not repeat it).
- [ ] **Step 2: Conflict prompt, asked once up front.** When the user presses Paste, the panel asks how to handle name clashes — "If something already exists: Overwrite / Skip / Keep both" — and sends that single choice as the task's `conflict` policy. The dialog's wording must say the choice applies to every item in the operation, so nobody expects to be asked again mid-copy.
  Per-file prompting during a running task is deliberately NOT in this phase: it needs the worker to pause and wait for a round trip through the panel, which is a different and much more intricate design. It is listed in "Out of scope" at the end of this plan.
- [ ] **Step 3:** Paste is disabled when the clipboard is empty, when the destination is the drive-list root, and when a task is already running.
- [ ] **Step 4:** i18n, verify, commit.

---

### Task 9: The task overlay

**Files:** `web/index.html`

- [ ] **Step 1:** Poll `GET /api/fm/task` every second while a task is running (and ONCE when the Files view opens, so a task started before a browser reload is picked up again — the reference does this and it matters, because a 40 GB copy outlives a page refresh).
- [ ] **Step 2:** Show an overlay with: what is being copied right now, "n of m items", a progress bar from `done_bytes`/`total_bytes`, speed in MB/s, ETA, and a Cancel button. Reuse `gb()` for sizes.
- [ ] **Step 3:** On `done`/`failed`/`cancelled`, show the message in the view's existing message area (green for done, amber for failed/cancelled — the panel already has `.ok`/`.warn` for this), refresh the listing, and stop polling.
- [ ] **Step 4:** While a task runs, disable the actions that the daemon will refuse anyway (new folder, rename, delete, paste) so the user is not offered something that cannot work.
- [ ] **Step 5:** i18n, verify (screenshot a running task against the mock), commit.

---

## Task 10: Docs, credit, verification and release

- [ ] **Step 1:** `bash test/run.sh` fully green including i18n.
- [ ] **Step 2:** Push, trigger CI (`gh workflow run "Build and release" -R EzioRamesha/ps4mount --ref pkgs-in-place`), confirm the SDK build compiles `src/fmgr.c` and everything else — this is the first real compile of the new module.
- [ ] **Step 3:** README: rewrite the **Files** section for the new file manager (full-screen view, sorting, multi-select, copy/move with progress and cancel, what it still will not do). Keep the existing Trash paragraph.
- [ ] **Step 4: Credit (required).** Add ps5-web-file-manager by owendswang to README.md's Credits section, noting the UI and task model were modelled on it and that it is GPL-3.0, the same licence as PS4Mount. CLAUDE.md requires this for anything borrowed from another project.
- [ ] **Step 5:** Add `src/fmgr.[ch]` to CLAUDE.md's architecture module list.
- [ ] **Step 6:** Cut the next `v0.5.1-test.N` release. On-console tests, in order of risk:
  - Open Files: the drive list, then a folder; sort by size and by date; confirm the choice survives a reload.
  - Select several pkgs; check the count and total size; Clear.
  - Copy ONE small file to a folder on the SAME drive: watch the overlay, confirm the file arrives and the progress/speed/ETA look sane.
  - Copy a LARGE pkg to the OTHER drive: confirm speed and ETA are believable, then press Cancel mid-way and confirm no partial file is left behind.
  - Start a big copy, reload the browser, reopen Files: the overlay must come back with the task still running.
  - Try to rename/delete a pkg a library game uses: must still be refused, visibly.
  - Confirm the game library still scans and plays normally throughout — that is the isolation requirement, and it is the thing to check hardest.

## Out of scope (later phases)

Download (needs HTTP streaming in `src/web.c`), upload with drag-and-drop (needs multipart parsing), archive extraction (needs a helper ELF), per-file conflict prompts during a running task, text editing, chmod, SMB.
