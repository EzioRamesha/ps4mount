# Add-ons Installed to Internal Storage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Install every add-on of a library game into `/user/addcont` with `sceAppInstUtilInstallByPackage`, so the system writes the addcont.db row, with a Settings option to keep the old mount mode.

**Architecture:** A new host-testable module `src/accopy.c` holds the state files (`.accopy`, `.acfail`), the space and progress rules and the per-add-on decision. `src/main.c` calls the system (install, uninstall, statfs, mounts) and runs one reconcile pass per scan after registration. The panel reads a per-add-on status from `/api/game`.

**Tech Stack:** C (PS5 Payload SDK, libSceAppInstUtil), host tests in `test/run.sh`, panel tests in `test/web` (jsdom).

**Spec:** `docs/superpowers/specs/2026-10-23-dlc-internal-install-design.md`

## Global Constraints

- Never write addcont.db or app.db. Only the system writes them.
- Never uninstall an add-on without a `STATE_DIR/<CONTENT ID>.accopy` file.
- In copy mode, never call `place_dlc`/`drop_dlc` and never mount `/user/addcont/<ADD-ON TID>`.
- The game base and update stay mounted from the drive.
- Free space: install only when `free > pkg_size + 1 GiB` on `/user`.
- Progress poll every 2 s. Timeout 30 min. Stall (no growth) 2 min. One install at a time.
- 3 failures: no more installs, panel shows a problem with the last reason. Off then on clears the count. Success deletes `.acfail`.
- User delete from the system UI: content ID goes into `<GAME TID>.dlcoff`, `.accopy` removed.
- Config key `dlc_copy`, default 1.
- Every panel change on `web/index.html` and `web/legacy.html` (ES5), tested on both.
- English only. One commit per task, generic messages, no trailers.

## Review Focus

1. Drive unplugged during a copy: the install must count as one failure, not hang the scan thread. Test: `test_progress_stall` in Task 1.
2. Store add-on in the same title folder as ours: never uninstalled or overwritten. Test: `test_state_store` in Task 1.
3. A game registered again after its add-ons were copied: the copy whose PlayGo data the system removed is installed again, not marked as deleted by the user. Test: `test_state_playgo_lost` in Task 1. Confirm on the console in Task 7.
4. Mode switched back to mount while a copy runs: the uninstall waits for the copy to end. Checked on the console in Task 7.
5. Unlock-only add-ons (no data): installed by copy too. Checked on the console in Task 7.

---

### Task 1: State and decision module

**Files:**
- Create: `src/accopy.h`, `src/accopy.c`
- Create: `test/accopy_test.c`
- Modify: `test/run.sh` (new `== add-on copies` block, same style as `== log with times`)
- Modify: `build.sh` (add `src/accopy.c` to the source list)

**Interfaces:**
- Produces (all paths under the `dir` argument, which is `STATE_DIR` in main.c and a temp folder in tests):
  - `int accopy_mark(const char *dir, const char *cid, const char *pkg_path);` writes `<cid>.accopy`, 0 on success.
  - `int accopy_marked(const char *dir, const char *cid);` 1 when the file exists.
  - `void accopy_unmark(const char *dir, const char *cid);`
  - `int acfail_add(const char *dir, const char *cid, const char *reason);` appends one failure, returns the new count.
  - `int acfail_get(const char *dir, const char *cid, char *reason, size_t cap);` count (0 without file) and last reason.
  - `void acfail_clear(const char *dir, const char *cid);`
  - `#define ACFAIL_MAX 3`
  - `int accopy_space_ok(uint64_t free_bytes, uint64_t pkg_size);`
  - `typedef struct { uint64_t last; time_t start, grew; } acwait_t;`
  - `void acwait_start(acwait_t *w, time_t now);`
  - `int acwait_step(acwait_t *w, uint64_t cur, uint64_t want, time_t now);` returns `ACW_DONE`, `ACW_WAIT`, `ACW_TIMEOUT` (1800 s from start) or `ACW_STALL` (120 s without growth).
  - `typedef struct { int on, marked, files, store, playgo, fails, busy; } acin_t;`. `files`: `/user/addcont/<TID>/<LABEL>/ac.pkg` exists. `store`: files present and not marked. `playgo`: `/user/playgo/addcont/<CID>` exists. `busy`: this add-on is being installed now.
  - `int accopy_decide(const acin_t *in);` returns one of `AC_NONE, AC_INSTALL, AC_UNINSTALL, AC_TURN_OFF, AC_REINSTALL, AC_PROBLEM, AC_STORE`.

