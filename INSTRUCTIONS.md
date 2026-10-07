# Instructions

## The original brief

> I want to write a todo app that is identical to the functions and style of the
> apple reminder app on ios but uses syncthing as it's backend to sync
> reminders with other clients and an eventual desktop app.
>
> i will eventually also want an android, linux, windows, and macos app that
> all sync together.
>
> the syncthing 'folder' it syncs to should be easily viewable and editable
> with text (or maybe markdown) files within it.

### Requested keyboard shortcuts

```
New Reminder (or New Item)   Control-N
New List                     Shift-Control-N
Show all subtasks            control-e
hide all subtasks            shift-control-e
hide/show sidebar            control-b
hide/show completed          control-h
delete                       delete
set as due today             control-t
set as due tomorrow          shift-control-t
```

---
## What this file is

The design record: the decisions behind the project and why they were taken,
plus a short state of play. It is not a to-do list and not a changelog.

- **What to do next** → `TODO.md`, which owns that list outright.
- **How to work in this repo** (build, test, format, the mandates, isolated
  test launches) → `CLAUDE.md`, plus one per client in `clients/<name>/`.
- **What the apps do** → `README.md`, `docs/USING.md`, `docs/TERMINAL.md`,
  and every key in both, side by side, in `docs/KEYS.md`.
- **The file format** → `docs/FORMAT.md`, the contract every client implements.
- **Each back end** → `backends/<id>/README.md`.
- **How to build it again from scratch** → `docs/rebuild-prompt.md`, a summary
  prompt and build order (brought up to date 2026-10-07; the code wins where
  they differ).
- **What changed when** → `git log`.

## Decisions already made

These were settled while building the first version. Reopen one only with a
reason that's new.

| Topic | Decision | Why |
|---|---|---|
| Storage | One Markdown file per list, Obsidian Tasks emoji syntax, `^id` per reminder | Easy to read and edit by hand; works in Obsidian. This, not any sync method, is what the project is built on |
| List marker | Front matter `reminders: 1` (format version) | So the folder can hold other notes |
| Sections | `## Heading` lines | Plain Markdown |
| Sync | The app only reads and writes files; a back end moves them. Concurrent edits are merged three-way | No server, no account of its own |
| Back ends | A module each under `backends/<id>/`, string ids in a registry (no enum), one build option per back end | Any of them can be added or left out; none is load-bearing |
| Clients | Native per platform: SwiftUI (iOS, macOS), Kotlin (Android), C++/GTK (Linux), Windows later | Each app should look and feel native |
| First client | Linux, GNOME | Development machine is Fedora |
| Language | C++23 + CMake | The author knows C++ best |
| GNOME toolkit | GTK 4 + libadwaita through their **C APIs**, wrapped in small home-made RAII helpers; no gtkmm | libadwaita has no C++ binding; one consistent API style |
| Terminal client | Separate binary from the GUI, no GTK dependency; ncurses for the TUI | Works over SSH and on headless machines |
| Look | GNOME HIG: navigation sidebar, boxed lists, list colour as accent, round checkboxes | Native on GNOME |
| Per-device state | A back end that sets `state_in_folder` (only Syncthing) keeps it in `<folder>/.reminders/<device>/`, excluded via `(?d).reminders` in the Syncthing root's `.stignore` (user's choice, kept 2026-10-03); every other back end uses `$XDG_STATE_HOME/reminders/<device>/<source>/` | A synced folder's state belongs with that folder; device in the path because folders (and home, over NFS) are shared |
| Paths | XDG config / data / state / cache dirs; `folder=` accepts `~`, `$HOME`, `${VAR}`, home-relative; saved as `~/…` | User's request 2026-10-03 |
| XDG locations (mandate) | Always through the `$XDG_*_HOME` variables, never a hard-coded `~/.config` or `~/.local/share`; docs name the variable | User's mandate 2026-10-03. Procedure in `CLAUDE.md` |
| C/C++ formatting (mandate) | The user's own `~/.clang-format`; every changed file reformatted before finishing | User's mandate 2026-10-04. Procedure in `CLAUDE.md`; tag `before-clang-format` marks the tree before |
| GUI tests (mandate) | Only through `tools/gui-test.sh`, never on the user's display or session bus | User's rule 2026-10-01, a script since 2026-10-04, after test windows twice opened on their screen. Procedure in `CLAUDE.md` |
| Server back ends | libcurl (not libsoup), `password-command=` (keyring maybe later), tests against a fake Python server, sync on open + `interval=` timer + shortly after edits | User's choices 2026-10-02, kept for WebDAV 2026-10-03 |
| iOS sync (later) | Syncthing embedded via gomobile, not the Möbius Sync app | Self-contained app |
| Names | GNOME app `Reminders`, terminal client `reminders` (CLI + TUI in one binary; not `rem`, too close to DOS `rem`), app ID `com.stephenhouser.Reminders` | Case-sensitive file names on Linux let both live in one `bin` |
| Migrations | None. This is the first version | |

