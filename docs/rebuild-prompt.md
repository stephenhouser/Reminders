# Rebuild prompt (frozen, 2026-10-04)

A historical artifact: the prompt and build order used to bring the first
version of this project into being, kept in case it's ever rebuilt from
scratch or used as the starting point for another client.

**It is not maintained.** It describes the project as it was planned, not as
it is. Where it disagrees with the code, `CLAUDE.md`, `README.md` or
`docs/FORMAT.md`, those are right and this is stale. It was moved out of
`INSTRUCTIONS.md` on 2026-10-05 for that reason.

---

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
- Drag and drop: rows carry the ids as a private boxed GType, plus their
  Markdown as plain text for other apps; the reminder drop targets run in
  the capture phase so the app's own text fields don't take that text.
  Text dropped from other apps (not the app's own drags, not files) becomes
  reminders. Drop on the top or bottom half of a
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
  not showing). TUI Ctrl+S (S until 2026-10-04) opens settings.ini in $EDITOR and reloads it.
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