Decision table (exact):

| on | store | marked | files | playgo | fails | result |
|----|-------|--------|-------|--------|-------|--------|
| any | 1 | - | - | - | - | AC_STORE |
| 1 | 0 | 0 | 0 | - | >=3 | AC_PROBLEM |
| 1 | 0 | 0 | 0 | - | <3 | AC_INSTALL |
| 1 | 0 | 1 | 1 | 1 | - | AC_NONE |
| 1 | 0 | 1 | 1 | 0 | - | AC_REINSTALL |
| 1 | 0 | 1 | 0 | - | - | AC_TURN_OFF |
| 0 | 0 | 1 | - | - | - | AC_UNINSTALL |
| 0 | 0 | 0 | - | - | - | AC_NONE |

`busy` = 1 always gives AC_NONE.

- [ ] **Step 1: Write the failing tests in `test/accopy_test.c`** (same `CHECK` macro as `test/unit.c`, a `mkdtemp` folder as `dir`):
  - `test_mark`: mark, `accopy_marked == 1`, unmark, `== 0`.
  - `test_fail_count`: three `acfail_add` return 1, 2, 3; `acfail_get` returns 3 and the last reason; `acfail_clear` gives 0.
  - `test_space`: `accopy_space_ok(2ULL<<30, 1ULL<<30) == 0` (equal is not enough), `accopy_space_ok((2ULL<<30)+1, 1ULL<<30) == 1`.
  - `test_progress_done`: cur == want gives ACW_DONE.
  - `test_progress_stall`: same cur at t=0 and t=121 gives ACW_STALL; growth at t=100 then same cur at t=200 gives ACW_WAIT.
  - `test_progress_timeout`: growing every step, t=1801 gives ACW_TIMEOUT.
  - `test_state_store`: `{on=0, store=1}` and `{on=1, store=1}` give AC_STORE.
  - `test_state_playgo_lost`: `{on=1, marked=1, files=1, playgo=0}` gives AC_REINSTALL.
  - `test_state_table`: every other row of the table above, plus `busy=1` giving AC_NONE.
- [ ] **Step 2:** `bash test/run.sh`. Expected: build error, `accopy.h` not found.
- [ ] **Step 3:** Implement `src/accopy.c` to the interfaces above. `.acfail` format: one line per failure, the reason text; count = lines.
- [ ] **Step 4:** `bash test/run.sh` and `SANITIZE=1 bash test/run.sh`. Expected: all blocks pass, no `FAIL` lines.
- [ ] **Step 5:** Commit `Add state files and rules for add-on copies`.

### Task 2: dlc_copy setting

**Files:**
- Modify: `src/main.c` (`config_t`, `g_cfg` default, config read/write/print near line 3288-3450)
- Modify: `web/index.html`, `web/legacy.html` (checkbox `s-dlccopy` under `s-dlc`, label `Copy add-ons to internal storage`, sent as `&dlc_copy=0|1`)
- Modify: `test/web/panel.test.js` (`CFG.dlc_copy: true`)

**Interfaces:**
- Produces: `g_cfg.dlc_copy` (int, 1 = copy mode). `/api/config` JSON field `dlc_copy` (bool).

