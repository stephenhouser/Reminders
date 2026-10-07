# GNOME client — GTK 4 / libadwaita notes

GTK and libadwaita are used through their **C APIs only** — no gtkmm — wrapped
in the small RAII helpers in `gtk_util.hpp`: `Obj<T>`, `connect<Sig>()`,
actions, toggles, timeouts, idle callbacks, shortcuts, and `attach()` to tie a
C++ object's lifetime to a widget.

The user guide is `docs/USING.md`; the layout follows the GNOME HIG
(navigation sidebar, boxed lists, list colour as accent, round checkboxes).

**A new or changed key** touches: its accelerator (`accel()` in `main.cpp`,
or the row's key handler in `reminder_row.cpp`), the menu item showing it,
the shortcuts dialog (`show_shortcuts()` in `main.cpp`), `docs/KEYS.md`, and
`docs/USING.md` where it's described. No Alt keys and no Ctrl+number jumps
(the HIG gives those to other uses). Check the terminal client's
equivalent too.

## Pitfalls met here, each one already paid for

- **Arrays passed to C functions must end in `nullptr`.**
  `gtk_string_list_new()` read past a priority list that didn't, and crashed
  the details dialog.
- **C++ needs casts C doesn't.** `adw_*_new()` returns `AdwDialog*` or
  `AdwNavigationPage*` — wrap in `GTK_WIDGET()`.
- **A signal handler's C signature must match the signal.** `on()` is only for
  signals that pass just the emitter ("clicked", "closed", …). `notify::…` also
  passes a GParamSpec, so with `on()` the closure pointer is read from the
  wrong argument and the app segfaults — Ctrl+B did, through
  `notify::show-sidebar`. Use `connect<void(GObject*, GParamSpec*)>`.
- **Drag payloads must not be plain strings** — text fields accept them and
  insert the raw id.
- **Drag handlers must not keep raw widget pointers.** "drag-end" fires after
  the drop's rebuild has destroyed the row.
- **Context menus need a proper popover host.** Parented to a `GtkListBox` that
  gets rebuilt, `gtk_list_box_remove_all()` tries to remove the popover as a row
  ("Tried to remove non-child") and the move fails. Parented to a plain widget,
  the popover keeps the size it had when it opened while its menu items arrive
  just after, so it comes out clipped with scrollbars. Host it in a
  `GtkMenuButton`, which re-sizes its popover: the sidebar keeps an invisible
  one in a `GtkOverlay` corner and points its popover at the click.
- **`GtkEditableLabel` never wraps by default.** Long titles widened the window
  ("AdwToastOverlay exceeds AdwApplicationWindow width").
- **`GtkSearchEntry` consumes Esc** — connect "stop-search".
- **`AdwNavigationSplitView` can't hide its sidebar on wide windows.** Use
  `AdwOverlaySplitView`.

## Checking UI work

Launch only through `tools/gui-test.sh` (see the root `CLAUDE.md`).
`REMINDERS_SCREENSHOT=out.png` renders the window and quits, 1.5 s after
start-up or after `REMINDERS_SCREENSHOT_DELAY` ms. Actions without input:
`gdbus` on the private session bus.

**Real keys and clicks:** `STEPS=steps.txt tools/gui-test.sh HOME LISTS`
runs `tools/gui-keys.py`, which types and clicks into the app through
mutter's RemoteDesktop API on the private bus: the same path a person's
input takes, shortcuts, key handlers and popover grabs included. The step
syntax and the pointer's starting place are in the script. Check results in
the list files (undo and paste-back make good checks), or with a screenshot
delayed past the steps. Run a control too, one that would fail if the
change didn't work, so a passing test means something.

Not covered yet: mouse *drags* (the tool has no press-and-hold step; mutter's
API could do one, untried). Popovers and combo-row lists never show in
screenshots (separate surfaces); test them by what choosing an item does. Keep
logic in `core/` or `app/` where it can be unit-tested, and say plainly
which parts were only compiled.
