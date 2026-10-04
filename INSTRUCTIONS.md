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

## Recreating the project from scratch

### Decisions already made

These were settled while building the first version. A rebuild should start
from them rather than reopen them.

| Topic | Decision | Why |
|---|---|---|
| Clients | Native per platform: SwiftUI (iOS, macOS), Kotlin (Android), C++/GTK (Linux), Windows later | Each app should look and feel native; Apple Reminders' look is only for the iOS app |
| First client | Linux, GNOME | Development machine is Fedora |
| Language | C++23 + CMake | The author knows C++ best |
| GNOME toolkit | GTK 4 + libadwaita through their **C APIs**, wrapped in small home-made RAII helpers; no gtkmm | libadwaita has no C++ binding; one consistent API style |
| Storage | One Markdown file per list, Obsidian Tasks emoji syntax, `^id` per reminder | Easy to read and edit by hand; works in Obsidian |
| List marker | Front matter `reminders: 1` (format version) | So the folder can hold other notes |
| Sections | `## Heading` lines | Plain Markdown |
| Sync | The app only reads and writes files; Syncthing does the syncing. Conflict copies are merged three-way | No server, no account |
| Per-device state | Syncthing sources: `<folder>/.reminders/<device>/`, excluded via `(?d).reminders` in the Syncthing root's `.stignore` (user's choice, kept 2026-10-03). Local and CalDAV sources: `$XDG_STATE_HOME/reminders/<device>/<source>/` | A Syncthing list's state stays with its folder; device in the path because folders (and home, over NFS) are shared |
| Paths | XDG config / data / state / cache dirs (core paths.hpp); `folder=` accepts `~`, `$HOME`, `${VAR}`, home-relative; saved as `~/…` | User's request 2026-10-03 |
| XDG locations (mandate) | Always resolve them through `$XDG_CONFIG_HOME`, `$XDG_DATA_HOME`, `$XDG_STATE_HOME`, `$XDG_CACHE_HOME` (core `config_dir()` / `data_dir()` / `state_dir()` / `cache_dir()`), never a hard-coded `~/.config`, `~/.local/share`, `~/.local/state` or `~/.cache`. Docs and comments name the variable and give the default only as "when it isn't set" | User's mandate 2026-10-03 |
| iOS sync (later) | Syncthing embedded via gomobile, not the Möbius Sync app | Self-contained app |
| Names | GNOME app `Reminders`, terminal client `reminders` (CLI + TUI in one binary; not `rem`, too close to DOS `rem`), app ID `com.stephenhouser.Reminders`, config `$XDG_CONFIG_HOME/reminders/` | Case-sensitive file names on Linux let both live in one `bin` |
| Terminal client | Separate binary from the GUI, no GTK dependency; ncurses for the TUI | Works over SSH and on headless Syncthing machines |
| Look | GNOME HIG: navigation sidebar, boxed lists, list colour as accent, round checkboxes | Native on GNOME |
| Migrations | None. This is the first version | |

### The prompt

Give this to a coding agent (such as Claude Code) in an empty directory on a
Fedora machine. It describes the finished result; the directions below give
the order to build it in.

````text
Build "Reminders": a to-do app modelled on Apple Reminders, whose data is a
folder of Markdown files synced between devices by Syncthing. Start with the
Linux (GNOME) client; iOS, Android, macOS and Windows clients come later and
must read the same folder, so the file format is the contract.

Ground rules
- Don't install anything (packages, toolchains, libraries) without asking me.
  Checking what's installed is fine.
- C++23, CMake + Ninja. GTK 4 and libadwaita through their C APIs only (no
  gtkmm); write small RAII/lambda helpers instead.
- The core logic is a separate library with no GTK dependency, using only the
  standard library, with its own unit tests (a tiny home-made test harness,
  no test framework).
- The app is single-instance (GApplication). When you launch it for testing,
  use `dbus-run-session`, and set XDG_CONFIG_HOME to a scratch directory, so
  you never reach my running copy or my settings.
- Add a developer hook: REMINDERS_SCREENSHOT=out.png makes the app render its
  own window to a PNG after 1.5 s and quit (screen capture is locked down on
  Wayland). Use it, plus `gdbus call ... org.gtk.Actions.Activate`, to check
  your UI work.

1. File format (write docs/FORMAT.md first; it is the cross-client contract)
- One list = one `<Name>.md` file directly in the folder, whose front matter
  contains `reminders: 1` (the key marks a list; 1 is the format version).
  Other Markdown files are ordinary notes and must be left alone.
- Front matter keys: color (red orange yellow green cyan blue indigo purple
  pink brown gray), icon (list bookmark cart gift home work school calendar
  flag star heart music game book food travel nature person people money pill
  computer camera), order (integer). Keep unknown keys, comments and nested
  YAML verbatim.
- A reminder is `- [ ] ` or `- [x] ` followed by the title and inline fields,
  ending in ` ^id` (6+ lowercase alphanumerics, unique in the folder).
  Inline fields use the Obsidian Tasks emoji syntax: #tag, ⏫ 🔼 🔽 priority
  (read 🔺 ⏬ too), 🚩 flag, 🔁 every <rule>, 📅 YYYY-MM-DD [HH:MM], ✅ done
  date, ➕ created date, 🔗 url. Ignore U+FE0F. Anything else is title.
- Subtasks: indented task lines under a reminder, one level deep. Notes:
  other indented lines; blank lines inside notes are kept when the next line
  is indented.
- Sections: `## Name` headings. Other headings and prose are kept in place.
- Round trip: an unchanged file serializes byte for byte. A hand-written line
  keeps its exact text until its fields change. Lines without ids get one in
  memory, and it's only written with the next real change to that list.
- Repeat rules: every day/weekday/weekend/week/month/year, every N
  days/weeks/months/years. Completing a repeating reminder marks it done
  (✅ today, no more 🔁) and inserts an open copy with the next date above it.
- Writes are atomic (`.<name>.md.tmp` then rename) and only happen when the
  content changed.

2. Sync and conflicts
- Watch the folder (GFileMonitor), debounce 400 ms, reload changed lists,
  ignore dotfiles and our own writes.