- [ ] **Step 1:** In `panel.test.js`, for both pages: settings view shows `s-dlccopy` checked from `CFG`; unchecking it and saving sends a body containing `dlc_copy=0`.
- [ ] **Step 2:** `cd test/web && npm test`. Expected: the new checks FAIL.
- [ ] **Step 3:** Add the field, default 1 in `g_cfg`, `dlc_copy=%d` line in the config file, JSON field, and the form field on both pages. A missing key in an old config file reads as 1.
- [ ] **Step 4:** `npm test` and `bash test/run.sh`. Expected: all pass.
- [ ] **Step 5:** Commit `Add a setting to copy add-ons to internal storage`.

### Task 3: Install and uninstall calls, TEMP code removed

**Files:**
- Modify: `src/main.c` (replace the TEMP block at about 2560-2660), `src/web.c`, `src/web.h`, `build.sh`, `web/index.html`
- Delete: `web/closetest.html`

**Interfaces:**
- Produces in `main.c`:
  - `static int ac_install(const dlc_src_t *a, const char *game_tid, char *why, size_t cap);` statfs space check, InstallByPackage with the TEMP structs (renamed `ac_meta_t`, `ac_pkg_t`, `ac_playgo_t`; MetaInfo zeroed, `uri` = `a->path`, `content_name` = `"<GAME TID> (Add-on)"`), then `acwait` poll every 2 s on `/user/addcont/<TID>/<LABEL>/ac.pkg` against `a->pkg.pkg_size`. 0 on ACW_DONE, -1 with `why` set otherwise (`"not enough free space"`, `"install call 0x%08x"`, `"copy timed out"`, `"copy stopped"`).
  - `static int ac_uninstall(const char *dtid, const char *cid);` label first, content ID second; returns 0 when either returns 0 or both return a not-found code (log the codes; the not-found value is the one seen in Task 7, keep 0x80A30004 as the first guess).
- Removes: `app_actest`, `app_acdel`, `/api/actest`, `/api/acdel`, `/closetest`, `/api/closetest`, the closetest button in `index.html`, the closetest line in `build.sh`.

- [ ] **Step 1:** `grep -n "actest\|acdel\|closetest" -r src web build.sh test`. Expected after the step: no output.
- [ ] **Step 2:** `bash build.sh` and `bash test/run.sh`. Expected: build succeeds, tests pass.
- [ ] **Step 3:** Commit `Install and uninstall add-ons through the system`.

### Task 4: Reconcile pass, mount guard and migration

**Files:**
- Modify: `src/main.c` (`mount_paths`, `place_files`, the scan loop that calls registration, `apply_dlc_choice`)

**Interfaces:**
- Consumes: Task 1 module, `ac_install`, `ac_uninstall`, `g_cfg.dlc_copy`.
- Produces: `static void ac_reconcile(title_src_t *t);` called once per scan for each library game, after that game's registration has finished.

Rules:
1. `place_files`: when `g_cfg.dlc_copy`, skip the `place_dlc`/`drop_dlc` loops.
2. `mount_paths`: when `g_cfg.dlc_copy`, skip the `/user/addcont` loop.
3. `ac_reconcile`, per add-on title folder first: if `/user/addcont/<TID>` is nullfs from `HOLD_DIR/addcont/<TID>`, unmount it (`unmount_all_layers(target, 1)`) and `drop_dlc` every add-on of that title. A mount from anywhere else: log `[accopy] /user/addcont/<TID> mounted from <from>, add-ons not copied` and skip the title.
4. Then per add-on, fill `acin_t` and act on `accopy_decide`: AC_INSTALL/AC_REINSTALL call `ac_install` (success: `accopy_mark`, `acfail_clear`; failure: `acfail_add`, `notify("%s: add-on %s not installed (%s)")`). AC_UNINSTALL calls `ac_uninstall` then `accopy_unmark`. AC_TURN_OFF appends the content ID to `<GAME TID>.dlcoff` and calls `accopy_unmark`. AC_REINSTALL first calls `ac_uninstall`.
5. Only one add-on is installed per `ac_reconcile` call and per scan, so the scan thread returns between copies. `busy` is set for that add-on while it runs.
6. `apply_dlc_choice`: for each add-on that goes from off to on, `acfail_clear`.

