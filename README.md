# PS4Mount

> [!CAUTION]
> **PS4Mount is a work in progress. Use it at your own risk.**
>
> On some consoles and firmwares it can still make the system report that its **database is
> corrupted**. When that happens the console rebuilds its database on the next start, games added by
> PS4Mount can disappear from the library until PS4Mount runs again, and you may have to restart the
> console to get out of repeated error prompts. A forced restart can lose data that was not saved yet.
> **Back up your save data** (PS Plus cloud storage or a USB drive) before using PS4Mount, and
> [report any problem](#reporting-problems) with the log.

Play PS4 fake PKGs on a jailbroken PS5 straight from a USB drive, without installing them.

Put the game (and its update) `.pkg` files on a USB drive, plug it in and load `ps4mount.elf`.
PS4Mount finds the packages, adds the games to the home screen and lets you play them from the
drive. Nothing is copied to the console storage, and deleting a game on the console only removes
it from the library: the `.pkg` stays on the drive.

![PS4Mount icon](assets/icon0.png)

## Screenshots

Notifications on the home screen while PS4Mount scans the USB drives:

![Scan notifications on the PS5 home screen](docs/notifications.png)

The web panel, opened from the **Media** tab:

![PS4Mount web panel](docs/web-panel.png)

## Features

- Detects PS4 fake PKGs (game, update and add-ons) in every folder of every connected USB drive and
  uses them where they are: keep one folder per game, with its update and add-ons in subfolders, or
  any other layout.
- Adds the games to the home screen with their icon, name and update version.
- Applies updates: put the update `.pkg` anywhere on the same drive and it is picked up automatically.
  With several updates of a game, the highest version is used, or the one you pick in the panel
  (including no update).
- Installs add-ons (DLC), with data or unlock-only. An add-on is matched to its game by title ID, or by
  the add-on IDs the game lists (so add-ons of another region of the same game work too). Each
  add-on can be turned off and on again in the panel.
- Optionally uses an update or add-ons from another drive than the game.
- Waits for pkgs that are still being copied (or were cut off) instead of failing on them.
- Tests the read speed of each drive when it is connected, and warns when it is much slower than
  usual, which usually means a loose or bad cable, or a USB 2 port.
- Adds many games in groups, so a drive with dozens of games does not overload the console.
- Stops adding games as soon as the system reports a corrupted database, and stops retrying a game
  that failed 3 times, so a problem never turns into an endless loop (see [Safety](#safety)).
- Remembers what it read of each `.pkg`, so after a restart only new or changed files are read.
- Games run from the drive; no internal storage is used for the packages.
- Deleting a game from the PS5 removes it from the library only. The `.pkg` is never deleted.
- Notifications while scanning (how many games were found on each drive) and for added and removed
  games.
- A web panel, also available as **PS4Mount** in the **Media** tab, to see the games and add or
  remove them from the library (one by one or several at once), keep a game out of the library, pick
  its update and add-ons, see the pkgs that are not used and why, filter and scan by drive, download
  the log and copy a diagnostics report. It warns when a drive is almost full, can be protected with
  a PIN, tells you when a newer PS4Mount is released, and is available in English, Spanish,
  Portuguese (Brazil) and French.

## Requirements

- A jailbroken PS5 with fake PKG support (for example etaHEN with kstuff).
  Any firmware with fake PKG support. From 6.00 the games are registered the usual way (reported
  working on 6.50, tested on 13.20). Below 6.00 that made the console report a corrupted database
  (5.50), so there PS4Mount registers each game on its own (confirmed working on 5.50), and does not
  add games if the firmware lacks that function. The startup notification and the log say which one
  is used.
- An ELF loader listening on port **9021** to send the payload.
- Access to the kernel log: either **klogsrv** running on port 3232, or no other payload reading
  `/dev/klog`. PS4Mount reads the system log to know when a game starts and when a delete is
  confirmed. Without it, games cannot start (see [How it works](#how-it-works)).
- A USB drive formatted as **exFAT**.
- PS4 **fake** PKGs (game, and optionally its update and add-ons). Retail PKGs are not supported.

## Usage

Download `ps4mount.elf` from the [Releases](https://github.com/gasb07/ps4mount/releases) page
(or [build it](#building)).

1. Copy the PS4 game `.pkg` (and its update and add-on `.pkg` files, if you have them) to the USB
   drive, in any folder. The file and folder names do not matter.
2. Plug the drive into the PS5.
3. Send `ps4mount.elf` to the ELF loader (port 9021). Do this after every console boot, or add it to
   the payloads your jailbreak loads automatically.
4. Wait for the notifications:
   - `Scanning usb0 for PS4 games...`
   - `usb0: 2 PS4 game(s) found, 2 new or updated`
   - `Adding 1/2: <game name> (CUSAxxxxx)`
   - `<game name> ready to play`
5. Start the game from the home screen.

PS4Mount never moves or renames the packages: it searches every folder of the drive and groups each
game with its update and add-ons on the same drive. For example:

```
/Games/Persona 5/Persona 5.pkg       game
/Games/Persona 5/Update/v1.10.pkg    update (optional)
/Games/Persona 5/DLC/*.pkg           add-ons (optional)
```

Hidden folders, `System Volume Information` and `$RECYCLE.BIN` are not searched. When the same game is
twice on a drive, the first copy (by path) is used and the scan notification says so. A game is
reported as ready only after its add-ons are installed too. An update or add-on whose game is not on
the same drive is not used, unless **Use updates and add-ons from other drives** is on (see
[Settings](#settings)); the scan notification says how many there are. **Problems** in the panel lists
these pkgs (duplicates, add-ons or updates without their game, pkgs that cannot be read, and incomplete
pkgs) with their path.

A pkg that is smaller than its header says (still being copied, or a copy that was cut off) is
listed as incomplete with how much of it is there, and is used on the first scan after it is complete.
A pkg whose full size was reserved before the copy stopped cannot be told apart from a complete one.

To add more games later, copy their `.pkg` files to the drive.

With **Move pkgs into the ps4 folder** on (see [Settings](#settings)), PS4Mount instead moves every
package it uses into a folder per game on the same drive, as earlier versions did:

```
/ps4/CUSAxxxxx/app.pkg                          game
/ps4/CUSAxxxxx/patch.pkg                        update (optional)
/ps4/CUSAxxxxx/addcont/CUSAyyyyy/<LABEL>/ac.pkg add-on (optional)
```

Games already in this layout keep working with the setting off. Each `CUSAxxxxx` folder also gets a
`<game name>.txt` file with the title, version and the original file names, so the folders can be told
apart on a computer.

### Web panel

Open **PS4Mount** in the **Media** tab, or browse to `http://<PS5 IP>:8770` from any device on
the same network. The panel shows every game found on the connected drives, with its state:

| State | Meaning |
| --- | --- |
| Ready | In the library and ready to play |
| Running | The game is open |
| Not in library | Removed from the library; the `.pkg` is still on the drive |
| Adding | Being added to the library |
| Installed on console | A normal installation of this game exists; PS4Mount leaves it alone |
| On another drive | The same game is also on another connected drive, which is the one used |
| Failed | Adding it failed 3 times; it is not tried again until you click **Try again** or **Scan for games** |
| Never added | You chose **Never add**; every scan skips it until you click **Allow** |

Games can be searched by name or title ID, sorted by name, state or location, and filtered by drive.
The summary shows the free and total space of each drive. A drive with less than 2 GB or 5% free is
marked **almost full**, and a notification says so once (again only after space was freed and ran low
again).

**Drive speed.** A cable that is not pushed in well, a worn cable or a USB 2 port can make a fast drive
fall back to USB 2 speed (about 40 MB/s instead of 300 or more). The drive still works, so this
otherwise only shows as games loading slowly or stuttering. PS4Mount checks it:

- After a drive is connected (and once after PS4Mount starts), it reads 64 MB of the drive's largest
  pkg. When the speed is under half of that drive's best earlier result, a notification says so, for
  example `usb0 reads at 38 MB/s, usually 410 MB/s: check the cable or port`.
- Pick a drive in the drive filter and click **Test speed** for a longer test (256 MB, at most 20
  seconds). The result shows next to the drive in the summary, in orange when it is slower than usual.
  The first test of a drive under 45 MB/s is marked "USB 2 speed": normal for a USB stick, low for an
  SSD.

A test only reads; nothing is written to the drive. It does not run while a PS4Mount game is open or
games are being added, so it never slows a game down; the check after connecting waits until then.
The best speed of each drive is kept in `/data/ps4mount/speed.txt`, by the drive's exact size (two
drives of exactly the same size share it). The test needs a pkg of at least 64 MB on the drive. It
finds a drive that is slow when tested; a cable that only drops out now and then can still pass.

A game with update pkgs has an **Update** list: **highest** (the default), **No update**, or a
specific version. The game is added again with the chosen update; the choice is kept in
`/data/ps4mount/updates.txt`. "with update from another drive" next to the version means the update
used is on another drive than the game.

A game with add-ons has an **Add-ons** list with each add-on's name and state (installed, being added,
turned off, deleted on the console, and whether it is on another drive). Untick an add-on to turn it
off: the game is added again without it, and the choice is kept in `/data/ps4mount/addons-off.txt`.
Tick it again to add it back, including an add-on deleted on the console. When more than 29 add-ons
of a game are on, the panel warns that a running game only sees about 29 of them (see
[Good to know](#good-to-know)) so you can turn off the ones you do not need. Add-ons cannot be
changed while the game is running.

The panel follows the browser's language (English, Spanish, Portuguese (Brazil) or French), or the
one picked under **Language** in the settings, which is kept in that browser only. Messages sent by
the console (notifications, the log and the confirmations shown after an action) stay in English.
The translations were not written by native speakers: corrections are welcome in an issue or pull
request.

Tick the box on several games to **Add selected**, **Remove selected** or **Never add selected** at
once. The games are handled one after the other.

When a newer PS4Mount is released, a **New: vX.Y.Z** link appears next to the version. The browser you
use for the panel asks GitHub at most every 12 hours; the console itself never goes online for this.

Buttons:

- **Remove from library**: removes the game from the home screen. The `.pkg` stays on the drive.
- **Never add** / **Allow**: keeps a game out of the library on every scan (for example a game you
  only keep as a backup), or lets it be added again. The list is kept in `/data/ps4mount/excluded.txt`.
- **Move pkgs back**: removes the game from the library and moves its `.pkg` files back to where they
  came from (see [Moving pkgs back](#moving-pkgs-back)). Only shown for games moved into the ps4 folder.
- **Try again**: tries a failed game again.
- **Add to library**: adds a removed game back.
- **Scan for games**: checks the drives now and shows how many games were found on each one. With a
  drive picked in the drive filter, the button becomes **Scan usbN** and checks that drive only.
- **Test speed**: with a drive picked in the drive filter, measures its read speed (see above).
- **Settings**: see below.
- **Problems**: lists the pkgs found but not used (see [Usage](#usage)).
- **Files**: browse the drives and manage pkgs directly (see [Files](#files)).
- **Log**: shows the end of the PS4Mount log; **Download the whole log** saves it as
  `ps4mount-log.txt`.
- **Diagnostics**: a report to paste in a bug report (version, firmware, settings, drives, failed
  games and the end of the log), with a **Copy** button. It holds no password or network address.
- **Stop**: stops PS4Mount. Its games cannot start until it is loaded again.

### Settings

![PS4Mount settings](docs/settings.png)

| Setting | Default | Meaning |
| --- | --- | --- |
| Language | Automatic | Language of the panel. Kept in the browser, not on the console, so each device can use its own. |
| Move pkgs into the ps4 folder | Off | Moves new `.pkg` files into `/ps4/CUSAxxxxx/` (see [Usage](#usage)). When off, every `.pkg` is used where it is. |
| Folders whose pkgs are moved | `/` | With the setting above on: folders, relative to each drive, whose `.pkg` files are moved (one per line). `/` is the root. The `ps4` folder cannot be used: it holds the games already moved. |
| Add new games in groups | On | Adds many new games in groups instead of one after the other, which keeps the console stable when a drive has a lot of games. |
| Games per group | 5 | Size of each group (1 to 50). |
| Folders to ignore | (none) | Folders, relative to each drive, whose `.pkg` files and subfolders are never used (one per line), for example `/Backup`. |
| Seconds between scans | 10 | How often the drives are checked (10 to 300). Longer is lighter on drives with many folders; **Scan for games** always checks at once. |
| Install add-ons (DLC) | On | Installs add-on `.pkg` files. When off, add-ons are not used. |
| Use updates and add-ons from other drives | Off | Uses an update or add-on whose game is on another connected drive. Keep both drives connected: when the drive with the update or add-ons is unplugged, the game is added again without them. |
| Panel PIN | (none) | 4 to 8 digits. With a PIN, each browser must enter it once before using the panel. Leave the field empty to keep the PIN, or tick **Remove the PIN**. |

The settings also have **Move all games back** and **Pick up moved-back pkgs again** (see below).

Settings are saved in `/data/ps4mount/config.txt`.

Without a PIN, anyone on your local network can use the panel. With a PIN, 5 wrong PINs in a row block
the panel for a minute. The panel uses plain HTTP, so the PIN keeps out people on your network, not
someone who can read its traffic. If you forget the PIN, delete the `pin=` line from
`/data/ps4mount/config.txt` over FTP and load PS4Mount again.

### Safety

If the system log reports a corrupted system database (`DbCorruptionOccured`), PS4Mount stops adding
games at once: games already in the library keep working, but no new game is registered, even after a
restart of PS4Mount. The panel then shows a red banner. Restart the console, check that the database
message is gone, then click **Resume adding games**. This needs klogsrv or access to `/dev/klog` (see
[Requirements](#requirements)).

A game that fails to be added is tried again after 5 minutes, at most 3 times. After that it shows as
**Failed** until you click **Try again** or **Scan for games**.

### Removing games

Deleting a game from the PS5 (Options > Delete on the home screen, or Settings > Storage), or using
**Remove from library** in the panel, removes the game from the library and leaves the `.pkg` on the
drive. A removed game comes back
when you reconnect the drive or click **Add to library**.

To delete the game for good, delete its `.pkg` files (or its `/ps4/CUSAxxxxx` folder) from the drive on
a computer.

### Moving pkgs back

This applies to games moved into the ps4 folder. PS4Mount records where every `.pkg` came from in `ps4/CUSAxxxxx/origin.txt` on the drive: the folder
(any of the scan folders) and the original file name. **Move pkgs back** on a game, or **Move all games
back** in the settings, undoes the move:

- The game leaves the library (several games are handled in groups, like when adding).
- The game, its update, older updates kept aside and its add-ons go back to their original folder with
  their original name. A folder that no longer exists is created again, and an existing file is never
  replaced: ` (1)` is added to the name.
- `.pkg` files moved by versions before 0.5.0 have no record and go to the drive root as
  `CUSAxxxxx - <game> - game.pkg`, `... - update.pkg` and `... - <add-on label>.pkg`.
- The files PS4Mount created for the game are removed. Folders are only removed when empty, and no
  `.pkg` is ever deleted.

The `.pkg` files moved back stay where they are on the next scans (they are listed in
`/data/ps4mount/restored.txt`). To add them again, use **Pick up moved-back pkgs again** in the
settings. A `.pkg` you move, rename or replace yourself is picked up as usual.

### Files

Click **Files** to browse the connected drives directly: folders, pkgs and other files, with
breadcrumbs back to the drive root. Each pkg shows what it is (game, update or add-on, its title ID
and version) and whether a library game is using it. From the list you can make a new folder, and
tap a file to rename, move or delete it. Folders can only be opened for now; renaming, moving and
deleting a folder is planned for a later version.

A pkg used by a game in the library cannot be renamed, moved or deleted: remove the game from the
library first, or move its pkgs back. Moving is limited to the same drive for now; moving a file
between drives is planned for a later version. Deleting a file is permanent: there is no undo and
no trash.

## Good to know

- Load PS4Mount soon after every boot. While it is not running, the console does not see the
  packages and may hide the games; they come back as soon as PS4Mount runs again.
- Do not unplug the drive while a game is running.
- **Two or more drives connected at the same time can make the whole console interface slow** (home
  screen, Control Center, game hubs), with or without PS4Mount: the system checks its storage devices
  more often with each extra drive or partition. A drive with extra partitions (for example a boot
  partition added by a multiboot tool) counts as several. If the interface gets slow, keep one drive
  connected, ideally with a single partition.
- For a game with an update, the library and the PS5's own "installed" notification show the version
  of the game pkg (for example 01.00). The game runs with the update; the web panel shows its version.
- Games already installed the normal way on the console are never touched, and their add-ons on the
  drive are not installed.
- **Close PS4Mount games before putting the console in rest mode.** Rest mode powers the drive off, and a
  game left open cannot read its files from the drive after waking up: it locks up, and closing it can
  hang on the "Closing" screen, which needs a console restart. Games that were not open stay in the
  library and work normally after waking up.
- **Games with more than about 29 add-ons:** while a game runs, the console mounts each of its packages (the game,
  the update and every add-on) with two virtual devices: a PS4 package holds an outer filesystem with the
  real content as a second filesystem inside it. The console has 64 of these devices, so a running game
  can use at most about 30 packages: the game, its update and roughly 29 add-ons. A game with more
  add-ons (for example Civilization VII with 35) starts normally but only sees the first add-ons, in
  alphabetical order of their label. This is a console limit that applies to installed games too; it
  does not limit how many games you can have. Turn off the add-ons you do not need in the panel's
  **Add-ons** list so the ones you want are seen.
- If a game fails to start right after being added, start it again. The log (panel > **Log**) shows
  what happened.

## Reporting problems

If something does not work, open an issue with the
[bug report form](https://github.com/gasb07/ps4mount/issues/new?template=bug_report.yml). It asks for
what is needed to find the cause:

1. **PS4Mount version**: shown at the top of the web panel and in the startup notification.
2. **PS5 firmware**, the **jailbreak / HEN** and every **payload** you load, with their versions.
3. Whether **klogsrv** is running.
4. **The game**: name and title ID (`CUSAxxxxx`), and whether it has an update and add-ons.
5. **Your drives**: how many are connected, how they are formatted (exFAT) and whether a drive has
   more than one partition.
6. **What happened**: what you did, what you expected, and the exact message or error code shown on
   the console.
7. **The diagnostics report**: open the web panel, click **Diagnostics** and **Copy**. It covers
   points 1, 3 and 5 and ends with the log. For a longer log, click **Log** > **Download the whole
   log** (or download `/data/ps4mount/log.txt` over FTP), and include the part from the moment the
   problem happened.

Do not attach `.pkg` files or links to them.

## How it works

A PS4 PKG install on the PS5 is mostly a byte copy of the package to `/user/app/<TITLE_ID>/app.pkg`
plus some metadata. PS4Mount skips the copy:

1. It writes the files an install would leave on the console, generated from the package itself:
   `app.json`, `app.pbm`, `app.xml`, `patch.*`, the PlayGo records (`/user/playgo/...`,
   `/system_data/playgo/<TITLE_ID>/pfs.pbm`), the app metadata, and the fake license (`.rif`/`.idx`)
   decrypted from the package.
2. The game files go into an internal folder, `/system_data/ps4mount/app/<TITLE_ID>` (and
   `/system_data/ps4mount/patch/<TITLE_ID>` for an update), where `app.pkg` and `patch.pkg` are
   symbolic links to the packages on the drive. That folder is mounted over `/user/app/<TITLE_ID>`
   (and `/user/patch/<TITLE_ID>`) with `nullfs`. The drive itself is never mounted there.
3. It asks the system to register the title (`sceAppInstUtilAppInstallAll`), which adds it to the
   library and refreshes the home screen. PS4Mount never writes to the system databases itself.
4. A delete from the console can only remove the links, never the packages. While a delete screen
   is open (on the home screen or in Settings > Storage) PS4Mount also unmounts the game folders,
   so the system can remove `/user/app/<TITLE_ID>` without an error.

State and log are kept in `/data/ps4mount/`. The Media tab shortcut is `PMNT00001`
(`/user/app/PMNT00001`).

## Building

Requirements: the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) in `/opt/ps5-payload-sdk`
(or set `PS5_PAYLOAD_SDK`), `xxd` and bash.

```sh
./build.sh          # builds build/ps4mount.elf
```

Unit tests (crypto, the generated install files and the `param.sfo` parser) run on the computer, with
no console or SDK:

```sh
bash test/run.sh
```

GitHub Actions runs them before every build.

Optional tools:

```sh
python3 tools/make_icon.py                                 # redraws assets/icon0.png (needs Pillow)
python3 tools/gen_keys.py <LibOrbisPkg>/LibOrbisPkg/Util/Keys.cs   # regenerates src/keys.h
```

### Releases

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request with the PS5
Payload SDK release set in `SDK_VERSION`, and keeps `ps4mount.elf` as a build artifact.

To publish a release:

1. Set the new version in `src/main.c` (`#define VERSION "PS4Mount X.Y.Z"`) and commit it.
2. Tag the commit and push the tag:
   ```sh
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

The workflow checks that the tag matches `VERSION`, builds the payload and creates the GitHub
release with `ps4mount.elf` and its SHA-256 checksum attached.

| Path | Content |
| --- | --- |
| `src/main.c` | Daemon: drive scan, registration, mounts, klog events, panel actions |
| `src/pkg.c` | PKG reader and fake-key entry decryption |
| `src/catalog.c` | Finds the `.pkg` files in every folder of a drive, groups them per game and keeps what was read in `/data/ps4mount/catalog-<drive>.txt` |
| `src/safety.c` | Database corruption reports and the limit on retries |
| `src/speed.c` | Read speed test of a drive, on one of its pkgs |
| `src/build.c` | Generators for the install files (app/patch metadata, PlayGo records, license) |
| `src/db.c` | Library rows (`app.db`, `appinfo.db`) |
| `src/crypto.c` | SHA-256, AES-128-CBC, CRC32C, RSA-2048 |
| `src/logfile.c` | Keeps `log.txt` under 1 MB |
| `src/web.c`, `web/index.html` | Web panel |
| `test/` | Unit tests (`bash test/run.sh`) |
| `tools/pdb_*_template.bin` | PlayGo record layouts (no title data) |
| `third_party/sqlite/` | SQLite amalgamation |

## Credits

PS4Mount does not contain code copied from other projects except where noted below.

| Project | Author | License | Use |
| --- | --- | --- | --- |
| [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) | John Törnblom | GPL-3.0-or-later | Toolchain, headers and runtime linked into the payload |
| [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) | maxton | LGPL-3.0 | Source of the fake PKG key (`PkgDerivedKey3Keyset`, in `src/keys.h`) and reference for the PKG entry encryption, entry IDs and the `playgo-chunk.dat` layout |
| [SQLite](https://sqlite.org) | SQLite authors | Public domain | `third_party/sqlite/` (amalgamation 3.46.1), used to write the library databases |
| [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | drakmor | GPL-3.0 | Inspiration for auto-mounting games from USB drives and for registering titles with `sceAppInstUtilAppInstallAll` on firmware 12.00+. No code was used. |
| [ps5-app-dumper](https://github.com/EchoStretch/ps5-app-dumper) | EchoStretch | see project | Reference for the Media tab shortcut (`applicationCategoryType` 65536 with `deeplinkUri`). No code was used. |

## License

PS4Mount is licensed under the GNU General Public License v3.0 or later, see [LICENSE](LICENSE).
It links the PS5 Payload SDK runtime (GPL-3.0-or-later). `src/keys.h` carries data published by
LibOrbisPkg under the LGPL-3.0. SQLite is in the public domain.

This project is not affiliated with or endorsed by Sony Interactive Entertainment. Only use it with
games you own.