- Merge `<Name>.sync-conflict-*.md` copies into `<Name>.md` by reminder id
  (title when there's no id), three-way when a base is known: one-sided field
  changes win; both changed → main file wins; deleted on one side and
  unchanged on the other → deleted; edit beats delete; additions are kept
  after their nearest earlier common reminder. Without a base: union, main
  wins, nothing deleted. Merge front matter the same way. Conflict copies of
  non-list files are left alone.
- Per-device state for a Syncthing source in `<folder>/.reminders/<device>/`
  (device = hostname plus 4 hex digits of a hash of /etc/machine-id):
  base/<list>.md (the last version received from elsewhere: the merge
  base), written/<list> (a 64-bit FNV-1a fingerprint of our last write, to
  recognise our own writes after a restart), declined.txt. Move it on
  rename and remove it on delete. Find the Syncthing root (nearest ancestor
  with `.stfolder`) and append `(?d).reminders` to its `.stignore` once.
  Local and CalDAV sources keep theirs in
  `$XDG_STATE_HOME/reminders/<device>/<source>/` (open_source moves a
  local source's old `<folder>/.reminders/<device>/` there).
- Every file location comes from the XDG base directories (config, data,
  state, cache; an unset or relative variable falls back to the spec's
  default). Paths in settings accept `~`, `$HOME`, `${VAR}` and
  home-relative paths, and are written back as `~/…` under home.

3. The GNOME app
- Layout: AdwOverlaySplitView (the sidebar can be hidden at any width;
  collapses below 560sp). The sidebar has smart lists (Today, Scheduled, All, All Reminders (completed too),
  Flagged, Completed, with counts), My Lists (coloured icon badges, counts)
  and Tags, plus a search bar and a "New List" button. The content area
  shows the selected view in AdwClamp'd boxed lists at 90% of the content
  width, updated on resize.
- List view: one boxed list per section, with a heading and a ⋮ menu
  (Rename, Delete with "keep reminders" or "delete them"). A "New Reminder"
  entry row ends each section: typed inline fields are parsed. Completed
  reminders are hidden (Ctrl+H / ⋮ → Show Completed shows them); the header
  subtitle reads "6 Reminders / 3 Complete" (all, subtasks included / done;
  the "/ N Complete" part only when some are done).
  Every view's header has a count (core count_label: lists, tags and All
  Reminders "N Reminders / M Complete"; Today/Scheduled/All/Flagged "N
  Reminders"; Completed "N Completed"; search "N Results"). The TUI puts it
  on the "# Title" line, dimmed and right-aligned; if it doesn't fit beside
  the title it shortens to "6/3" (core count_short), then is left out (not
  in parentheses, which mixed views use for each reminder's list).
  show-sidebar (Ctrl+B) is saved by both apps and read at start-up; the GUI
  saves it only while the split view isn't collapsed (narrow windows hide
  the sidebar by themselves).
- Reminder row: round check button (the list colour is the accent), an
  editable title (it must wrap: find the GtkLabel inside GtkEditableLabel and
  enable wrapping), priority marks !/!!/!!!, a second line (due date, red if
  overdue; repeat icon; tags; link; list name in smart views), 2 lines of
  notes, flag icon, a subtask disclosure button with a hidden count, and
  hover-revealed ✏ details and ⋮ menu buttons (Details, Flag, Due
  Today/Tomorrow, Indent/Outdent, Delete, each showing its accelerator).
  Right-click and long-press open the same menu.
- Details dialog (AdwDialog with Cancel/Done): title, URL, notes, date
  (expander with calendar), time, repeat (presets plus "Custom: <rule>"),
  flag, priority, list (moves the reminder), tags, subtasks (add new),
  Delete. List dialog: name with validation (unique case-insensitively, no
  / \ < > : " | ? *, no leading dot), colour swatches, icon grid.
- Smart views group by date (Overdue first) or by list. Search covers titles
  and notes.
- Undo/redo: snapshot every list's file text before and after each user
  action; record the difference as a step. Undo restores the old text if
  the file is unchanged since, or else three-way merges (current, old,
  base = new) so newer changes from other devices are kept; skip lists that
  were deleted elsewhere and say so. Nested actions record one step. Not
  for "adopt checklist file as list".
- Drag and drop: rows carry the id as a private boxed GType (never plain
  text, or text fields will paste it). Drop on the top or bottom half of a
  row (taking that row's level: next to a subtask means becoming a subtask),
  on a section's New Reminder row (end of section), or on a sidebar list
  (move to it). Auto-scroll near the edges. Look up the row through
  gtk_event_controller_get_widget in drag handlers; the row is destroyed by
  the rebuild after a drop.
- A banner offers Markdown files that contain a checklist but no marker:
  "Review…" opens a dialog of switches; adopted files get the marker as their
  first front-matter key; declined files are remembered per device.
- Notifications via GNotification at the due time (09:00 for all-day), only
  while running. Clicking one runs app.show-reminder(id).
- Command line: `reminders [FOLDER]` opens FOLDER for this session only
  (resolve relative paths against the caller's directory, even when handed
  to a running instance); --version; errors exit 1.
- Remember the folder and the last view in $XDG_CONFIG_HOME/reminders/settings.ini.
  A session-only folder doesn't overwrite the saved view.
- Keyboard: Ctrl+N new reminder; Ctrl+Shift+N new list; Space complete;
  Enter/F2 edit title; Ctrl+S save / Esc cancel while editing (dialogs too;
  a GtkSearchEntry swallows Esc, so close on its "stop-search"); Ctrl+I
  details; Ctrl+Shift+F flag; Ctrl+T / Ctrl+Shift+T due today / tomorrow;
  Alt+0…3 priority; Ctrl+] / Ctrl+[ indent / outdent; Alt+↑/↓ move (skipping
  hidden completed reminders, crossing sections); Delete; ↑/↓ across
  sections; Ctrl+K "Go to" switcher (prefix > word start > substring >
  subsequence; ↑/↓ while typing; last entry searches); Ctrl+1…9, Ctrl+0
  sidebar entries 1–10; Ctrl+Page Down/Up next/previous; Ctrl+H completed;
  Ctrl+E toggles all subtasks; Ctrl+B sidebar; Ctrl+F
  search (Enter or ↓ moves into the results); Ctrl+Z / Ctrl+Shift+Z;
  Ctrl+? shortcuts dialog (AdwShortcutsDialog).
- Custom symbolic icons (cart, gift, briefcase, mortarboard, heart, book,
  banknote, pill, flag, tag) in a GResource, plus an app icon and a .desktop
  file. App ID com.stephenhouser.Reminders.

4. The terminal client: `reminders` (separate binary, no GTK; ncursesw)
- Settings and the device name live in the core library (shared with the
  GUI): $XDG_CONFIG_HOME/reminders/settings.ini, keeping other lines intact. The
  "view" setting (the list that last had focus: today | … | list:NAME |
  tag:NAME) is shared by all three: the GUI and TUI open on it and save it as
  it changes (not for searches); `reminders list` with no VIEW shows it and
  `add` without --list adds to it (else the first list); the CLI never writes
  it; a --folder other than the saved folder ignores it.
  "show-key-numbers=true" (settings file only) labels the
  first ten sidebar entries with their jump key: in the GUI a dim
  gtk_accelerator_get_label() "Ctrl+1" … "Ctrl+0" to the right of the name
  (before the count), in the TUI a "(1)Today" … "(0)…" prefix (the TUI's 0 key jumps to the 10th entry, like Ctrl+0).
  Sidebar groups: smart-lists-display, local-lists-display and tags-display are visible |
  collapsible | hidden (default visible; "collapsable" accepted). Rule: the
  top group has no heading unless collapsible; every group below it has one
  (plain "Smart Lists" / "My Lists" / "Tags"). Collapsible gives a
  heading that folds the group (GUI: click; TUI: select it, Enter/Space),
  remembered as smart-lists-/local-lists-/tags-collapsed. smart-lists picks
  which smart lists and in what order. sidebar-order (smart-lists, local-lists,
  tags; missing/misspelled groups appended) orders the groups; local-lists-display
  is visible | collapsible (never hidden). Rearranging: GUI right-click /
  long-press on a group heading → Move "Group" Up/Down and a Collapsible check
  item (visible <-> collapsible), or Alt+↑/↓ on a row. Right-click on a
  list entry pops up the ⋮ menu's items for that list without opening it (a
  "sidebar-list" action group, bound to the list's name, is inserted on the
  hidden menu button; Show Completed stays win.show-completed); smart lists
  and tags get no menu;
  TUI J/K or Alt+↑/↓ in the sidebar (core move_sidebar_group skips groups
  not showing). TUI S opens settings.ini in $EDITOR and reloads it.
  GUI main menu Settings… opens it with GtkFileLauncher (the default text
  editor), creating it with [general]; a GFileMonitor on the file reloads it
  300 ms after a change (skipped if the text is unchanged; sidebar focus kept
  across the rebuild), so the app's own writes and outside edits both apply.
  One ordering
  function per front end drives drawing, the number labels, Ctrl+1…/1…,
  Ctrl+PgUp/PgDn (over what's showing) and Go To (which also finds folded
  groups); a hidden saved view falls back to Today or the first entry. The
  TUI sidebar starts with a bold "Reminders" title and a blank line.
  Hidden entries: each sidebar entry's right-click menu has Hide (or Show,
  on an entry already hidden) (lists: Show Completed, Add Section, List Info,
  Hide, Delete List; tags: Tag Info, Move Up/Down, Hide; smart lists: Move
  Up/Down, Hide), acting on that entry without
  opening it ("sidebar-entry" action group on the hidden menu button).
  Hidden smart lists are left out of smart-lists; lists/tags go in
  lists-hidden / tags-hidden (comma-separated, quoted if a name has a comma;
  core load/save_names_setting). Main menu "Show Hidden Lists" (show-hidden)
  shows them dimmed (.hidden-entry). Hiding is per device (settings.ini) by
  choice: a list's file is synced, and you may hide Work on one machine only.
  A renamed hidden list stays hidden. Tag Info (show_tag_dialog: the list
  dialog without the name row) saves tag-color.NAME / tag-icon.NAME; "tag" was
  added to kIcons as the tags' default icon. The TUI follows all of these.
  Entry order (same keys in GUI and TUI): Alt+↑/↓ moves the selected smart
  list, list or tag within its group (TUI also J/K; on a heading, the group);
  Alt+Shift+↑/↓ moves the group (TUI: kUP4/kDN4, or Esc + KEY_SR/KEY_SF).
  Menus have Move Up/Down too. Saved per device: smart-lists, lists-order,
  tags-order (core order_lists / order_tags: named ones first; move_in_order
  skips entries not showing); a renamed list keeps its place. The TUI marks
  a hidden entry shown by show-hidden with "-" in column 0. TUI h hides the
  selected sidebar entry (the open view from the reminders pane), or shows
  a hidden one; H toggles show-hidden. (h no longer means "go to the
  sidebar"; ← and Tab do that.)
  Copy/paste (GUI): Ctrl+C on a reminder row copies it as list-file Markdown
  without ids (core clipboard.hpp: to_clipboard_text / from_clipboard_text).
  Ctrl+V is a window key controller in the bubble phase, so focused text
  fields paste normally; elsewhere checklist lines paste with all fields,
  notes and subtasks, plain text as one reminder per line. They go after the
  focused reminder (rows carry "reminder-id"), else at the end of the shown
  list, else into the first list adjusted to the smart view. One undo step;
  Store::add gives new ids to subtasks too.
  --show-key-numbers / --hide-key-numbers on either command line override it
  for that run without writing the file (and switch a running GUI).
- CLI: lists, list [VIEW] [-a], show, add, edit, done, undone, move
  (--to LIST), delete [--yes], search, new-list, folder [PATH]; global
  --folder, --json, --no-color. Reminders are referred to by NAME, never by
  an id the user has to copy: exact title > prefix > substring > all words
  in any order; open beats completed; --in LIST narrows; if still ambiguous,
  ask with a numbered list on a terminal, else list the matches (with their
  lists) and exit 1. Ids are still accepted and appear in --json.
- Output looks like the Markdown files: "# List", "## Section",
  "- [ ] Title #tag ⏫ 🚩 📅 2026-10-03 17:30", notes indented, list name in
  brackets in mixed views; no ^id, no ➕ date, no symbols like ○ ● ⚑ ⟳. Colour
  only for list names and overdue dates (on a terminal; NO_COLOR respected).
  Field options --title --list --section --parent --in --due
  (today/tomorrow/weekday/+3d/YYYY-MM-DD) --time --no-due --flag/--unflag
  --priority --tag/--untag --repeat/--no-repeat --notes --url; exit codes 0/1/2.
- TUI (no command): sidebar (plain names, lists in their colour, a blank
  line above "My Lists" and "Tags") + reminders pane in the same Markdown
  form, with the keys listed in docs/TERMINAL.md; Enter/F2 edits the title
  in place (a line editor with a cursor, shared with the bottom-line prompts;
  typed fields applied, empty title deletes, as in the GUI); e/i opens the
  reminder in $VISUAL/$EDITOR (nano, else vi) as YAML-style fields: title,
  done, due, time, repeat, priority, flagged, tags, list, section, url,
  notes (block), subtasks (Markdown lines, matched to existing ones by title
  so they keep their notes); on a bad value, ask "Edit it again, or revert
  to how it was? [E/r]" (Enter = edit again, with "# Error: …" on top; r =
  discard); no change or an emptied file cancels; saving is one undo step.
  `reminders edit NAME` with no field options does the same. Clear the
  terminal's IXON so Ctrl+S isn't swallowed as XOFF. GNOME-app shortcuts
  also work where terminals can send them (Ctrl+N/T/K/F/H/E/B, F2,
  Alt+0-3 and Alt+arrows decoded from Esc-prefixed input and the kUP3/kDN3
  key codes, Ctrl+PgUp/PgDn via kPRV5/kNXT5, Ctrl+Q/W quit); Ctrl+Z stays
  "suspend"; Tab switches panes (so no Ctrl+I, which is the same key); TUI letter keys: n new reminder,
  N new list, x/Space done, ]/[ indent/outdent, u/r undo/redo, Delete deletes; undo (u) via the same History class; poll the folder every
  second and reload changed lists; Unicode box drawing (WACS_*), wide-char
  input (get_wch).

5. Docs: README.md (overview, build, layout), docs/USING.md (GNOME guide),
docs/TERMINAL.md (terminal guide), docs/FORMAT.md (the format), and keep this
file current.
````

### Directions

Build in this order, and check each stage before going on.

1. **Check the machine and ask before installing.** Needed: a C++23 compiler,
   CMake, Ninja, `gtk4-devel`, `libadwaita-devel`, `glib2-devel` and
   `ncurses-devel`.
2. **Write `docs/FORMAT.md` first.** Every later client depends on it.
3. **Core library and tests:** model → format (parse/serialize) → recurrence
   → merge → store → history → syncthing. Aim for byte-for-byte round trips.
   Also run the tests under `valgrind` and with `-D_GLIBCXX_DEBUG`.
4. **GTK helpers (`gtk_util.hpp`):** `Obj<T>`, `connect<Sig>()`, actions,
   toggles, timeouts, idle callbacks, shortcuts, `attach()` to tie a C++
   object's lifetime to a widget.
5. **App skeleton:** main, the welcome page, the folder chooser, the
   screenshot hook. Check it with a screenshot.
6. **Views:** the sidebar, list view, smart views, rows, then the dialogs.
   Screenshot each one with sample data.
7. **Editing:** add, edit, complete, delete, details, then undo/redo,
   wrapping every change so it records exactly one step.
8. **Drag and drop, keyboard shortcuts, sections, the quick switcher, search
   focus.**
9. **Sync:** watching, reloading, conflicts, the per-device state,
   `.stignore`. Test with a scratch folder containing `.stfolder`, and fake
   Syncthing by renaming files and writing conflict copies by hand.
10. **Terminal client:** the CLI first (quick to test from a shell), then the
    TUI, tested in a private tmux server (`tmux -L test …`, `send-keys`,
    `capture-pane`).
11. **Docs.**

**Verifying UI work:** run the app on a private session bus, open dialogs and
switch views through D-Bus actions, and check the rendered PNG. Keystrokes and
mouse drags can't be scripted this way, so put the logic behind them in the
core library, where it can be unit-tested, and say plainly which parts were
only compiled.

### Pitfalls met the first time

- **Arrays passed to C functions must end in `nullptr`.**
  `gtk_string_list_new()` read past a priority list that didn't, and crashed
  the details dialog.
- **C++ needs explicit casts that C doesn't.** `adw_*_new()` returns
  `AdwDialog*` or `AdwNavigationPage*`, so wrap these in `GTK_WIDGET()`.
- **Drag payloads must not be plain strings.** Text fields accept them and
  insert the id.
- **Drag handlers must not keep raw widget pointers.** "drag-end" fires after
  the drop's rebuild has destroyed the row.
- **`GtkEditableLabel` never wraps by default.** Long titles widened the window
  ("AdwToastOverlay exceeds AdwApplicationWindow width").
- **`GtkSearchEntry` consumes Esc**, so connect "stop-search".
- **`AdwNavigationSplitView` can't hide its sidebar on wide windows.** Use
  `AdwOverlaySplitView`.
- **A single-instance app hands test launches to the user's running copy.**
  Always use `dbus-run-session`.
- **`.stignore` is only read at the Syncthing folder root,** so find it via
  `.stfolder`.
- **Ctrl+S in a terminal is XOFF** (pause output) unless IXON is cleared;
  it froze the TUI the first time.
- **Recognising your own writes after a restart needs persistent state** (the
  fingerprint), or unsynced local edits become the merge base and the other
  device's changes get lost in the next conflict.
- **Context menus need a proper popover host.** Parented to a `GtkListBox`
  that gets rebuilt, `gtk_list_box_remove_all()` tries to remove the popover
  as a row ("Tried to remove non-child") and the move fails. Parented to a
  plain widget, the popover keeps its size from when it opened, while its
  menu items arrive just after, so it comes out clipped with scrollbars.
  Host it in a `GtkMenuButton`, which re-sizes its popover: the sidebar has
  an invisible one in a `GtkOverlay` corner, and right-click points its
  popover at the click.
- **A signal handler's C signature must match the signal.** `on()` is only
  for signals that pass just the emitter ("clicked", "closed", …).
  "notify::…" also passes the GParamSpec, so with `on()` the closure
  pointer is read from the wrong argument and the app segfaults (Ctrl+B did,
  through notify::show-sidebar). Use `connect<void(GObject*, GParamSpec*)>`.
- **settings.ini keys outside `[general]` are silently ignored.** A
  hand-written file without the section header has no effect; the TUI's `S`
  creates the file with the header.
- **Testing pitfalls:**
  - **tmux:** keep tmux's own TERM. Forcing `TERM=screen-256color` loses the
    extended key names (kUP3 …), so Alt+arrows aren't recognised.
  - **Saved view:** `--folder` with a folder other than the saved one ignores
    the saved view, so such a test opens on Today.
  - **GUI actions without a mouse:** inside the private `dbus-run-session`,
    `gdbus call --session --dest com.stephenhouser.Reminders --object-path
    /com/stephenhouser/Reminders/window/1 --method org.gtk.Actions.Activate
    move-group-up [] {}` runs a `win.` action, timed before the screenshot
    hook fires (1.5 s after start-up).

## Sources and back ends (stages 1–3 done; stage 4: CalDAV done, 2026-10-02)

Plan agreed with the user: lists come from *sources*, each a back end and its
settings in a `[source.NAME]` section; later the apps show several at once.

1. **Done:** back ends inside one source. core backend.hpp: `Backend` with
   `syncthing` (conflict copies, merge base and own-write fingerprints in
   `<folder>/.reminders/<device>/`, .stignore) and `local` (nothing extra).
   `Store(folder, state_dir, BackendKind)`; `Store::list_name_for` is now a
   member (the back end decides what a conflict copy is). settings.ini reads
   and writes any section (`load_section_setting` / `save_section_setting` /
   `section_names`). core sources.hpp: `SourceConfig`, `load_sources`,
   `default_source` (`default-source=`, else the first), `source_for_folder`
   (configured, else detected), `set_default_folder` (Change Folder… /
   `reminders folder`: detects the back end, creates the source named after
   the folder if needed), `open_source`. `[general] folder=` is no longer
   read (user chose a one-time manual change over migration code).
2. **Done (core only; the apps still use one Store):** core library.hpp:
   `Library` holds a Store per source (`add`, `load_all`, `open_library`
   from settings, skipping missing folders). Lists are keyed "source/name"
   (`key_of`, `list(key)`; a bare name works when unambiguous). Store's
   edits and smart lists are mirrored across sources (scheduled/completed
   re-sorted after merging). Ids are unique across sources
   (`Store::set_other_ids`). `move_to_list` works between sources (the
   reminder keeps its id). Undo: `ListTexts` (snapshot / current_text /
   restore) is implemented by Store (keys = names) and Library (keys =
   "source/name"); `History::undo/redo` take a `ListTexts&`. The Library is
   immovable (stores call back into it), so `open_library` returns a
   unique_ptr. Tests: core/tests/library_test.cpp.
3. **Done:** both apps on the Library. GUI `store_`, the TUI and
   the CLI hold a `rem::Library`; `open_library(folder, device)` opens every
   configured source, or just a --folder one for the session. Sidebar
   groups are `rem::SidebarGroup{kind, source}`: smart lists, one lists group
   per source, tags; `sidebar-order` has `lists:NAME` entries, `local-lists`
   meaning the sources not named (one source: written as `local-lists`, titled
   "My Lists"; several: titled by `source_title`). Folded state is
   `lists-collapsed.NAME`; `local-lists-display` covers every source. Views and
   settings name lists "source/name" (`view`, `lists-order`, `lists-hidden`);
   `list_entry_matches` lets a bare name from older settings match.
   `Library::label` shows "source/name" only where names clash. GUI: a
   folder monitor per source; removing a source only edits settings (files
   stay); settings.ini changes to sources reopen them. TUI: `N` creates in
   the selected group's source. CLI: `lists` grouped by source,
   `new-list --source`, ambiguous names ask for `source/name`.
   Main menu Sources… (dialogs.cpp show_sources_dialog: every source →
   Source Info…, plus one Add Source…); Source Info… and Add Source share
   show_source_dialog: Name (title), Type (Syncthing / Local Folder /
   CalDAV), Folder (Local Copy for CalDAV, defaulting to
   $XDG_DATA_HOME/reminders/caldav/NAME and following the name until a
   folder is chosen; choosing a folder for a new source detects its type),
   a Server group shown only for CalDAV, Default Source, Remove Source….
   Core: `add_source(SourceConfig)`, `new_source_name()`. Source Info… is
   also on a source heading's menu with New List…. Change
   Folder… left the main menu (the welcome page's Choose Folder… and
   `reminders folder` set the default source's folder).
4. New back ends. `local` keeps watching the folder for outside edits
   (agreed). **CalDAV done** (2026-10-02, not yet tried against a real
   server by the user); **WebDAV done** (2026-10-03, see below); git to come. User's choices: libcurl
   (not libsoup), `password-command=` (keyring maybe later), tests against
   a fake server (net/tests/fake_dav.py, Python stdlib; nothing
   installed), sync on open + `interval=` timer + shortly after edits.
   Design: a CalDAV source's folder is a local Markdown copy
   (`$XDG_DATA_HOME/reminders/caldav/NAME`), so Store/apps/undo are
   unchanged. core caldav.hpp `caldav_sync(folder, state, Remote&, lock)`:
   per calendar, pull (CTag, then ETags, then multiget) into "theirs" =
   base.md + server changes; `merge(ours, theirs, base)`; write the list
   under the back end's write lock (aborts if the file changed meanwhile);
   push changed VTODOs (PUT If-Match / If-None-Match, DELETE If-Match);
   412 → base stays the server's version, retried next sync. Records in
   `<state>/caldav/` (calendars.tsv, per calendar items.tsv, base.md,
   <id>.ics raw copies). core ical.hpp / vtodo.hpp: parse/serialize keeping
   unknown properties; write_todo() changes only properties whose meaning
   changed. Store gained `Backend::write_lock()` (held while it writes,
   renames, deletes) and `deleted_by_user()` (only an app delete removes
   the server calendar; a file that merely vanished is fetched again).
   net/ (reminders_net, optional `-DBUILD_NETWORK`): CurlRemote (discovery
   via url → current-user-principal → calendar-home-set, or
   /.well-known/caldav), `run_password_command`, `sync_caldav_source`,
   `SyncRunner` (worker thread; detects list-file changes by mtime/size).
   CLI: `reminders sync [SOURCE]`, syncs before/after commands,
   `--offline`. TUI and GUI run a SyncRunner; GUI ☰ → Sync Now (hidden
   without CalDAV sources), Sources… → Add Source… with Type CalDAV;
   its Server group has address, username, password command, interval. Known limits:
   moves/section changes made in other CalDAV clients aren't merged (local
   order wins); RRULEs beyond FORMAT.md's rules are kept, not shown.
   **WebDAV** (2026-10-03): the server folder holds the list files as they
   are; the local copy is `$XDG_DATA_HOME/reminders/webdav/NAME`. CalDAV and
   WebDAV share `DavSettings` (url, username, password-command,
   interval; `SourceConfig::dav`; named for DAV because OAuth back ends
   would need their own), `has_server(kind)`,
   `default_copy_folder(kind, name)`, `SyncError`/`SyncResult`, and
   `ServerBackend` (write lock; renamed.tsv / deleted.txt notes in
   `<state>/<backend>/`). core webdav.hpp `webdav_sync(folder, state,
   FileRemote&, lock)`: PROPFIND the folder; apply app renames as MOVE
   (Overwrite: F; rounds, a cycle goes through a temporary name; a taken
   name → synced as a new file, merged without base); per file, records
   `files.tsv` (list, name on server, ETag) and `base/<name>.md`; changed
   on one side → copied; both → `merge(ours, theirs, base)` (local wins
   same field) then PUT If-Match; 412 → base stays the server's, merged
   next sync; non-lists changed on both sides → server's wins, ours kept
   locally as "NAME (this device).md". All server files are pulled; local
   files are pushed only when they're lists. Vanished-here files are
   fetched again; app deletions DELETE If-Match (changed there → comes
   back). net/: shared `dav.hpp` (Http, multistatus, URL escaping),
   `webdav_client.hpp` (CurlFiles; makes a missing folder with MKCOL),
   `server_sync.hpp` (`sync_source`, `run_password_command`); SyncRunner
   and the CLI/TUI/GUI handle both kinds. Tests: core webdav_test.cpp (fake
   FileRemote), net_test.cpp against net/tests/fake_dav.py (now CalDAV +
   WebDAV under /files/). GUI Type list: Syncthing, Local Folder, CalDAV,
   WebDAV; the Server group's description and Address hint follow the type.

## Future features (not started)

- **Other platforms**: iOS first, then Android, Windows and macOS, each
  native (SwiftUI on iOS/macOS); iOS syncs with Syncthing's Go core built in
  (gomobile). The iOS Reminders look is the reference for features, and
  docs/FORMAT.md is the contract every client implements. iOS can't be built
  on the Linux dev machine (no Swift or Xcode); nothing gets installed
  without asking.
- **KDE variant** (possible): a native Qt/Kirigami client for Plasma beside
  the GNOME one, on the same core library. Not decided; added 2026-10-03.
- **Drag and drop with other apps**: dropping text onto the window or a
  sidebar list, and dragging reminders out as text. Dropping files is done
  (they're imported); dragging within the app already works. Added
  2026-10-03.
- **Query language**, SQL-like, for the CLI (`reminders query "…"`) and for
  defining your own smart lists (saved queries in the sidebar). Wanted by the
  user 2026-10-02; design not started.

## Where things stand (2026-10-04)

- **Done:**
  - **Core library** (C++23): the file format with byte-for-byte round
    trips, three-way merging of Syncthing conflict copies, undo/redo history,
    settings, the XDG base directories, and sources: several open at once
    (`Library`), each with its own back end (syncthing, local, caldav,
    webdav, git). Unit tests pass (137).
  - **CalDAV, WebDAV and git back ends** (net/): CalDAV and WebDAV (libcurl
    + libxml2) keep a local Markdown copy in step with the server, merged
    three-way; git commits, pulls (list conflicts merged by the app) and
    pushes. Synced on open, every `interval=` minutes and shortly after
    edits. Tested against a fake DAV server in Python and local git
    repositories (15 network tests).
  - **GNOME app** (`Reminders`): the features in the brief, the keyboard
    shortcuts, drag and drop within the app, the quick switcher,
    configurable sidebar groups (order, visible / collapsible / hidden,
    rearranged from the sidebar), one sidebar group per source, ☰ →
    Sources… with one Add Source… form for every type, Sync Now.
  - **Terminal client** (`reminders`): the CLI and the TUI, sharing settings
    and the last view with the app; `sync`, `--offline`, `new-list
    --source`, `import`.
  - **Import** (2026-10-03): core importer.hpp. `read_import` tells the
    kinds apart by content: BEGIN:VCALENDAR → `read_ics`; checklist lines
    → `read_markdown` (Document's reminders with sections, ids, colour);
    else `read_plain_text` (a line each; bullets/numbering dropped, "#
    Heading" → section, indented → subtask, inline fields via
    parse_fields, front matter skipped). `read_ics` (VTODOs via
    read_todo; RELATED-TO → subtasks under the top parent;
    X-APPLE-SORT-ORDER order when every task has one; events and other
    components counted as skipped; X-WR-CALNAME / X-APPLE-CALENDAR-COLOR
    for a new list) and `import_into` (appends, one save; skips ids already
    in the library; id-less ones get new ids, and are skipped when the list
    has an open reminder of that title). Ids come from UIDs (`id_for_uid`: a usable UID as is,
    else 10 base-36 digits of its FNV-1a hash), so re-importing doesn't
    duplicate. CLI `reminders import FILE [--list L] [--source S]`
    (list by name, made if missing). GUI ☰ → Import…: file chooser (.ics,
    .md, .txt, or all files),
    then an alert with "Into" (New List “calendar name” or any list), one
    undo step. Events aren't imported (could become dated reminders later).
  - **Export** (2026-10-03): core exporter.hpp `export_list(list, format)`:
    Markdown = serialize(doc); text = open reminders (all with
    `completed`) via format_fields without id/created, subtasks indented
    2, sections "# Name"; ics = VCALENDAR (X-WR-CALNAME, X-APPLE-CALENDAR-
    COLOR) of new_todo_calendar VTODOs, UID = reminder id (ensure_ids
    first), RELATED-TO parents, X-APPLE-SORT-ORDER steps of 1024. Each
    round-trips through the importer (tests). CLI `reminders export LIST
    [--format md|txt|ics] [-o FILE|DIR|-] [-a]` (format from -o's
    extension). GUI ⋮ → Export… (header and sidebar menus): alert with
    Format (+ Include Completed for text), then a save dialog.
  - **todo.txt, CSV, export all** (2026-10-03): `Import::Kind` and
    `ExportFormat` gained Todotxt and Csv. todo.txt: x/dates, (A)–(C) ↔
    high/medium/low (pri: on done lines), +project/@context → tags (export
    writes +tag), due:, and extensions time:, rec: (Nd/w/m/y, 1b = every
    weekday; "every weekend" isn't written), flag:yes, url:, id:, p:
    (topydo-style parent); unknown key:value words stay in the title. CSV:
    RFC 4180 with delimiter (, ; tab) found from the header; columns by
    normalised header name with synonyms (Todoist's CONTENT / DESCRIPTION
    / PRIORITY 1–4 work); Parent by id or title (`nest`). Export columns
    List, Section, Title, Done, Due Date, Due Time, Priority, Flagged, Tags,
    Repeat, URL, Notes, Completed, Created, ID, Parent ID; CRLF.
    `detect_kind(text, file_name)`: BEGIN:VCALENDAR; .csv / todo.txt /
    done.txt / *.todo.txt / .ics names; checklist → Markdown; header with
    Title + another known column → CSV; half the lines todo.txt-like →
    todo.txt; else plain text. `export_all(library, folder, format)`: a
    file per list, `source-Name` on clashes. An import that made a new
    list and added nothing deletes it again. CLI: `import --format`,
    `export` without LIST → every list into `-o FOLDER`. GUI: Import's
    alert has Read As (re-reads; Import disabled when it can't), ☰ →
    Export All Lists… (same alert, then a folder chooser); replaced below.
  - **Import Duplicates, one Export dialog** (2026-10-03): `import_into(…,
    duplicates)` skips nothing (taken ids get new ones); GUI switch Import
    Duplicates, CLI `import --duplicates`. ☰ → Export… (`Window::
    export_lists(chosen)`): alert with Format, Include Completed (text) and
    a scrolling check list of every list under an All Lists check
    (inconsistent when some); one ticked → save dialog, several → folder
    chooser + core `export_lists(library, lists, folder, …)` (export_all
    calls it). A list's ⋮ → Export… opens it with that list ticked; Export
    All Lists… is gone.
  - **.zip archives** (2026-10-03): core src/zip.{hpp,cpp} `ZipWriter`
    (local headers, central directory, end record; UTF-8 names; deflate
    via zlib when found, `REMINDERS_ZLIB`, else stored; tiny files stored
    anyway; own CRC-32 without zlib). zlib is optional in core/CMakeLists
    (find_package(ZLIB)), so the core still needs only a compiler.
    `export_zip(library, lists, format)` returns the archive (same file
    names as export_lists). CLI: `export -o FILE.zip` without LIST. GUI:
    Compressed Archive switch, shown when two or more lists are ticked →
    save dialog for Reminders.zip. Checked with unzip -t.
  - **git back end** (2026-10-03): runs the `git` command (no libgit2: not
    installed, and git's own auth then works), from net/ git_sync.{hpp,cpp}
    via posix_spawnp, no shell, LC_ALL=C, GIT_TERMINAL_PROMPT=0, SSH
    BatchMode (fails rather than prompts). Settings: `GitSettings` (url,
    remote, branch, interval; `SourceConfig::git`), `backend=git`;
    `syncs(kind)` = CalDAV, WebDAV, git (SyncRunner, CLI sync, Sync Now);
    `has_server` stays DAV only; `sync_interval(source)`; `in_git_repo`.
    Back end: ServerBackend (write lock; its notes are deleted, git sees
    renames itself). Sync: open/clone (url= into an empty folder; a
    non-empty one: init + remote's HEAD branch, then merged with
    --allow-unrelated-histories); commit `:(glob)*.md` only (relative to
    the folder, so subfolders and other files stay out) as "Reminders
    (DEVICE): names", identity from git config else -c user.name=Reminders
    user.email=reminders@DEVICE; fetch remote branch (missing → push);
    under the write lock commit again and merge FETCH_HEAD; unmerged
    top-level list files resolved with `merge(ours=:2, theirs=:3,
    base=:1)`, delete/modify keeps the modified side, anything else →
    merge --abort + error; push HEAD:refs/heads/BRANCH, retried after a
    rejection (3 tries). No remote → commits only. remove_source leaves a
    git clone (unpushed commits). GUI: Type Git, Repository group (Clone
    From, Remote, Branch, Sync Every), folder defaults to
    $XDG_DATA_HOME/reminders/git/NAME when Clone From is set; choosing a
    folder in a repository picks Git. Tests: net_test.cpp against bare
    repositories made by the test (GIT_CONFIG_GLOBAL set to a file with
    init.defaultBranch=main, no identity).
  - **git: new repositories** (2026-10-04): a git source's folder that
    isn't in a repository and has no url= is `git init`ed on the first sync
    (was an error); url= on a repository without that remote adds it, so a
    local-only source can start pushing later. Git folders are made on open
    like DAV copies (open_source, open_library); the dialog's default folder
    ($XDG_DATA_HOME/reminders/git/NAME) applies with or without Clone From,
    and any folder is accepted. Test net_git_new_repository_then_a_remote.
  - **Dropping files imports them** (2026-10-03): `make_file_drop_target`
    (GDK_TYPE_FILE_LIST, COPY) on the window (into: the list in view) and
    on each sidebar list row (highlighted with drop-into; into: that
    list), separate from the reminder drop targets. `Window::import_files`
    opens the import alert for each file in turn (`then` chains them),
    with `into` chosen under Into; folders and unreadable files get a
    toast.
  - **Dragging sidebar entries** (2026-10-04): smart lists, lists and tags
    are dragged with the mouse to another place in their own group (a
    source's lists stay in that source's group). `make_entry_draggable` /
    `make_entry_drop_target` (boxed type RemSidebarEntry carrying the View;
    drop-above / drop-below line, faded row) beside the reminder and file
    drop targets on list rows. Core `move_next_to(order, name, target,
    after)`; `Window::entry_order` / `save_entry_order` are shared with
    Move Up / Down; `drop_entry`, `same_sidebar_group`. Checked headless
    through `drop_entry` (a real mouse drag can't be made headless).
    Groups too: a group is dragged by its heading (`make_group_draggable`,
    type RemSidebarGroup; the whole group fades) and dropped on any row of
    another group (`make_group_drop_target`): above it over the group's top
    half, below over its bottom half (line on the group's first / last
    row). Core `move_sidebar_group_next_to`; `Window::drop_group` saves
    sidebar-order. The top group has no heading unless collapsible, so it's
    moved by dropping another above it. Checked headless via `drop_group`.
  - **Selecting several reminders** (GNOME app, 2026-10-04): implicit,
    no selection mode. `Window::selected_` (ids) is separate from the focus
    (`cursor_`); `anchor_` for ranges; `shown_ids_` / `reminder_rows_`
    registered by build_reminder_row, pruned on rebuild, cleared on a view
    change. Ctrl+click toggles, Shift+click range (Ctrl+Shift adds), a
    capture-phase click gesture that claims modified clicks so the title /
    check / buttons don't see them; plain click outside clears, on a
    selected row clears on release (so a press-drag drags the selection);
    Shift+↑/↓ extends, Ctrl+A all, Esc or plain ↑/↓ clears. Ctrl+A and Esc
    are a capture-phase key controller on the window (any focus, even none
    after a rebuild), skipped for GtkText / GtkTextView focus, popovers and
    AdwDialogs; select_all focuses the first row unless a reminder row has
    focus, so Delete etc. work next. `.selected-
    reminder` tint; subtitle "N Selected". Every row action goes through
    `targets(id)` (the selection when id is in it, display order) and
    `outermost()` (drops subtasks whose parent is there) for move / delete /
    copy. Ops take id vectors: complete_reminders (all done, or all undone
    if all were), toggle_flag (same rule), set_due, set_priority,
    move_reminders (first next to target, rest after it), move_to_list,
    move_to_section_end, delete_reminders, copy_reminders; checkbox click is
    still one reminder; Details / indent / Alt+↑↓ stay single. Each is one
    undo step via `batch()`: undoable + core `hold_saves()` /
    `release_saves()` (Store and Library) so each list file is written
    once. Drag carries `std::vector<std::string>` (type
    RemindersReminderIds) with a count badge (accent pill) and fades all
    dragged rows. The ⋮ menu is made on open (create_popup_func) with
    plural labels, Mark as (Not) Completed (Space; complete_reminders), Copy
    and a new Move To submenu. Right-click / long-press anywhere on a row
    (capture phase, claimed, so the title doesn't start editing; not while
    editing) opens the same menu at the pointer from an invisible
    `content_menu_button_` over the content (as the sidebar's), with the
    row's action group (kept as "reminder-actions" data) inserted on it.
    Later: a plain click on a row's empty space selects it (on release, so
    a press-drag still drags the selection; title / circle clear it, ⋮ /
    Details keep it); `select_for_menu` selects a row while its menu is open
    and unselects on the popover's "closed" (`menu_selected_`); plain ↑/↓
    with a selection make the row focused next the selection
    (`follow_focus_`, set by the key handler, used by the row's focus
    "enter"); "N Selected" only for 2+. A capture-phase click gesture on the
    window (any button; not in menus, by surface, or dialogs) calls
    `Window::clicked(hit, modified)`: outside the title being edited it
    stops editing, keeping the text; outside every reminder row it clears
    the selection. Shortcuts dialog has a section.
    Checked headless through direct calls (selection, menu, each op +
    undo); real Ctrl/Shift clicks and drags need trying by hand.
  - **Marking several reminders in the TUI** (2026-10-04): `v` toggles the
    mark and moves down, `*` marks every reminder showing, Esc (anywhere)
    unmarks. `Tui::marked_`, pruned to the reminders in view each draw and
    cleared on a view change; `*` in the margin (kMarked colour), "N marked"
    in the title and the status line. While any are marked, `act_on_marked`
    takes x / Space, f, t / T, d, 0–3 / Alt+0–3, #, m and Delete for all of
    them (`batch()` = undoable + hold_saves, so one undo step and one write
    per list); same rules as the GNOME app (complete / flag all alike,
    outermost for move / delete, step_off keeps the cursor's place). Tested
    in tmux.
  - **Docs:** README, docs/FORMAT.md, docs/USING.md, docs/TERMINAL.md,
    docs/settings.example.ini.
- **Known gaps:**
  - CalDAV and WebDAV haven't been tried against a real server yet (only
    the fake one); git only against local repositories (not GitHub or
    another host over SSH or HTTPS).
  - CalDAV: moves and section changes made in other CalDAV clients aren't
    merged (local order wins); RRULEs beyond FORMAT.md's rules are kept but
    not shown.
  - Only the Linux clients exist (see Future features).
- **Next step:** not chosen yet; ask the user. The Future features above
  are the candidates.
