# Reminders

A to-do app modelled on Apple Reminders, whose data is a folder of plain
Markdown files. No account, no server, no database: every list is a `.md` file you can read and edit in any text editor. You can of course use one of
several synchronization back-ends including Syncthing, CalDAV and WebDAV to connect
your reminders with all your devices.

The app can show several *sources* at once, for example a Syncthing folder
shared with your phone beside a local folder that stays on this computer, or
the task lists of a CalDAV account (Nextcloud, Fastmail, …), or a folder on
a WebDAV server. Each source has
a *back end* that says how its lists are kept in sync.

```markdown
---
reminders: 1
color: orange
icon: cart
---
- [ ] Milk #errands ⏫ 🚩 📅 2026-10-03 17:30 ^milk01
  2% if they have it
- [ ] Bread ^brea01

## Party
- [ ] Balloons 📅 2026-10-04 ^ball01
- [ ] Cake 🔁 every year 📅 2026-11-02 ^cake01
  - [ ] Candles ^cand01
```

The syntax is a superset of the [Obsidian Tasks](https://publish.obsidian.md/tasks/)
format, so the same folder also works as an Obsidian vault.

## Status

| Client | State |
|---|---|
| **Linux (GNOME)**: `Reminders`, C++23, GTK 4 + libadwaita | Working; the reference client |
| **Terminal (CLI + TUI)**: `reminders`, C++23, ncurses | Working; runs anywhere with a terminal, including over SSH |
| iOS: SwiftUI, Syncthing embedded via gomobile | Planned |
| Android, macOS, Windows: native per platform | Planned |

Every client looks native to its own platform because it is. They share
**[the file format](docs/FORMAT.md)**, which is the real contract between them,
and the C++ **core library** (`core/`), which any client that can call C++
can reuse.

## Features (Linux client)

- **Sources**: several open at once, each its own sidebar group, each with its own back end (Syncthing, a plain local folder, a CalDAV account, or a WebDAV folder). Smart lists, tags and search cover them all, and reminders can be moved between them. Managed from ☰ → Sources….
- **Lists**: colours, icons, sections, manual order (drag or Alt+↑/↓), and subtasks one level deep (indent with Ctrl+]).
- **Reminders**: title, notes, URL, due date and time, repeat ("every 2 weeks", weekdays, …), flag, priority, tags.
- **Smart lists**: Today, Scheduled, All, All Reminders (completed ones too), Flagged, Completed, plus one per tag. Search across everything, and a Ctrl+K "Go to" switcher.
- **Quick entry**: type `Pay rent #home 📅 2026-10-31 🚩` into "New Reminder" and the fields are filled in.
- **Import** tasks from iCalendar (`.ics`) files, from ☰ → Import… or `reminders import`; importing again doesn't duplicate.
- **Undo and redo** (Ctrl+Z / Ctrl+Shift+Z) for every change, including moves and deleted lists.
- **Keyboard-driven**: nearly everything has a shortcut; see the [user guide](docs/USING.md#keyboard-shortcuts).
- **Live sync**: changes from other devices appear within half a second. Syncthing conflict copies are merged automatically, reminder by reminder.
- **Hand-editing friendly**:
  - Lines you write by hand keep their exact formatting until you change them in the app.
  - Markdown files with checklists can be turned into lists.
  - Ordinary notes in the same folder are left alone.
- **Notifications** when reminders come due, while the app is running.
- **A terminal client** (`reminders`): commands for scripts (with `--json`) and a full-screen interface. See [docs/TERMINAL.md](docs/TERMINAL.md).

## Sources and back ends

Sources are set up in the app (☰ → Sources… → Add Source…: a name, a type
and a folder, plus the server for CalDAV and WebDAV), with `reminders folder PATH` in a
terminal, or by hand in `settings.ini`:

```ini
[source.personal]
backend=syncthing
folder=~/Sync/Reminders

[source.work]
backend=local
folder=~/Documents/Work lists
title=Work

[source.fastmail]
backend=caldav
url=https://caldav.fastmail.com/
username=me@fastmail.com
password-command=secret-tool lookup service reminders-caldav

[source.cloud]
backend=webdav
url=https://cloud.example.com/remote.php/dav/files/me/Reminders/
username=me
password-command=secret-tool lookup service reminders-webdav
```

| Back end | What it does |
|---|---|
| `syncthing` | The folder is synced by Syncthing; conflict copies are merged (below). Picked automatically for a folder inside a Syncthing folder (one with `.stfolder`). |
| `local` | Just the folder: files are read and written as they are. Outside edits still show up live. |
| `caldav` | Task lists on a CalDAV server, one calendar per list. A local copy (Markdown, like the others) is merged three-way with the server's tasks on start, every few minutes and after each change. Properties the app doesn't use are kept. See [CalDAV accounts](docs/USING.md#caldav-accounts). |
| `webdav` | List files in a folder on a WebDAV server (Nextcloud, ownCloud, a NAS, …), as they are. A local copy is synced file by file: ETag-conditional writes, a three-way merge when both sides changed, renames sent as MOVE. See [WebDAV folders](docs/USING.md#webdav-folders). |

Lists are named `source/name` where a name alone would be ambiguous (in
settings, the CLI and the quick switcher). More back ends (git) are planned.

## How syncing works (Syncthing back end)

The app never talks to Syncthing. It reads and writes files, and Syncthing
moves them between devices. To make that safe, the app:

- **writes atomically** (temporary file + rename), so Syncthing never sees a half-written list;
- **writes only when the content changed**, and never rewrites your lines just to tidy them;
- **gives each reminder a stable id** (`^milk01`), so devices can match reminders when merging;
- **merges `List.sync-conflict-….md` copies** with a three-way merge. Edits from both devices are kept; only when both changed the same field does one side win;
- **keeps per-device state in `.reminders/<device>/`** inside the folder (the merge base for each list) and adds `(?d).reminders` to Syncthing's `.stignore` so it isn't synced.

Details: [docs/FORMAT.md](docs/FORMAT.md).

## Building

### Requirements

- A C++23 compiler (tested with GCC 16 and Clang 22), CMake ≥ 3.25, Ninja (or Make)
- GTK ≥ 4.20 and libadwaita ≥ 1.8, with development headers
- `glib-compile-resources` (part of GLib's development tools)
- ncurses with wide-character support (`ncursesw`), for the terminal client
- libcurl and libxml2, with development headers, for CalDAV and WebDAV
- Python 3, only to run the network tests (they start a small fake server)

| Distribution | Packages |
|---|---|
| Fedora 44 (tested) | `sudo dnf install gcc-c++ cmake ninja-build gtk4-devel libadwaita-devel glib2-devel ncurses-devel libcurl-devel libxml2-devel` |
| Debian / Ubuntu (untested; needs a release with GTK 4.20 and libadwaita 1.8) | `sudo apt install g++ cmake ninja-build libgtk-4-dev libadwaita-1-dev libglib2.0-dev-bin libncurses-dev libcurl4-openssl-dev libxml2-dev` |

Each client can be left out: `-DBUILD_GNOME_APP=OFF` builds without GTK, and
`-DBUILD_TERMINAL_APP=OFF` without ncurses. `-DBUILD_NETWORK=OFF` leaves out
CalDAV and WebDAV (and libcurl and libxml2). The core library alone needs only a
C++23 compiler and CMake.

### Build, test, run

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build            # all the tests (or run them one by one:)
./build/core/core_tests          # 122 tests for the core library (core_tests NAME runs the matching ones)
./build/net/net_tests net/tests/fake_dav.py   # CalDAV and WebDAV over HTTP, against a fake server
./build/linux/Reminders          # the GNOME app (or: ./build/linux/Reminders ~/Sync/Reminders)
./build/cli/reminders --help     # the terminal client
```

### Install

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build
cmake --install build
```

This installs `Reminders` (the GNOME app), `reminders` (the terminal client),
the desktop entry (`com.stephenhouser.Reminders.desktop`) and the app icon.

## Using it

1. Set up a Syncthing folder (or use any folder; syncing is optional).
2. Start Reminders and choose that folder. Add more folders later from ☰ → **Sources…**.
3. Make lists with **New List** (Ctrl+Shift+N), and add reminders by typing into **New Reminder**.

In a terminal: `reminders folder ~/Sync/Reminders`, then `reminders` for the
full-screen interface or `reminders --help` for commands
([terminal guide](docs/TERMINAL.md)).

The **[user guide](docs/USING.md)** covers everything else: lists,
sections, subtasks, repeats, smart lists, keyboard shortcuts, editing files by
hand, and what to do about sync problems.

## Project layout

```
docs/
  FORMAT.md            The on-disk format: the contract every client implements
  USING.md             User guide for the GNOME app
  TERMINAL.md          Guide to the terminal client (CLI and TUI)
core/                  Platform-neutral C++23 library (standard library only)
  include/reminders/   model, format, merge, recurrence, store, library, backend, sources, caldav, webdav, …
  src/
  tests/               Unit tests with a tiny built-in harness (no dependencies)
net/                   CalDAV and WebDAV over HTTP (libcurl, libxml2) and background syncing
  tests/               Tests against fake_dav.py, a small CalDAV and WebDAV server in Python
linux/                 The GNOME client
  src/                 main, window, dialogs, support, gtk_util (RAII + signal helpers)
  data/                style.css, icons, .desktop file, GResource manifest
cli/                   The terminal client: cli.cpp (commands), tui.cpp (ncurses)
INSTRUCTIONS.md        The original brief, and how to recreate this project
```

### The core library

| Header | Purpose |
|---|---|
| `model.hpp` | `Document` (front matter + blocks), `Reminder`, sections, ids. Editing operations: insert, move, indent/outdent, sections |
| `format.hpp` | Parse and serialize list files, with byte-for-byte round trips |
| `merge.hpp` | Three-way (or two-way) merge of a Syncthing conflict copy |
| `recurrence.hpp` | Next date for repeat rules |
| `store.hpp` | One source's folder: loading, saving, conflict handling, list detection, smart-list queries |
| `library.hpp` | Every source open at once: lists keyed `source/name`, smart lists, tags and search across sources, moves between them |
| `sources.hpp` | The `[source.NAME]` sections of settings: loading, saving, the default source, opening one |
| `backend.hpp` | Back ends (`syncthing`, `local`, `caldav`, `webdav`): what each adds to plain loading and saving |
| `caldav.hpp` | CalDAV syncing: pull, three-way merge with local edits, push; the server behind a `Remote` interface |
| `webdav.hpp` | WebDAV syncing, file by file: ETag-conditional writes, three-way merges, renames and deletions; the server behind a `FileRemote` interface |
| `ical.hpp` | iCalendar components and properties, kept close to the text so unknown properties survive |
| `vtodo.hpp` | VTODO ↔ reminder, changing only the properties whose meaning changed |
| `ics_import.hpp` | Reading an `.ics` file's tasks into reminders (ids from UIDs, so importing again skips what's there) |
| `history.hpp` | Undo/redo as before/after snapshots of list files, merging around changes from other devices |
| `syncthing.hpp` | Syncthing sources' per-device state location and the `.stignore` entry |
| `paths.hpp` | The XDG base directories (config, data, state, cache), and `~` / `$VAR` in paths from settings |
| `settings.hpp` | `$XDG_CONFIG_HOME/reminders/settings.ini`, shared by all clients, sidebar layout, and the device name |
| `clipboard.hpp` | Copying and pasting reminders as text |
| `dates.hpp` | Local date, typed dates (`tomorrow`, `fri`, `+3d`), relative labels (`Tomorrow`, `Oct 3`) |

### CalDAV and WebDAV (`net/`)

| Header | Purpose |
|---|---|
| `caldav_client.hpp` | The `Remote` over HTTP: finding the calendars, listing, fetching and storing tasks, making calendars |
| `webdav_client.hpp` | The `FileRemote` over HTTP: PROPFIND, GET, PUT, DELETE and MOVE on the folder's files, made if missing |
| `server_sync.hpp` | `sync_source()` for either kind; `password-command=` |
| `sync_runner.hpp` | Background syncing for the apps: on start, every `interval=` minutes, and shortly after a list file changes |

### The GNOME client

It uses the GTK and libadwaita **C APIs** directly from C++, with small helpers
in `gtk_util.hpp`: an owning `Obj<T>` for GObjects, `connect<Sig>()` to attach
lambdas to signals, actions, timeouts and shortcuts. The UI is rebuilt from the
library after every change, which keeps the code simple. Lists are small.

## Development notes

- **Screenshots without screen capture:** GNOME locks down screen capture on
  Wayland, so the app can render its own window:
  `REMINDERS_SCREENSHOT=out.png ./build/linux/Reminders` saves a PNG after
  1.5 s and quits.
- **Testing without touching your real session:** the app is single-instance,
  so a test launch can be handed to your running copy, and a test window can
  take keystrokes meant for something else. Run tests on a private D-Bus
  session and a hidden screen (GNOME's mutter in headless mode):
  ```sh
  dbus-run-session -- sh -c '
    mutter --headless --wayland --no-x11 --virtual-monitor 900x640 --wayland-display=test &
    sleep 1
    WAYLAND_DISPLAY=test XDG_CONFIG_HOME=/tmp/r/config XDG_STATE_HOME=/tmp/r/state XDG_DATA_HOME=/tmp/r/data \
      XDG_CACHE_HOME=/tmp/r/cache REMINDERS_SCREENSHOT=/tmp/r/shot.png \
      ./build/linux/Reminders /tmp/r/lists'
  ```
- **Driving the app from scripts:** actions are exported over D-Bus, e.g.
  `gdbus call --session --dest com.stephenhouser.Reminders --object-path /com/stephenhouser/Reminders/window/1 --method org.gtk.Actions.Activate go-to '[]' '{}'`.
- **Testing the TUI:** run it on a private tmux server, send keys, and
  capture the screen:
  `tmux -L test new -d -s t -x 100 -y 30 ./build/cli/reminders; tmux -L test send-keys -t t 6 Tab j; tmux -L test capture-pane -p -t t`.
- **Memory checks:** the core tests run clean under `valgrind`, and with
  `-D_GLIBCXX_DEBUG`.

## License

Not chosen yet.