- [ ] **Step 1:** `bash build.sh` and `bash test/run.sh`. Expected: build succeeds, tests pass.
- [ ] **Step 2:** Commit `Copy add-ons to internal storage on each scan`.

### Task 5: Mode switch and game removal

**Files:**
- Modify: `src/main.c` (config save path, `[PlayGoCore][Uninstall]` handler near line 2208, panel remove path near line 2787)

**Interfaces:**
- Produces: `static void ac_uninstall_game(const char *game_tid, char (*dtids)[10], int n);` uninstalls every add-on with `.accopy` whose content ID title matches one of `dtids`.

Rules:
1. `dlc_copy` 1 to 0 in the config save: wait while an install is running, then for every library game call `ac_uninstall_game`, then trigger the same re-registration `apply_dlc_choice` triggers.
2. Game removed (`[PlayGoCore][Uninstall]` for the game, and the panel remove): `ac_uninstall_game` after `g_deleting` is cleared.
3. The add-on branch of that handler (writes `.removed`, deletes the HOLD_DIR folder) runs only when `!g_cfg.dlc_copy`.

- [ ] **Step 1:** `bash build.sh` and `bash test/run.sh`. Expected: pass.
- [ ] **Step 2:** Commit `Uninstall add-on copies when a game or copy mode is removed`.

### Task 6: Per add-on status in the panel

**Files:**
- Modify: `src/main.c` (`/api/game` `dlcs` items near line 2384)
- Modify: `web/index.html`, `web/legacy.html` (the add-on dialog/view third column)
- Modify: `test/web/panel.test.js`

**Interfaces:**
- Produces: each `dlcs` item gets `copy` (`"installed"`, `"installing"`, `"problem"`, `"store"` or `""`) and `reason` (string, the last `.acfail` reason, only for `"problem"`).

- [ ] **Step 1:** In `panel.test.js`, set `DETAIL.dlcs[0].copy = 'installed'`, `[1].copy = 'problem'`, `[1].reason = 'copy stopped'`. For both pages: row 0 shows `Installed`, row 1 shows `Problem: copy stopped`.
- [ ] **Step 2:** `npm test`. Expected: the new checks FAIL.
- [ ] **Step 3:** Add the fields and render `Installed`, `Installing`, `Problem: <reason>`, `From the store` next to the Content/Unlock text on both pages.
- [ ] **Step 4:** `npm test`. Expected: pass.
- [ ] **Step 5:** Commit `Show the copy status of each add-on`.

### Task 7: Console validation

Build, upload to `/data/pldmgr/payloads/local/ps4mount/ps4mount.elf` over FTP (probe 1337 first), `POST /api/stop`, the user starts it from the Payload Manager.

- [ ] **Step 1:** Naruto (CUSA32836): after one scan, `/user/addcont/CUSA32836/<LABEL>/ac.pkg` exist, addcont.db has the rows, the log has no `[mount] /user/addcont` line for it. The user confirms Kawaki in game.
- [ ] **Step 2:** Record whether InstallByPackage returned before the copy ended (log times) and the uninstall not-found code; fix Task 3 if the guess was wrong.
- [ ] **Step 3:** An unlock-only add-on installs and gets a row.
- [ ] **Step 4:** Turn one add-on off in the panel: row and files go. Turn it on: it comes back.
- [ ] **Step 5:** Re-register the game (change add-ons): the copies still work or are reinstalled (Review Focus 3).
- [ ] **Step 6:** Delete one copy from the system storage screen: it shows as off in the panel and is not installed again.
- [ ] **Step 7:** Switch `dlc_copy` off: copies uninstalled, mounts back, SotC add-ons still seen.
- [ ] **Step 8:** Unplug the drive during a copy: one failure counted, the scan continues.
