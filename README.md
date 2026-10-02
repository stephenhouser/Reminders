# Reminders

A to-do app modelled on Apple Reminders, whose data is a folder of plain
Markdown files kept in sync by [Syncthing](https://syncthing.net). No account,
no server, no database: every list is a `.md` file you can read and edit in any
text editor, and every device that syncs the folder sees the same lists.

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
| **Linux (GNOME)**: C++23, GTK 4 + libadwaita | Working; the reference client |
| iOS: SwiftUI, Syncthing embedded via gomobile | Planned |
| Android, macOS, Windows: native per platform | Planned |

Every client looks native to its own platform. They share
**[the file format](docs/FORMAT.md)**, which is the real contract between them,
and the C++ **core library** (`core/`), which any client that can call C++
can reuse.

## Features (Linux client)

- **Lists**: colours, icons, sections, manual order (drag or Alt+↑/↓), and subtasks one level deep (indent with Ctrl+]).
- **Reminders**: title, notes, URL, due date and time, repeat ("every 2 weeks", weekdays, …), flag, priority, tags.
- **Smart lists**: Today, Scheduled, All, Flagged, Completed, plus one per tag. Search across everything, and a Ctrl+K "Go to" switcher.
- **Quick entry**: type `Pay rent #home 📅 2026-10-31 🚩` into "New Reminder" and the fields are filled in.
- **Undo and redo** (Ctrl+Z / Ctrl+Shift+Z) for every change, including moves and deleted lists.
- **Keyboard-driven**: nearly everything has a shortcut; see the [user guide](docs/USING.md#keyboard-shortcuts).
- **Live sync**: changes from other devices appear within half a second. Syncthing conflict copies are merged automatically, reminder by reminder.
- **Hand-editing friendly**:
  - Lines you write by hand keep their exact formatting until you change them in the app.
  - Markdown files with checklists can be turned into lists.
  - Ordinary notes in the same folder are left alone.
- **Notifications** when reminders come due, while the app is running.

## How syncing works

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

| Distribution | Packages |
|---|---|
| Fedora 44 (tested) | `sudo dnf install gcc-c++ cmake ninja-build gtk4-devel libadwaita-devel glib2-devel` |
| Debian / Ubuntu (untested; needs a release with GTK 4.20 and libadwaita 1.8) | `sudo apt install g++ cmake ninja-build libgtk-4-dev libadwaita-1-dev libglib2.0-dev-bin` |

The core library on its own needs only a C++23 compiler and CMake. Build it
with `-DBUILD_GNOME_APP=OFF`.

### Build, test, run

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/core/core_tests          # 66 tests for the core library
./build/linux/reminders          # or: ./build/linux/reminders ~/Sync/Reminders
```

### Install

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build
cmake --install build
```

This installs the `reminders` command, the desktop entry
(`com.stephenhouser.Reminders.desktop`) and the app icon.

## Using it

1. Set up a Syncthing folder (or use any folder; syncing is optional).
2. Start Reminders and choose that folder.
3. Make lists with **New List** (Ctrl+Shift+N), and add reminders by typing into **New Reminder**.

The **[user guide](docs/USING.md)** covers everything else: lists,
sections, subtasks, repeats, smart lists, keyboard shortcuts, editing files by
hand, and what to do about sync problems.

## Project layout

```
docs/
  FORMAT.md            The on-disk format: the contract every client implements
  USING.md             User guide for the Linux client
core/                  Platform-neutral C++23 library (standard library only)
  include/reminders/   model, format, merge, recurrence, store, history, syncthing
  src/
  tests/               Unit tests with a tiny built-in harness (no dependencies)
linux/                 The GNOME client
  src/                 main, window, dialogs, support, gtk_util (RAII + signal helpers)
  data/                style.css, icons, .desktop file, GResource manifest
INSTRUCTIONS.md        The original brief, and how to recreate this project
```

### The core library

| Header | Purpose |
|---|---|
| `model.hpp` | `Document` (front matter + blocks), `Reminder`, sections, ids. Editing operations: insert, move, indent/outdent, sections |
| `format.hpp` | Parse and serialize list files, with byte-for-byte round trips |
| `merge.hpp` | Three-way (or two-way) merge of a Syncthing conflict copy |
| `recurrence.hpp` | Next date for repeat rules |
| `store.hpp` | The folder: loading, saving, conflict handling, list detection, smart-list queries |
| `history.hpp` | Undo/redo as before/after snapshots of list files, merging around changes from other devices |
| `syncthing.hpp` | Per-device state location and the `.stignore` entry |

### The GNOME client

It uses the GTK and libadwaita **C APIs** directly from C++, with small helpers
in `gtk_util.hpp`: an owning `Obj<T>` for GObjects, `connect<Sig>()` to attach
lambdas to signals, actions, timeouts and shortcuts. The UI is rebuilt from the
store after every change, which keeps the code simple. Lists are small.

## Development notes

- **Screenshots without screen capture:** GNOME locks down screen capture on
  Wayland, so the app can render its own window:
  `REMINDERS_SCREENSHOT=out.png ./build/linux/reminders` saves a PNG after
  1.5 s and quits.
- **Testing without touching your real session:** the app is single-instance,
  so a test launch can be handed to your running copy. Isolate test runs:
  ```sh
  dbus-run-session -- env XDG_CONFIG_HOME=/tmp/r/config ./build/linux/reminders /tmp/r/lists
  ```
- **Driving the app from scripts:** actions are exported over D-Bus, e.g.
  `gdbus call --session --dest com.stephenhouser.Reminders --object-path /com/stephenhouser/Reminders/window/1 --method org.gtk.Actions.Activate go-to '[]' '{}'`.
- **Memory checks:** the core tests run clean under `valgrind`, and with
  `-D_GLIBCXX_DEBUG`.

## License

Not chosen yet.
