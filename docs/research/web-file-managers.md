# Research: web file manager features and design patterns for the PS4Mount panel

Date: 2026-10-14. Status: research only, nothing here is implemented.

This document looks at established web file managers and one direct PS5 comparable, and
recommends what the PS4Mount panel's file manager should and should not do. The panel is one
embedded HTML file served by a tiny C HTTP server on the daemon's main loop, used mostly from
phones, in four languages, behind a PIN. The drives hold mostly large `.pkg` files (1–100 GB)
in shallow folder trees, and pkgs that belong to a mounted game must never be deleted or moved.

## Summary and recommendation

**Phase 1 (table stakes plus the one thing only we can do):**

- Browse a drive as a list: name, size, date, folder/file. Pkg rows are **pkg-aware**: the
  daemon already parses pkg headers into the catalog, so a pkg row shows the game icon, game
  name, role (game / update / add-on), and an "in use" badge instead of a cryptic filename.
- **Pkg viewer**: tapping a pkg opens a detail sheet (icon, game name, type, version, size,
  owning game for updates/add-ons, and whether it is protected because the game is mounted).
  This is cheap for us (catalog data exists) and is the single most useful feature for the
  actual audience. The PS5 Web File Manager added exactly this ("PS4/PS5 PKG info preview",
  v1.7) as one of its most-requested features.
- Rename, new folder, delete (with confirmation; server refuses in-use pkgs), and move
  **within the same drive** (a `rename(2)`, instant, no worker thread needed).
