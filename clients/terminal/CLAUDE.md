# Terminal client — CLI and TUI notes

One binary, `reminders`: with a command it's the CLI, with none it's the
ncurses TUI. No GTK dependency, so it runs over SSH and on headless machines.
User guide: `docs/TERMINAL.md`.

- `src/cli/` — the CLI: `App` with a `cmd_*` method per command, dispatched
  in `App::run` (`app.cpp`). `--help` is `usage()`: `kUsage`, then each
  command's `help` (else `summary`) from the table, wrapped, then `kUsageEnd`.
- `src/commands.hpp` — the command table (`cmd::commands()`, in `app.cpp`):
  names, usage, `edits`, where each runs, what its words complete to; and
  `cmd::run`, which the TUI's `:` prompt (`tui/command.cpp`) calls on its open
  library with `Hooks` for questions, `$EDITOR` and the selection.
- `src/tui/` — the TUI: `tui.cpp` (setup, event loop), `keys.cpp`
  (`handle_key`), `help.cpp` (the `?` box), `marks.cpp` (`v` / `*` marks),
  `sidebar.cpp`, `items.cpp`, `editing.cpp`.
- `src/text.hpp` — colours, due labels and priority marks shared by both, with
  the same palette as the GNOME app.

## Adding things touches more than one place

- **A CLI command:** its entry in `cmd::commands()` and its branch in
  `App::run` (both `app.cpp`), `docs/TERMINAL.md`. **Set `edits`
  if it changes any list:** that decides whether the CLI pushes to
  CalDAV/WebDAV/git sources afterwards and whether `:` makes it an undo step.
  Write to `out_`, never `std::cout`, and ask through `hooks_` (`confirm`,
  `choose`, `edit`; not stdin), or it breaks under the TUI. The `:` prompt gets it for free.
- **A `:` command only the TUI has:** an entry with `Where::Tui`, handled in
  `Tui::run_command` (`tui/command.cpp`), and the `:` table in
  `docs/TERMINAL.md`.
- **A TUI key:** `handle_key` in `keys.cpp`, the entries in `help.cpp`, the key
  table in `docs/TERMINAL.md`. If it should also work on marked reminders,
  `act_on_marked` in `marks.cpp`. Check for a collision first — `s` / `S` sync,
  `,` is settings, `O` imports, `e` edits, `:` is the command line; `i` and
  `I` are unbound. No Alt keys, as in the GNOME app.
- **Anything the GNOME app also does:** match its rules rather than inventing
  new ones — complete and flag act on all alike, move and delete take the
  outermost reminders, every multi-reminder change goes through `batch()` so
  it's one undo step and one write per list.

## ncurses: what the setup depends on

- `setlocale(LC_ALL, "")` before `initscr()`, then wide characters
  throughout: `get_wch`, `WACS_*` box drawing. Plain `getch` / `ACS_*` break on
  non-ASCII titles.
- **Ctrl+S / Ctrl+Q are XOFF / XON** in a terminal — Ctrl+S froze the TUI the
  first time. `run()` clears `IXON` so Ctrl+Q can quit; ncurses restores it on
  exit. Ctrl+S is no key of the TUI's: bound to anything, it's lost wherever
  flow control is still on.
- **Alt+key arrives as Esc then the key.** `set_escdelay(25)` and a
  non-blocking second `get_wch` tell the two apart; a bare Esc is one with
  nothing after it.
- **Modified arrows aren't keypad codes.** Ctrl+↑ etc. are looked up by
  terminfo capability (`kUP5`, `kDN6`, `kNXT5`, …) through
  `tigetstr` + `key_defined`, so they depend on `TERM` being right.
- `timeout(1000)` makes the loop wake every second to poll sync and the folder,
  even with no key pressed.

## Testing

On a private tmux server, never the user's:

```
tmux -L reminders-test -f /dev/null new-session -d -x 90 -y 24 \
  "env HOME=<scratch>/home XDG_CONFIG_HOME= build/bin/reminders --folder <dir>; sleep 3"
```

- **Leave `TERM` alone.** Under `screen-256color` the `kUP5` family isn't
  defined, so Ctrl+arrows silently do nothing (`tmux-256color` has them).
- `send-keys -l` for literal text (otherwise "Home" is the Home key); `C-Up`,
  `C-S-Up` for modified keys; `capture-pane -p` to read the screen.
- The CLI needs no terminal — test it straight from the shell, with
  `--json` for output that's easy to check.