## How the pieces fit

Lists come from *sources*. A source is a folder plus the back end that keeps it
in sync, configured in a `[source.NAME]` section; several are open at once.

- `core` holds the model, the format, three-way merge, undo history, settings,
  XDG paths, import/export, and `Library` — a `Store` per source, lists keyed
  `source/name`, ids unique across sources, edits and smart lists mirrored
  across them, undo spanning them through `ListTexts`.
- `app` is the toolkit-free layer both clients share: Actions, Selection,
  SidebarModel, ViewModel, preferences, `import_file`.
- Each back end is a module registered by string id. `local` watches its folder
  for outside edits; `syncthing` merges the conflict copies Syncthing leaves;
  `caldav` and `webdav` keep a local Markdown copy in step with a server under
  a write lock, merging three-way against the last-synced base and retrying on
  a 412; `git` commits, pulls and pushes. Each one's protocol, settings and
  on-disk records are in `backends/<id>/README.md`.
- Both clients drive a `SyncRunner` on a worker thread: on open, on the
  `interval=` timer, and shortly after an edit.

Two staged refactors shaped this, both finished: **sources and back ends**
(stages 1–4, 2026-10-02/03: one back end per source, then `Library`, then both
apps onto it, then the server back ends) and the **modular refactor** (five
stages, 2026-10-04: the big UI files split by area, the shared `app` layer
extracted and unit-tested, `net/` split into `backends/<id>/`, app preferences
split from core settings). The plans aren't reproduced here — the code is the
result and `git log` is the record. Tags to diff from:

| Tag | The state just before |
|---|---|
| `single-source` | sources — one folder, one back end |
| `before-modular-refactor` | the modular refactor (so, the end of the sources work) |
| `before-stage-2` … `before-stage-5` | each later stage of that refactor |
| `before-clang-format` | the whole tree being reformatted |

## State of play (2026-10-07)

- **Built and working:** the core library and format with byte-for-byte round
  trips; the GNOME app and the terminal client (CLI and TUI) with the brief's
  features and shortcuts, multiple sources, multi-select, drag and drop,
  import/export (Markdown, text, ics, todo.txt, CSV); all five back ends.
  Tests green: core 106, app 15, back ends 51 including 10 against the fake DAV
  server.
- **Not verified against the real thing:** CalDAV and WebDAV have only been run
  against the fake Python server. Git has been tried against a hosted
  repository and works. Add Source for CalDAV and WebDAV still wants a run by
  hand.
- **Only the Linux clients exist.** Other platforms are in `TODO.md`.
- **Known gaps** are listed in `TODO.md`, with the reasons here where they
  aren't obvious: CalDAV can't see reordering done in other clients because
  the order lives in `X-APPLE-SORT-ORDER` and local order wins the merge;
  RRULEs beyond `docs/FORMAT.md`'s rules are kept but not shown; a notes line
  written `- [ ] …` reads back as a subtask because the format has no escape
  for it (combining pasted text writes `☐ …` instead to dodge it).
- **Installing:** `cmake --install` puts both executables, the `.desktop`
  file and the hicolor icon under the prefix (see `README.md`).