- Bulk select with checkboxes (Nextcloud's model), long-press to enter selection mode
  (Filebrowser's model).
- One background task at a time with a progress overlay (the PS5 Web File Manager's model,
  which fits a single-threaded daemon perfectly).

**Phase 2:**

- Cross-drive copy/move on a worker thread with progress, speed, ETA, cancel, a free-space
  pre-check, and conflict prompts (overwrite / skip / auto-rename; fail closed by default).
- Download a single file (streaming GET; mostly useful for logs or small files, not 100 GB pkgs).
- Name filter inside the current folder (client-side, no API needed).

**Never (bloat for this use case):**

- Upload from the browser (users already move pkgs with FTP payloads or a PC; see section 7).
- Text/code editor, media previews, thumbnails of arbitrary images.
- chmod/permissions UI, sharing links, multi-user accounts (the PIN is the whole auth model).
- Archive create/extract (pkgs are not shipped inside archives often enough to justify it).
- Trash bin / undo (drives are usually near full of 50 GB files; a trash bin that holds a
  100 GB pkg is worse than a confirmation dialog). WebDAV (see section 5).

---

## 1. Feature inventory of established web file managers

### Filebrowser (filebrowser/filebrowser)

Source: <https://github.com/filebrowser/filebrowser> (note: the upstream repo was **archived
on August 31, 2026** and is read-only; the actively maintained fork is
<https://github.com/gtsteffaniak/filebrowser>).

- Core ops: browse, upload, download, delete, preview, edit, rename/move/copy, search,
  per-user scopes and permissions, share links. The README's own one-line pitch is that it
  "can be used to upload, delete, preview and edit your files."
- Rename, move, and copy are all one `PATCH` on the resource API with an `action` query
  parameter (details in section 4). Source:
  <https://github.com/filebrowser/filebrowser/blob/master/http/resource.go>
- Uploads use the **tus resumable upload protocol** (`http/tus_handlers.go`,
  `upload_cache_memory.go`, `upload_cache_redis.go` in the repo), not plain multipart.
  Source: <https://github.com/filebrowser/filebrowser/tree/master/http>
- Deliberate omissions / warnings: the README warns that the command-runner feature has
  unresolved security issues and ships disabled, and that JWT sessions cannot be revoked
  before expiry; it tells users not to expose the server directly to the internet.
  Source: <https://github.com/filebrowser/filebrowser>

### FileGator (filegator/filegator)

Source: <https://github.com/filegator/filegator>

- Core ops (quoted from the README): "copy, move, rename, edit, create, delete, preview,
  zip, unzip, download, upload", plus "Zip and bulk download support" and multi-user roles
  with per-user home folders.
- Uploads are "chunked so you should be able to upload large files regardless of your server
  configuration", "built with Resumable.js" with "drag&drop, progress bar, pause and resume".
- Deliberate omissions, stated in the README: "Symlinks are not supported", "File ownership
  is not supported (chown)", "No database required". No search feature is documented.
- Front end is a Vue.js single-page app over Flysystem storage adapters.

### Tiny File Manager (prasathmani/tinyfilemanager)

Source: <https://github.com/prasathmani/tinyfilemanager>

- Notable because, like the PS4Mount panel, it is **one file** (a single PHP script you copy
  to the server).
- Core ops: "Easily create, delete, modify, view, download, copy, and move files", zip/tar
  compress and extract, search ("fast file search and filtering" via datatable.js), Ajax
  uploads with drag-and-drop and multi-file support, a built-in code editor
  ("syntax highlighting for 150+ languages"), document preview, chmod-style permissions.
- Auth: session login with "user-specific root folder mapping"; IP allow/deny lists.
- i18n: "35+ languages with translation.json" — evidence that a single-file manager can
  carry translations, the same way PS4Mount's `I18N` object does.
- It is "Mobile Friendly: Optimized for touch devices", but it does not do chunked uploads,
  so very large uploads depend on PHP limits.

### elFinder (Studio-42/elFinder)

Source: <https://github.com/Studio-42/elFinder>

- The maximalist end of the spectrum: "Usability like the MacOS Finder or Windows Explorer",
  drag-and-drop copy/move, rename, mkdir, upload ("Background file/folder upload",
  "Chunked file upload for large file"), download, archive "create/extract (zip, rar, 7z,
  tar, gzip, bzip2)", "Quicklook, preview for common file types", image and text editing,
  chmod, multi-root volumes, keyboard shortcuts, "List and Icons view", and a
  "Mobile friendly view for touch devices".
- Useful lesson: this is what "everything" looks like, and almost none of it (editors,
  image editing, archive creation, chmod) matters for organizing pkgs on a USB drive.

### Nextcloud Files

Sources: <https://docs.nextcloud.com/server/latest/user_manual/en/files/index.html>,
<https://docs.nextcloud.com/server/latest/user_manual/en/files/access_webgui.html>,
<https://docs.nextcloud.com/server/latest/user_manual/en/files/deleted_file_management.html>

- Core ops in the web UI: browse, open/preview, rename, "Move or copy" from a per-row
  three-dot menu with a folder picker dialog, delete to a trash bin, favorites, versions,
  sharing, search, tags.
- Delete is soft: "By default, deleted files are retained for at least 30 days" in a
  "Deleted files" area, capped at "up to 50% of the account's remaining quota space";
  oldest files are purged first. Restores go to the original location, or the root with a
  unique name on conflict.
- Long operations (sync, large uploads) are handled by the WebDAV layer with chunked
  uploads (section 5).

---

## 2. PS5 Web File Manager (owendswang/ps5-web-file-manager) — the direct comparable

Source: <https://github.com/owendswang/ps5-web-file-manager> (GPL-3.0). This is a web file
manager that runs **as a PS5 homebrew payload**, i.e. the same platform and deployment model
as PS4Mount (an `.elf` sent to the loader on port 9021). It is the closest existing answer to
"what works on a PS5 for this".

**Features** (from the README and releases):

- "List files and folders"; sort by name, type, size, modified time, or permissions.
- "Copy, move, delete, rename, create files, and create folders"; "Multi-select operations";
  copy/move works clipboard-style: "choosing sources first, then pasting or moving them into
  the current folder"; "Conflict prompts for overwriting files and merging folders".
- Upload "single files, multiple files, or folders from a remote browser using the file
  picker or full-page drag and drop"; download a single file directly, or folders/multiple
  selections "as a `.tar` archive".
- Archive **extraction** for 30+ formats, but only via a separate helper payload
  (`wfm-7zip-helper.elf`) added in v1.8 — the main binary does not carry the extractors.
- PKG handling: PKG **installation** (v1.2) and "PS4/PS5 PKG info preview" (v1.7, improved
  with more "PKG metadata" in v1.10). Text editor for files up to 1 MiB; image preview;
  chmod with "checkboxes or a validated four-digit octal mode"; SMB client (v1.10); sending
  `.elf` files to the loader.
- Localization: English and Chinese, picked by browser language.

**Backend architecture:**

- Written in C against the PS5 Payload SDK; HTTP is **libmicrohttpd** (vendored via
  `install-libmicrohttpd.sh`); libsmb2 for SMB. Listens on port 8888, falling back to 8889+.
- Long operations: "Copy, move, delete, upload, and download run as **single background
  tasks**. While one task is running, other file operations are rejected." The UI shows a
  "full-screen task overlay with delayed display, progress, speed, ETA, cancel support, and
  task recovery after reopening the browser while the payload process is still running."
- Safety: "The preparing stage may calculate folder size and check available space";
  "Uploaded files are written through temporary files and are renamed into place after the
  upload completes" (added v1.1); "Delete is recursive and permanent. There is no recycle
  bin." Cancel does partial cleanup.
- Throughput: v1.5 release notes claim "above 80 MB/s from/to the internal storage" and
  100+ MB/s on external drives — proof that a C payload server can move data at drive speed.

**Known pain points** (issue tracker,
<https://github.com/owendswang/ps5-web-file-manager/issues>):

- Data-loss and stability bugs around big copies: "File corruption / folder deletion when
  copying within the same external SSD" (#18), "Kernel panic during large file transfers"
  (#17), kernel panic HDD→internal (#10, closed), "Stuck on finishing..." (#12),
  "复制失败: Operation not permitted" (#16).
- Upload performance: "Improve upload throughput and support resumable uploads" (#8).
- UX requests: a back button (#2), shortcuts (#3), a side menu (#14), quick storage
  switching (#13), and surprise that multi-file downloads turn into a `.tar` (#9).

**What PS4Mount should copy vs do differently:**

- Copy: the **one-background-task-at-a-time** model with a polled progress overlay and task
  recovery after a page reload — it is simple, honest about a single-threaded daemon, and
  proven on this exact console. Copy the free-space pre-check, the temp-file-then-rename
  write pattern, and the explicit conflict prompts. Copy the idea that delete is permanent
  and clearly said to be.
- Do differently: PS4Mount should **not** vendor libmicrohttpd (we have a working tiny
  server in `src/web.c`), should not add SMB, chmod, editors, archive helpers, or PKG
  *installation* (PS4Mount's whole point is playing pkgs without installing). Its UI is a
  generic file table; ours can be **pkg-aware** because the daemon already parses every pkg
  header into the catalog (`src/catalog.[ch]`), so listings can show game name, icon, role,
  and in-use state — something a generic manager cannot do. Its kernel-panic issues on big
  transfers (#17, #10) are a warning: large cross-device copies on this platform need
  conservative buffered I/O, a cancel path, and fsync-before-rename, not cleverness.

---

## 3. Which features matter for pkg drives

Context: files are mostly 1–100 GB pkgs, trees are shallow (often one folder per game, or a
flat dump folder), and the user's real job is "see what is on the drive, group it, free
space, and know what is safe to delete".

**Essential (every manager above has them):** list with name/size/date and folders first;
rename; delete with confirmation; new folder; move; multi-select. (All five surveyed
managers offer this set — see section 1 citations.)

**Valuable specifically for pkg management:**

- The pkg viewer detail sheet and pkg-aware listing rows (game name, icon, role, version,
  owning game, in-use badge). Precedent: ps5-web-file-manager's PKG info preview (v1.7).
- In-use protection surfaced in the UI (grayed delete/move with a reason), enforced
  server-side against the mounted set — unique to PS4Mount, no comparable has it.
- Free-space pre-check before copy/move (ps5-web-file-manager does this in its "preparing
  stage") and the low-space verdict logic that already exists in `src/policy.c`.
- Same-drive move as instant rename; cross-drive copy/move as the one background task.
- Sort by size (finding the 90 GB pkg to delete) and a simple name filter.

**Skippable (bloat here):** text/code editors (Tiny File Manager, elFinder,
ps5-web-file-manager all have one; our files are opaque binaries), media preview and image
editing (elFinder), archive create/extract (elFinder, Tiny File Manager; ps5-web-file-manager
needed a whole separate helper payload for it), chmod (FileGator explicitly dropped chown;
exFAT drives barely have permissions anyway), sharing links and multi-user roles
(Filebrowser, FileGator, Nextcloud — PIN auth already gates everything), WebDAV, SMB,
thumbnails of arbitrary files.

---

## 4. UI patterns for small screens

- **List, not grid.** A grid earns its place when thumbnails identify files (photos). For
  pkgs the identifying data is text (game name, role, size), so a list with a small icon
  per row is denser and scrolls better. Nextcloud defaults to a list and offers grid as a
  toggle (<https://docs.nextcloud.com/server/latest/user_manual/en/files/access_webgui.html>);
  elFinder offers "List and Icons view" (<https://github.com/Studio-42/elFinder>).
- **Breadcrumbs + tapping rows to descend.** Nextcloud: "breadcrumb trail appears at the top
  of the file list so you can jump back to any parent folder with a single click" (same doc).
  With shallow trees a breadcrumb bar is one line and beats a lone back button; notably, a
  back button was one of the first user requests on ps5-web-file-manager (issue #2). Drive
  roots (`/mnt/usbN`, `/mnt/extN`) become the first crumb; quick drive switching was also
  requested there (issue #13), and PS4Mount's panel already knows the drive list.
- **Selection on touch: checkboxes, entered via long-press.** Nextcloud uses explicit
  checkboxes ("Click the checkbox to the left of a file or folder to select it", with a
  select-all in the header). Filebrowser's listing item implements a long-press
  (`startLongPress()`, a 500 ms timer cancelled if the finger moves more than ~10 px) that
  acts like right-click/selection on touch
  (<https://github.com/filebrowser/filebrowser/blob/master/frontend/src/components/files/ListingItem.vue>).
  Recommended blend: tap opens (folder) or shows the pkg sheet (file); long-press enters
  selection mode and shows checkboxes on every row; header gets select-all.
- **Actions in a bottom sheet / per-row menu, not hover toolbars.** Nextcloud puts rename /
  move / copy / download / delete behind a per-row "three-dot action menu" — hover does not
  exist on phones. For PS4Mount the pkg detail sheet doubles as the per-file action sheet;
  in selection mode a fixed bottom bar holds the bulk actions (delete, move). Move/copy
  destination should be a folder-picker dialog (Nextcloud's "Move or copy ... to a folder
  you choose from a picker dialog"), not drag-and-drop, which is desktop-only (elFinder's
  drag model and Filebrowser's draggable rows both degrade on touch).
- **Long operations: full-screen progress overlay** with percent, speed, ETA and cancel,
  that reattaches after a reload (ps5-web-file-manager, README). This also sidesteps any
  need for notifications or job lists.
- Phone width means every column is contested: show name + size (+ in-use badge), put date
  and the rest in the detail sheet. ps5-web-file-manager instead uses "horizontally
  scrollable file lists" on narrow browsers (README) — workable, but hiding columns is nicer.
- i18n cost is real: every label above must be added to all four languages in the `I18N`
  object or `test/i18n_check.js` fails — a reason to keep the verb set small.

---

## 5. HTTP API design for file operations

**Filebrowser's resource API** (source:
<https://github.com/filebrowser/filebrowser/blob/master/http/resource.go>) is the cleanest
minimal-JSON precedent:

- `GET /api/resources/<path>` — stat + directory listing as JSON (sorted; `?checksum=` opts in
  to hashing).
- `POST /api/resources/<path>` — create; a trailing `/` means mkdir, otherwise file upload;
  `?override=true` to replace.
- `PUT` — update an existing file only (404 otherwise).
- `PATCH /api/resources/<path>?action=copy|rename&destination=<dst>&override=...&rename=...`
  — one endpoint for rename, move, and copy; `rename=true` auto-suffixes on collision
  (`addVersionSuffix`); `checkParent(src, dst)` rejects a destination inside the source tree.
- `DELETE` — remove; deleting the scope root is `StatusForbidden`.

**elFinder** uses the other classic shape: a single connector endpoint with `cmd=` (`open`,
`paste` for copy/move with a `cut` flag, `rm`, `mkdir`, `upload`), paths passed as reversible
base64 hashes, chunked upload via `chunk`/`cid`/`range` params, and an `abort` command keyed
by a request id (<https://github.com/Studio-42/elFinder/wiki/Client-Server-API-2.1>).

**WebDAV (RFC 4918)** is the standards option: `MKCOL` (§9.3), `DELETE` (§9.6), `COPY`
(§9.8), `MOVE` (§9.9), with the `Destination` header (§10.3) and the `Overwrite: T|F` header
(§10.6), and multi-item results as 207 Multi-Status XML
(<https://datatracker.ietf.org/doc/html/rfc4918>). Nextcloud exposes exactly this at
`/remote.php/dav/files/{user}/`, including `Destination`-header MOVE/COPY and a separate
chunked-upload endpoint for large files
(<https://docs.nextcloud.com/server/latest/developer_manual/client_apis/WebDAV/basic.html>).

**Verdict for PS4Mount: do not adopt WebDAV.** It would buy interop with DAV clients nobody
will point at a PIN-gated console daemon, at the cost of XML parsing/generation, PROPFIND
semantics, and new HTTP methods in a hand-rolled server. Borrow its two good ideas — an
explicit overwrite flag that defaults to "fail", and move/copy expressed as source +
destination — inside a minimal JSON API matching the existing `/api/*` style in `src/web.c`
(simple paths, `POST` for actions, query/body params, PIN guard on the `/api/` prefix):

- `GET  /api/fm/list?path=/mnt/usb0/games` → JSON rows, each joined against the catalog
  (name, size, mtime, dir flag, and for pkgs: title, role, version, in-use).
- `GET  /api/fm/pkg?path=...` → the pkg viewer payload (or reuse catalog data client-side).
- `POST /api/fm/mkdir`, `/api/fm/rename`, `/api/fm/delete` — synchronous, they are fast.
- `POST /api/fm/move`, `/api/fm/copy` with `src` (repeatable), `dst`, `overwrite=0|1` —
  same-device move happens inline via `rename(2)`; otherwise the daemon starts the single
  background task on a worker thread (the existing install/speed-thread pattern) and
  returns a task id.
- `GET  /api/fm/task` → `{state, current file, bytes done/total, speed, eta}` for polling;
  `POST /api/fm/task/cancel`. One task at a time; a second mutating request gets 409
  (ps5-web-file-manager's "while one task is running, other file operations are rejected").

---

## 6. Safety patterns

- **Sandbox the root.** Filebrowser scopes every user to a filesystem rooted at the server
  root plus a per-user `Scope`, checks every path (and, for recursive ops, every descendant)
  against rules before acting, validates relative paths with `filepath.Rel`, and refuses to
  touch the scope root itself
  (<https://github.com/filebrowser/filebrowser/blob/master/http/resource.go>). elFinder goes
  further and never puts clear-text paths on the wire at all (hashed targets,
  <https://github.com/Studio-42/elFinder/wiki/Client-Server-API-2.1>). PS4Mount equivalent:
  accept only absolute paths, canonicalize, reject any `..` segment or symlink escape, and
  require the result to start with a mounted `/mnt/usbN/` or `/mnt/extN/` prefix — never
  `/data`, `/system_data`, or `/user`. Refuse to delete/rename a drive root.
- **Destination-inside-source check** on move/copy (Filebrowser's `checkParent`): copying
  `/a` into `/a/b` must be rejected before any bytes move.
- **In-use protection** (PS4Mount-specific): before delete/move, the daemon checks whether
  the path is a pkg (or a folder containing a pkg) referenced by a mounted title, and
  refuses with a translated reason. The listing marks these rows so the UI can disable the
  actions up front. This belongs in `src/policy.c` so it gets host tests.
- **Confirm, don't trash.** Nextcloud keeps deletes "for at least 30 days" in a bin capped
  at "50% of the account's remaining quota"
  (<https://docs.nextcloud.com/server/latest/user_manual/en/files/deleted_file_management.html>) —
  affordable for documents, not for 80 GB pkgs on a nearly full exFAT drive.
  ps5-web-file-manager is blunt: "Delete is recursive and permanent. There is no recycle
  bin" (<https://github.com/owendswang/ps5-web-file-manager>). Recommended: a confirmation
  dialog that states exactly what will be removed (count + total size + game names from the
  catalog), plus the in-use guard. That guard already protects the only files that are truly
  catastrophic to lose while playing.
- **Conflict handling on move/copy:** fail closed by default (WebDAV `Overwrite: F`
  semantics, RFC 4918 §10.6), and let the UI re-submit with overwrite, skip, or auto-rename
  (Filebrowser's `rename=true` version-suffix behavior; ps5-web-file-manager's "conflict
  prompts for overwriting files and merging folders").
- **Write patterns for big copies:** pre-check free space, copy to a temp name and rename
  into place on completion (ps5-web-file-manager v1.1), fsync before the rename, and make
  cancel remove the partial file. Its open kernel-panic (#17) and same-disk corruption (#18)
  issues show large transfers are where this platform bites; after any mutation, invalidate
  or refresh the affected drive's catalog cache so the panel's game list stays truthful.

## 7. Upload from the browser

- Plain multipart upload of multi-GB files is where naive implementations die: one
  connection, no resume, and the whole file re-sends on any hiccup. That is why FileGator
  uses Resumable.js chunks ("chunked so you should be able to upload large files regardless
  of your server configuration", <https://github.com/filegator/filegator>), elFinder chunks
  with `chunk`/`cid`/`range` parameters
  (<https://github.com/Studio-42/elFinder/wiki/Client-Server-API-2.1>), Filebrowser moved to
  the tus protocol (`http/tus_handlers.go`,
  <https://github.com/filebrowser/filebrowser/tree/master/http>), and Nextcloud has a
  dedicated chunked-upload WebDAV endpoint
  (<https://docs.nextcloud.com/server/latest/developer_manual/client_apis/WebDAV/basic.html>).
- tus in one line: `POST` creates an upload, `HEAD` returns the current `Upload-Offset`,
  `PATCH` with `Content-Type: application/offset+octet-stream` appends from that offset —
  so a dropped connection resumes instead of restarting
  (<https://tus.io/protocols/resumable-upload>).
- It *can* be done in a C payload: ps5-web-file-manager accepts browser uploads through
  libmicrohttpd, staged through temp files, at 80–100 MB/s — but resumable upload is still
  an open feature request there (issue #8), and uploads occupy its single task slot.
- **Recommendation: do not build upload in Phase 1 or 2.** PS4Mount's server reads requests
  on the daemon's main loop; a 60 GB upload is hours of connection babysitting for a job
  users already solve with an FTP payload or by plugging the drive into a PC (and phones —
  the panel's main client — do not hold 60 GB pkgs anyway). If it is ever added, it must be
  a tus-style offset/append protocol to a temp file on a worker thread, never a single
  multipart POST.

---

### Fit with the existing code (anchors for later work)

- Routes and PIN guard: `src/web.c` (`pin_guard` on the `/api/` prefix, lines ~292–409).
- Pkg metadata for the viewer and pkg-aware rows: `src/catalog.[ch]` (headers cached per
  drive, `CAT_ROLE_*`).
- In-use / decision rules and their host tests: `src/policy.[ch]`.
- Worker-thread precedent for the background task: the install and speed threads in
  `src/main.c` (see commit c709d45).
- Every new panel string: the `I18N` object in `web/index.html`, all four languages,
  checked by `test/i18n_check.js`.
