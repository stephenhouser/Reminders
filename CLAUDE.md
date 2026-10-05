# Reminders — working notes for Claude

A to-do app whose data is a folder of plain Markdown files — one `.md` per
list, readable and editable in any text editor. No account, no server, no
database. C++23 + CMake, with native clients per platform; the Linux GNOME one
comes first.

Several *sources* can be open at once, each with a *back end* that says how its
lists are kept in sync: a local folder, CalDAV, WebDAV, git or Syncthing. No
one of these is the basis of the project — the Markdown files are, and a back
end is a module that can be added or left out.

Design history, decisions and "Where things stand": `INSTRUCTIONS.md`.
The short to-do list: `TODO.md`. The file format: `docs/FORMAT.md`.

Apple Reminders on iOS is the model for the features and the feel, and its
keyboard shortcuts are in the brief at the top of `INSTRUCTIONS.md`. It's the
reference, not the architecture.

## Layout

- `core/` — the library: model, file format, merge, history, settings, XDG
  paths, importer/exporter, `Library` (several sources open at once).
- `app/` — the layer the GUI and the terminal client share (preferences,
  `import_file`), kept apart from core settings.
- `backends/<id>/` — one module per source type: `local`, `syncthing`,
  `caldav`, `webdav`, `git`, plus `common/`.
- `linux/` — the GNOME app (`build/linux/Reminders`), GTK4 + libadwaita.
- `cli/` — the terminal client (`build/cli/reminders`): CLI and TUI.

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
- **`TODO.md` mirrors the to-dos**: when an item, known gap or hand-check
  changes, update `INSTRUCTIONS.md` (the detail) and `TODO.md` (one terse
  line) together. No dates, explanations or test history in `TODO.md`.
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
  Never set `WAYLAND_DISPLAY=wayland-0` or any name not made for the test.
  `REMINDERS_SCREENSHOT=out.png` renders the window and quits.
  Actions without input: `gdbus call --session --dest
  com.stephenhouser.Reminders --object-path
  /com/stephenhouser/Reminders/window/1 --method org.gtk.Actions.Activate`.
- **TUI:** a private tmux server, `tmux -L reminders-test -f /dev/null`.
  Leave `TERM` alone — under `screen-256color` the Alt+arrow codes aren't
  recognised and the keys silently do nothing. `send-keys -l` for literal
  text, `M-Up` for Alt keys.
- Test settings files need a `[general]` header line; keys outside it are
  ignored. A `--folder` other than the saved one ignores the saved `view`.
- If a launch returns in ~30 ms it was handed to an existing instance: stop
  and check `pgrep -af build/linux/Reminders`.
