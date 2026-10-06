# Reminders — working notes for Claude

A to-do app whose data is a folder of plain Markdown files — one `.md` per
list, readable and editable in any text editor. No account, no server, no
database. C++23 + CMake, with native clients per platform; the Linux GNOME one
comes first.

Several *sources* can be open at once, each with a *back end* that says how its
lists are kept in sync: a local folder, CalDAV, WebDAV, git or Syncthing. No
one of these is the basis of the project — the Markdown files are, and a back
end is a module that can be added or left out.

Decisions and why, and a short state of play: `INSTRUCTIONS.md`. The to-do
list: `TODO.md`. The format: `docs/FORMAT.md`. Each back end:
`backends/<id>/README.md`. Each client has its own notes, loaded when you
work there: `clients/gnome/CLAUDE.md` (GTK) and `clients/terminal/CLAUDE.md`
(CLI and ncurses).

Apple Reminders on iOS is the model for the features and the feel, and its
keyboard shortcuts are in the brief at the top of `INSTRUCTIONS.md`. It's the
reference, not the architecture.

## Layout

- `core/` — the library: model, file format, merge, history, settings, XDG
  paths, importer/exporter, `Library` (several sources open at once).
- `app/` — the toolkit-free layer every client shares (preferences,
  `import_file`), kept apart from core settings.
- `backends/<id>/` — one module per source type: `local`, `syncthing`,
  `caldav`, `webdav`, `git`, plus `common/`.
- `clients/<name>/` — one directory per client, named for the desktop or
  environment it targets, not the OS:
  - `gnome/` — the GNOME app, GTK 4 + libadwaita.
  - `terminal/` — the terminal client: CLI and TUI in one binary.

  Every client's executable builds into `build/bin/` (`REMINDERS_BIN_DIR`), so
  it's `build/bin/Reminders` and `build/bin/reminders` whatever the source
  layout. A new client sets `RUNTIME_OUTPUT_DIRECTORY ${REMINDERS_BIN_DIR}`
  the same way. Test executables stay where they were (`build/core/`,
  `build/app/`, `build/backends/`).

Public headers sit under `<dir>/include/reminders/` and are included as
`<reminders/paths.hpp>`; everything is in `namespace rem`.

GTK and libadwaita are used through their **C APIs only** — no gtkmm — wrapped
in small home-made RAII helpers.

## Build, test, format

```
cmake -B build && cmake --build build -j      # configure and build
ctest --test-dir build --output-on-failure    # 8 suites: core, app, backends
cmake --build build --target format           # clang-format -i, two passes
cmake --build build --target format-check     # report only, changes nothing
```

## Rules

- **Formatting.** All C/C++ follows the user's own `~/.clang-format` (Google
  base, tabs, width 4, `ColumnLimit 0`, `InsertBraces`). After any C/C++ edit
  — including one written by a sed or Python patch script, which writes spaces
  — run `clang-format -i --style=file` on the changed files until a pass
  changes nothing, then build and test. Brace every `if`/`for`/`while` body.
  Re-read `~/.clang-format` rather than trusting this summary; the user may
  change it.
- **XDG paths.** Always resolve through `$XDG_CONFIG_HOME`,
  `$XDG_DATA_HOME`, `$XDG_STATE_HOME`, `$XDG_CACHE_HOME`. In code use
  `core/include/reminders/paths.hpp` (`config_dir()`, `data_dir()`,
  `state_dir()`, `cache_dir()`, and `expand_path()` for a path that came from
  settings or the command line); never a literal `~/.config` or
  `~/.local/share`. In docs, comments, UI text
  and messages to the user, write `$XDG_DATA_HOME/reminders/…`, giving
  `~/.local/share/…` only as "when `XDG_DATA_HOME` isn't set".
- **Per-device state** comes from `source_state_dir()` in `sources.hpp` — call
  it rather than composing a path. A back end that sets `state_in_folder`
  (today only Syncthing, since a synced folder's state belongs with that
  folder) keeps it at `<folder>/.reminders/<device>/`, with `(?d).reminders` in
  the root `.stignore`; every other back end uses
  `$XDG_STATE_HOME/reminders/<device>/<source>/`. Don't move in-folder state
  out in a later cleanup — that was tried and reverted.
- **Never install anything** — system packages, toolchains, language deps —
  without asking first and saying what it's for. `command -v` checks are fine.
  The dev machine has no Swift or Xcode, so iOS code can't be built here.
- **One home each.** `TODO.md` owns the to-do list — features, known gaps,
  what needs trying by hand — one terse line per item, no dates, explanations
  or test history. `INSTRUCTIONS.md` is the design record (decisions and why,
  a short state of play) and does not track to-dos. Don't write an item into
  both.
- **Settled, don't re-open:** one Markdown file per list; native per platform
  (not Flutter); libcurl + libxml2 for CalDAV and WebDAV; `password-command=`
  for passwords; fake Python DAV servers in the tests; on iOS, Syncthing's Go
  core embedded rather than an external app.

## Testing the apps by hand

Never launch a test instance on the user's desktop or session bus — the app is
single-instance, so a stray launch is forwarded to the window they have open
and overwrites their saved view.

- **GUI:** always `tools/gui-test.sh HOME_DIR [APP_ARGS…]` — private D-Bus
  session, headless mutter on a private Wayland socket, no X11 fallback.
  Never set `WAYLAND_DISPLAY=wayland-0` or any name not made for the test,
  for the app *or* the bus (services it starts inherit the bus's display).
  `REMINDERS_SCREENSHOT=out.png` renders the window and quits.
  Real key presses and clicks: `STEPS=file` (`tools/gui-keys.py`; see
  `clients/gnome/CLAUDE.md`).
  Actions without input: `gdbus call --session --dest
  com.stephenhouser.Reminders --object-path
  /com/stephenhouser/Reminders/window/1 --method org.gtk.Actions.Activate`.
- **TUI:** a private tmux server, `tmux -L reminders-test` — recipe in
  `clients/terminal/CLAUDE.md`.
- Test settings files need a `[general]` header line; keys outside it are
  ignored. A `--folder` other than the saved one ignores the saved `view`.
- If a launch returns in ~30 ms it was handed to an existing instance: stop
  and check `pgrep -af build/bin/Reminders`.

## Gotchas already paid for

- **`.stignore` is only read at the Syncthing folder root**, so find the root by
  looking for `.stfolder`.
- **Recognising your own writes after a restart needs the persisted
  fingerprint.** Without it, unsynced local edits become the merge base and the
  other device's changes are lost at the next conflict.
- **A notes line written `- [ ] …` reads back as a subtask** — the format has no
  escape. Text combined from a paste is written `☐ …` to dodge it.

Client-specific ones are in each `clients/<name>/CLAUDE.md`.
