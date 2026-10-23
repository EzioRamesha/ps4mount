# Add-ons installed to internal storage

## Intent

Some games ask the system for their add-on list (AppInstUtil/AppPromoter, backed by
`/system_data/priv/mms/addcont.db`). PS4Mount mounts add-ons as nullfs folders and never
gets a row into addcont.db, so those games do not see their add-ons (issue #21: Naruto
Kawaki, Dave the Diver, Cuphead). Writing addcont.db directly is ruled out: the system
databases are written by the system alone (`src/db.c`).

`sceAppInstUtilInstallByPackage` with the add-on pkg on the USB drive makes the system copy
the pkg into `/user/addcont/<ADD-ON TID>/<LABEL>/` and write the addcont.db row itself.
Validated on the console with Naruto Kawaki (seen in game) and The Order DLC200.
`sceAppInstUtilAppUnInstallAddcont(title_id, label)` removes the row and the files
(validated).

## Goals

1. Add-ons of every game are installed to internal storage by copy, automatically.
2. The panel add-on on/off switch installs and uninstalls the copy.
3. Removing a game from the library uninstalls the copies PS4Mount installed for it.
4. Games whose add-ons are mounted today move to copy mode without user action.
5. A Settings option switches back to the current mount mode.

## Non-goals and anti-goals

- No direct write to addcont.db or app.db, ever.
- Never uninstall an add-on PS4Mount did not install (store or other tools).
- No `place_dlc`/`drop_dlc` for add-ons in copy mode: `place_dlc` overwrites the PlayGo
  records and license of the system install.
- The game base and its update stay mounted from the USB drive as today.

## Decided

1. **Mode setting.** New `config_t` field `dlc_copy` (config key `dlc_copy`, default 1).
   1 = copy mode, 0 = current mount mode. Shown in Settings next to the existing add-ons
   option, on `index.html` and `legacy.html`. Existing `dlc` (add-ons on/off globally)
   keeps its meaning.
2. **Install call.** `sceAppInstUtilInstallByPackage(meta, pkginfo, playgo)` with a zeroed
   0x70-byte MetaInfo, `uri` = the add-on pkg path on the drive
   (`<root>/ps4/<GAME>/addcont/<ADD-ON TID>/<LABEL>/ac.pkg`), `content_name` =
   `"<GAME TID> (Add-on)"`, same structs as the TEMP `app_actest` in `src/main.c`.
3. **Own record.** `STATE_DIR/<CONTENT ID>.accopy` written after the install call returns
   0, holding the pkg path. Only add-ons with this file are ever uninstalled by PS4Mount.
4. **When installs run.** During registration of a game (the place where `place_dlc` runs
   today), in copy mode: for each add-on in use, if no `.accopy` and no installed copy,
   install it. For each add-on turned off that has `.accopy`, uninstall it and delete the
   file. Installs go one at a time.
5. **Mount guard.** In copy mode `mount_paths` never mounts `/user/addcont/<ADD-ON TID>`.
   Before installing, if that folder is a nullfs mount from `HOLD_DIR/addcont/<TID>`
   (ours), unmount it and remove the HOLD_DIR add-on folder and the PlayGo/appmeta
   records `place_dlc` wrote. A mount from anywhere else aborts the install for that
   add-on with a log line.
6. **Migration.** Point 5 applied on the first registration after the update converts
   mounted games. No separate migration step.
7. **Game removal.** Where a game removal is handled today (`[PlayGoCore][Uninstall]` for
   the game, and the panel remove path), uninstall every `.accopy` add-on of that game.
8. **Back to mount mode.** Switching `dlc_copy` to 0 uninstalls every `.accopy` add-on and
   registers the games again so `place_dlc` and the mounts come back.
9. **Free space (builder's call, mine).** Before an install, `statfs("/user")` free bytes
   must exceed the pkg size plus 1 GiB, else skip it and show the reason.
10. **Status (my call).** The panel shows per add-on: installed, installing (`.accopy`
    present, `/user/addcont/<TID>/<LABEL>/ac.pkg` not yet full size), failed (with the
    return code or the free-space reason). Install failures also go through `notify`.
11. **Cleanup.** Remove the TEMP code: `app_actest`, `app_acdel`, their routes and
    declarations, and the closetest page, route, build entry and button.

12. **Reconcile pass (my call).** Copy-mode work runs as its own pass on every scan, not
    only when a game's marker changes, so skipped or interrupted installs are retried.
    It runs after the game's registration has finished, since registering a game makes
    the system uninstall its add-ons' PlayGo data. A copy whose files lose their PlayGo
    data after a later registration is installed again.
13. **Completion (my call).** If the install call returns before the copy ends, the pass
    polls the copied file size every 2 s (never faster than 1 s) with a 30 min timeout.
    One install at a time. A timeout or a size that stops growing for 2 min counts as
    failed, the `.accopy` is removed and the add-on is retried on the next scan.
    Failures are counted in `STATE_DIR/<CONTENT ID>.acfail`. After 3 failures the add-on
    is not installed again and the panel marks it as a problem, with the last reason.
    Turning it off and on in the panel clears the count (my call); a successful install
    deletes the file.
14. **Per label (my call).** The "already installed" check is per label
    (`/user/addcont/<TID>/<LABEL>/ac.pkg` present and not ours means store install: skip).
    Labels from the store are never touched; other labels of the same title are copied.
15. **Uninstall argument.** Label first, content ID as fallback, as in `app_acdel`.
    "Not found" counts as success (the system may have removed it already, for example
    when the game itself was deleted). Uninstalls run after `g_deleting` is cleared.
16. **Unlock-only add-ons (my call).** Add-ons without data are installed by copy too,
    since games that query the system need their row. Validated on the console during
    the tests.
17. **Mode switch during an install (my call).** The switch waits for the install in
    progress to end, then uninstalls.
18. **Delete from the system UI.** A copied add-on deleted by the user from the system
    storage screen (its `.accopy` exists but the row and files are gone, outside a
    PS4Mount uninstall) is turned off: its content ID goes into `<GAME TID>.dlcoff`, the
    `.accopy` is removed and it is not installed again until turned on in the panel.

## Left to the builder

- How the "installing" state is detected exactly (file size vs pkg size is the default).
- Log wording, following the existing `[reg]`/`[mount]` prefixes.
- Panel layout of the status, matching the current add-on list.

## Constraints

- English only in code and docs. One commit per fix, generic messages, no trailers.
- Every panel change on both `index.html` and `legacy.html` (ES5), tested on both.
- Libraries are linked into the PS4Mount build (fresh payloads linking
  libSceAppInstUtil hang on the current HEN stack).
- Console tests: the user runs the build from the Payload Manager slot; Claude checks the
  log and addcont.db over FTP.
