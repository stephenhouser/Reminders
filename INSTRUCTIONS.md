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
| Per-device state | `<folder>/.reminders/<device>/`, excluded via `(?d).reminders` in the Syncthing root's `.stignore` | Everything about a list stays in its folder |
| iOS sync (later) | Syncthing embedded via gomobile, not the Möbius Sync app | Self-contained app |
| Names | App "Reminders", command `reminders`, app ID `com.stephenhouser.Reminders`, config `~/.config/reminders/` | |
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
- Per-device state in `<folder>/.reminders/<device>/` (device = hostname plus
  4 hex digits of a hash of /etc/machine-id): base/<list>.md (the last
  version received from elsewhere: the merge base), written/<list> (a 64-bit
  FNV-1a fingerprint of our last write, to recognise our own writes after a
  restart), declined.txt. Move it on rename and remove it on delete. Find the
  Syncthing root (nearest ancestor with `.stfolder`) and append
  `(?d).reminders` to its `.stignore` once.

3. The GNOME app
- Layout: AdwOverlaySplitView (the sidebar can be hidden at any width;
  collapses below 560sp). The sidebar has smart lists (Today, Scheduled, All,
  Flagged, Completed, with counts), My Lists (coloured icon badges, counts)
  and Tags, plus a search bar and a "New List" button. The content area
  shows the selected view in AdwClamp'd boxed lists at 90% of the content
  width, updated on resize.
- List view: one boxed list per section, with a heading and a ⋮ menu
  (Rename, Delete with "keep reminders" or "delete them"). A "New Reminder"
  entry row ends each section: typed inline fields are parsed. Completed
  reminders are hidden behind a "N completed · Show" footer.
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
- Remember the folder and the last view in ~/.config/reminders/settings.ini.
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
  Ctrl+E / Ctrl+Shift+E show/hide all subtasks; Ctrl+B sidebar; Ctrl+F
  search (Enter or ↓ moves into the results); Ctrl+Z / Ctrl+Shift+Z;
  Ctrl+? shortcuts dialog (AdwShortcutsDialog).
- Custom symbolic icons (cart, gift, briefcase, mortarboard, heart, book,
  banknote, pill, flag, tag) in a GResource, plus an app icon and a .desktop
  file. App ID com.stephenhouser.Reminders.

4. Docs: README.md (overview, build, layout), docs/USING.md (user guide),
docs/FORMAT.md (the format), and keep this file current.
````

### Directions

Build in this order, and check each stage before going on.

1. **Check the machine and ask before installing.** Needed: a C++23 compiler,
   CMake, Ninja, `gtk4-devel`, `libadwaita-devel` and `glib2-devel`.
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
10. **Docs.**

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
- **Recognising your own writes after a restart needs persistent state** (the
  fingerprint), or unsynced local edits become the merge base and the other
  device's changes get lost in the next conflict.
