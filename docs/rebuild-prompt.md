# Rebuild prompt (as of 2026-10-07)

The prompt and build order that would bring this project into being from an
empty directory: kept in case it's ever rebuilt from scratch, or used as the
starting point for another client.

It was first written for the original build (2026-10-04) and brought up to
date with the apps on 2026-10-07. It's a summary, so where it disagrees with
the code, `CLAUDE.md`, `README.md`, `docs/FORMAT.md`, `docs/KEYS.md` or a back
end's README, those are right.

---

### The prompt

Give this to a coding agent (such as Claude Code) in an empty directory on a
Fedora machine. It describes the finished result; the directions below give
the order to build it in.

````text
Build "Reminders": a to-do app modelled on Apple Reminders, whose data is a
folder of plain Markdown files, one per list, readable and editable in any
text editor. No account, no server, no database. Several *sources* can be
open at once, each with a *back end* that keeps its lists in sync: a local
folder, Syncthing, CalDAV, WebDAV or git. None of these is the basis of the
project: the Markdown files are, and a back end is a module that can be added
or left out. Start with the Linux clients (a GNOME app and a terminal
client); iOS, Android, macOS and Windows come later, each native, and must
read the same files, so the file format is the contract.

Ground rules
- Don't install anything (packages, toolchains, libraries) without asking me.
  Checking what's installed is fine.
- C++23, CMake + Ninja, everything in `namespace rem`. Public headers under
  `<dir>/include/reminders/`, included as <reminders/x.hpp>. GTK 4 and
  libadwaita through their C APIs only (no gtkmm), with small home-made RAII
  and lambda helpers.
- Format all C/C++ with my ~/.clang-format (read it; Google base, tabs) until
  a pass changes nothing; brace every if/for/while body. Add `format` and
  `format-check` targets.
- Every file location goes through $XDG_CONFIG_HOME, $XDG_DATA_HOME,
  $XDG_STATE_HOME and $XDG_CACHE_HOME (core paths.hpp), never a literal
  ~/.config; docs name the variable.
- Tests: a tiny home-made harness, no framework. A Debug build is the default;
  a Release build must be warning-free too (GCC's -Wmaybe-uninitialized).
- The GNOME app is single-instance. Never launch a test copy on my desktop or
  session bus: write tools/gui-test.sh (private D-Bus session, headless mutter
  on a private Wayland socket, its own HOME and XDG dirs, nothing reaching my
  display) and tools/gui-keys.py (real key presses and clicks through
  mutter's RemoteDesktop API, from a STEPS file). Add a developer hook:
  REMINDERS_SCREENSHOT=out.png renders the window to a PNG after 1.5 s (or
  REMINDERS_SCREENSHOT_DELAY ms) and quits. Test the TUI on a private tmux
  server (tmux -L <name>).

Layout
- core/: the library, standard library only: model, format, recurrence,
  merge, store, library (several sources), history (undo), settings, XDG
  paths, sources, back end registry, sync runner, iCalendar/VTODO,
  importer/exporter, clipboard text, dates.
- app/: the toolkit-free layer both clients share: Actions on reminders,
  Selection, the sidebar's model, ViewModel, display preferences,
  import_file. Unit-tested.
- backends/<id>/: one module each (local, syncthing, caldav, webdav, git,
  plus common/ for HTTP and multistatus), each with its tests and README.md.
  Each has a register_<id>_backend(); the build generates
  register_backends() from the ones turned on (BUILD_BACKEND_<ID>, all on).
  Nothing outside a back end's folder names it; a source naming a back end
  that isn't built opens as a local folder.
- clients/gnome/ and clients/terminal/, named for the environment, not the
  OS (BUILD_GNOME_APP, BUILD_TERMINAL_APP). Both executables build into
  build/bin/: `Reminders` (GNOME) and `reminders` (terminal). Each client
  has its own version in src/version.hpp, apart from the file format's
  version. `cmake --install` installs both, the .desktop file and the
  hicolor icon. App ID com.stephenhouser.Reminders.

1. File format (write docs/FORMAT.md first; it is the cross-client contract)
- One list = one `<Name>.md` file directly in the folder, whose front matter
  contains `reminders: 1` (the key marks a list; 1 is the format version;
  writers put it first). Other Markdown files are ordinary notes and are
  left alone, so the folder can be an Obsidian vault.
- Front matter keys: color (red orange yellow green cyan blue indigo purple
  pink brown gray), icon (list tag bookmark cart gift home work school
  calendar flag star heart music game book food travel nature person people
  money pill computer camera), order (integer). Keep unknown keys, comments
  and nested YAML verbatim.
- A reminder is `- [ ] ` or `- [x] ` followed by the title and inline fields,
  ending in ` ^id` (6+ lowercase alphanumerics, unique in the folder).
  Inline fields use the Obsidian Tasks emoji syntax: #tag, ⏫ 🔼 🔽 priority
  (read 🔺 ⏬ too), 🚩 flag, 🔁 every <rule>, 📅 YYYY-MM-DD [HH:MM], ✅ done
  date, ➕ created date, 🔗 url. Ignore U+FE0F. Anything else is title.
- Subtasks: indented task lines under a reminder, one level deep. Notes:
  other indented lines; blank lines inside notes are kept when the next line
  is indented. The format has no escape, so a notes line written `- [ ] …`
  reads back as a subtask; text combined from a paste is written `☐ …`.
- Sections: `## Name` headings. Other headings and prose are kept in place.
- Round trip: an unchanged file serializes byte for byte. A hand-written line
  keeps its exact text until its fields change. Lines without ids get one in
  memory, written only with the next real change to that list.
- Repeat rules: every day/weekday/weekend/week/month/year, every N
  days/weeks/months/years; others kept but not acted on. Completing a
  repeating reminder marks it done (✅ today, no more 🔁) and inserts an open
  copy with the next date above it (subtasks reopened, notes copied).
- Writes are atomic (`.<name>.md.tmp` then rename) and only happen when the
  content changed.

2. Sources, back ends and sync
- settings.ini ($XDG_CONFIG_HOME/reminders/) is shared by every client: a
  [general] section (keys outside it are ignored) and one [source.NAME]
  section per source: backend=, folder= (accepts ~, $HOME, ${VAR},
  home-relative; written back as ~/… under home), title=, interval=, and the
  back end's own keys. default-source= picks where new lists go. Kept line
  for line; other lines survive.
- Library: a Store per source, lists keyed `source/name` (a bare name where
  that's unambiguous), ids unique across sources, smart lists, tags and
  search across all of them, moves between sources, undo spanning them.
- A SyncRunner on a worker thread syncs each server-backed source on open,
  every interval= minutes (default 15) and shortly after an edit, under the
  back end's write lock. The CLI syncs before a command and after one that
  changes something (--offline skips it).
- Per-device state: device = hostname plus 4 hex digits of a hash of
  /etc/machine-id. Always from source_state_dir(): a back end that sets
  state_in_folder (only Syncthing) keeps it in <folder>/.reminders/<device>/;
  every other one in $XDG_STATE_HOME/reminders/<device>/<source>/. Records:
  base/<list>.md (merge base), written/<list> (64-bit FNV-1a fingerprint of
  our last write, so our own writes are recognised after a restart; without
  it unsynced local edits become the base and the other side's changes are
  lost), declined.txt. Move them on rename, remove them on delete.
- local: watch the folder (GFileMonitor in the GUI, a 1 s poll in the TUI),
  debounce, reload changed lists, ignore dotfiles and our own writes.
- syncthing: as local, plus merge `<Name>.sync-conflict-*.md` copies into
  `<Name>.md` by reminder id (title when there's no id), three-way when a
  base is known: one-sided field changes win; both changed → main file wins;
  deleted on one side and unchanged on the other → deleted; edit beats
  delete; additions go after their nearest earlier common reminder. Without
  a base: union, main wins, nothing deleted. Merge front matter the same way.
  Leave conflict copies of non-list files alone. Find the Syncthing root
  (nearest ancestor with .stfolder: .stignore is only read there) and append
  `(?d).reminders` to its .stignore once. Picked automatically for a folder
  inside a Syncthing folder.
- caldav: libcurl + libxml2. One calendar per list, one VTODO per reminder,
  a Markdown local copy in $XDG_DATA_HOME/reminders/caldav/NAME/. Discovery
  through /.well-known/caldav and the principal. Pull (CTag, ETags,
  multiget), three-way merge against base.md, write under the lock, push
  changed VTODOs; a 412 is retried next sync. Change only the properties
  whose meaning changed; keep everything else as the server sent it (raw
  .ics kept per reminder). Field mapping, order (X-APPLE-SORT-ORDER),
  sections (X-REMINDERS-SECTION), flag (X-REMINDERS-FLAGGED): see
  backends/caldav/README.md. password-command= runs a command that prints
  the password.
- webdav: the list files themselves in a server folder, synced file by file:
  If-Match / If-None-Match: * writes, three-way merge when both changed,
  renames as MOVE with Overwrite: F, deletes with If-Match. A non-list
  changed on both sides keeps the server's and saves ours as
  "NAME (this device).md".
- git: by running `git` (so SSH keys and credential helpers work): commit the
  changed list files ("Reminders (<device>): Groceries, Home"), fetch, merge,
  push. A list file in conflict is merged by our merge (git's base, ours as
  main, theirs as the conflict copy), never left with conflict markers. A
  folder not yet in a repository is cloned from url= or `git init`ed.
- Tests for caldav and webdav run against a fake Python DAV server
  (backends/common/tests/fake_dav.py).
- Remove Source leaves the files; "Erase all source data" erases the folder
  and the records (never the filesystem root or home), never the remote.

3. The GNOME app (GTK 4 ≥ 4.20, libadwaita ≥ 1.8)
- Layout: AdwOverlaySplitView (the sidebar can be hidden at any width;
  collapses below 560sp). The sidebar has groups: smart lists (Today,
  Scheduled, All, All Reminders (completed too), Flagged, Completed, with
  counts), one group per source's lists (coloured icon badges, counts; "My
  Lists" with one source, else the source's title) and Tags, plus search and
  a "New List" button. The content shows the selected view in AdwClamp'd
  boxed lists. Every view's header has a count (core count_label: lists,
  tags and All Reminders "6 Reminders / 3 Complete"; Today, Scheduled, All,
  Flagged "N Reminders"; Completed "N Completed"; search "N Results"; with
  two or more selected, "N Selected").
- Sidebar settings (per device, in settings.ini, applied live through a
  GFileMonitor on the file): sidebar-order (smart-lists, local-lists,
  lists:NAME, tags; unknown or missing groups appended), smart-lists (which,
  in order), *-display = visible | collapsible | hidden (lists never
  hidden), *-collapsed, lists-hidden / tags-hidden plus show-hidden (hidden
  entries dimmed), lists-order / tags-order, tag-color.NAME / tag-icon.NAME,
  show-sidebar, row-buttons = hover | always, note-lines = N. The top group
  has no heading unless collapsible. Entries and groups move by drag, by
  their right-click menu (Move Up/Down, Hide/Show, Collapsible) or by
  Ctrl+↑/↓ and Ctrl+Shift+↑/↓. Right-clicking a list in the sidebar gives
  the list's ⋮ menu for it without opening it; a source's heading gives New
  List…, Sync Now, Source Info…. Popover menus are hosted in a GtkMenuButton
  (an invisible one in a GtkOverlay corner for the sidebar) so they size
  themselves and survive rebuilds.
- List view: one boxed list per section, with a heading and a ⋮ menu
  (Rename, Delete keeping or deleting its reminders). A "New Reminder" entry
  row ends each section; typed inline fields are parsed. Completed reminders
  are hidden (Ctrl+H / ⋮ → Show Completed). The list's ⋮: Show Completed,
  Show / Hide All Subtasks, Add Section…, List Info…, Export…, Delete List….
- Reminder row: round check button (list colour as accent), an editable
  title that wraps (GtkEditableLabel doesn't by default), priority marks
  !/!!/!!!, a second line (due date, red if overdue; repeat icon; tags; link;
  list name in smart views), note-lines of notes, a subtask disclosure with
  a hidden count, and flag, ✏ details and ⋮ buttons (on hover, or always
  dimmed). The ⋮ menu, also on right-click / long-press anywhere on the row:
  Mark as Completed, Details…, Flag, Due Today / Tomorrow, Cut, Copy, Paste,
  Move To ▸, Indent / Outdent, Move Up / Down, Delete, each with its
  accelerator. Menu / Shift+F10 opens it from the keyboard.
- Details dialog (AdwDialog, Cancel / Done, Ctrl+S saves, Esc cancels):
  title, URL, notes, date, time, repeat (presets plus "Custom: <rule>"),
  flag, priority, list (moves it, across sources too), tags, subtasks,
  Delete. List dialog: name (unique case-insensitively, no / \ < > : " | ?
  *, no leading dot), source (with several), colour swatches, icon grid.
  Tag Info: the same without the name.
- Multi-select: Ctrl+click toggles, Shift+click (and Ctrl+Shift+click) a
  range, Shift+↑/↓ extends, Ctrl+A all showing, Esc / Ctrl+Shift+A or a
  click outside clears. The reminder keys and menu then act on all of them
  as one undo step and one write per list: complete and flag set all alike;
  move, copy and delete take the outermost reminders with their subtasks;
  Details, Indent/Outdent and Ctrl+↑/↓ act on the focused one. Dragging a
  selected reminder drags them all.
- Copy / paste: Ctrl+C copies as list-file Markdown without ids (core
  clipboard.hpp); Ctrl+X cuts in one step. Ctrl+V (a window key controller
  in the bubble phase, so text fields paste normally) pastes checklist lines
  with every field, bullet or numbered lines as one reminder each, other
  text as one reminder (first line title, rest notes); a lone URL in a title
  also sets the URL. A toast after several lines offers Split into N /
  Combine into One. Ctrl+Shift+V asks first. They go after the focused
  reminder, else at the end of the shown list, else into the first list
  made to show in the smart view. One undo step.
- Drag and drop: rows carry ids as a private boxed GType (never a plain
  string, which text fields would accept) plus Markdown text for other apps;
  the reminder drop targets run in the capture phase. Drop on the top or
  bottom half of a row (taking its level), on a section's New Reminder row,
  or on a sidebar list. Text dropped from other apps becomes reminders as
  pasted text does; a dropped file is imported. Auto-scroll near the edges.
  Look the row up through gtk_event_controller_get_widget; it's destroyed by
  the rebuild after a drop.
- Smart views group by date (Overdue first) or by list. Search (Ctrl+F)
  covers titles and notes; Enter, ↓ or Tab moves into the results. Go To
  (Ctrl+K): prefix > word start > substring > subsequence; finds entries in
  folded groups; the last entry searches.
- Undo/redo: snapshot every list's file text before and after each user
  action and record the difference as a step. Undo restores the old text if
  the file is unchanged since, else merges three-way so newer changes from
  other devices are kept; skip lists deleted elsewhere and say so. Nested
  actions record one step. Undo also in the toast after a delete or cut.
- Import (☰ → Import…, Ctrl+O, or a dropped file) and Export (☰ →
  Export…, or a list's ⋮ → Export…): iCalendar VTODOs, Markdown checklists,
  todo.txt, CSV with a header row, plain text a line each; the kind found
  from name and content and shown under "Read As". Import into a new list
  named after the calendar or file (one choice per source) or an existing
  one; reminders already there (by id, else by open title in that list) are
  skipped unless Import Duplicates. Export one list to a file, several to a
  folder or one .zip (zlib optional); every format but text keeps ids, so
  an export imports back as it was. One undo step.
- Sources dialog (☰ → Sources…): Add Source (name, type, folder; type picked
  from the folder: Syncthing inside .stfolder, Git inside a repository, else
  Local Folder; a Server section for CalDAV/WebDAV, a Repository section for
  Git), Source Info (Default Source switch, Remove Source… with "Erase all
  source data", ticked to start with for CalDAV and WebDAV).
- A banner offers Markdown files that contain a checklist but no marker:
  "Review…" opens a dialog of switches; adopted files get the marker as
  their first front-matter key; declined files are remembered per device.
- Sync problems show as a message at the bottom of the window. ☰ → Settings…
  (Ctrl+,) opens settings.ini with GtkFileLauncher, creating it with
  [general].
- Notifications via GNotification at the due time (09:00 for all-day), only
  while running; clicking one runs app.show-reminder(id).
- Command line: `Reminders [FOLDER]` opens FOLDER for this session only
  (relative paths resolved against the caller's directory, even when handed
  to a running instance), using its source's settings if it is one; a
  folder other than the saved one ignores the saved view. --version.
- Keys: GNOME HIG, no Alt keys and no Ctrl+number jumps. The full table is
  docs/KEYS.md. Ctrl+N new reminder; Ctrl+Shift+N new list; Space complete;
  Enter/F2 edit title (Ctrl+S save, Esc cancel; a GtkSearchEntry swallows
  Esc, so close on "stop-search"); Ctrl+E / Alt+Enter details; Ctrl+D flag;
  Ctrl+T / Ctrl+Shift+T due today / tomorrow; Ctrl+0…3 priority; Ctrl+] /
  Ctrl+[ indent / outdent; Ctrl+↑/↓ move (a reminder, or a sidebar entry;
  Ctrl+Shift+↑/↓ its group); Shift+→/← show / hide subtasks; Ctrl+X/C/V,
  Ctrl+Shift+V; Delete; Ctrl+A, Esc, Ctrl+Shift+A selection; Ctrl+K go to;
  Enter in the sidebar opens the entry and moves into its reminders;
  Ctrl+Page Down/Up next / previous entry (over what's showing); Ctrl+H
  completed; Ctrl+Shift+H hidden lists; Ctrl+B / F9 sidebar; Ctrl+L switch
  between sidebar and reminders; Ctrl+F search; F10 main menu; Ctrl+S sync
  this source (the focused heading's or list's, else the shown list's; all
  for a smart list, tag or search), Ctrl+Shift+S sync all; Ctrl+O import;
  Ctrl+, settings; Ctrl+Z / Ctrl+Shift+Z; Ctrl+? shortcuts dialog
  (AdwShortcutsDialog, General first); Ctrl+W / Ctrl+Q.
- Custom symbolic icons (cart, gift, briefcase, mortarboard, heart, book,
  banknote, pill, flag, tag, …) in a GResource, plus the app icon and a
  .desktop file.

4. The terminal client: `reminders` (one binary, no GTK; ncursesw)
- With a command it's the CLI; with none, the TUI. One command table
  (names, usage, summary/help, whether it edits lists, CLI / TUI / both,
  what its words complete to) drives dispatch, --help, the TUI's `:` prompt
  and Tab completion. Commands write to an output stream and ask through
  hooks (confirm, choose, edit, edit_list), never stdin/stdout directly, so
  they run unchanged inside the TUI.
- CLI: lists, list [VIEW] [-a], show, add, edit, edit-list [LIST], done,
  undone, move (--to LIST [--section S]), delete [--yes], search, new-list
  (--color --icon --source), import FILE (--list --source --format
  --duplicates), export [LIST] (--format -o -a), folder [PATH], sync
  [SOURCE], tui, help [COMMAND]; global --folder, --json, --no-color,
  --offline (anywhere on the line). Reminders are named by title, never by
  an id the user has to copy: exact > prefix > substring > all words in any
  order; open beats completed; --in LIST narrows; still ambiguous: a
  numbered pick list on a terminal, else list the matches and exit 1. Ids
  are still accepted and appear in --json. Exit codes 0/1/2.
- Field options: --title --list --section --parent --in --due
  (today/tomorrow/weekday/+3d/+2w/+1m/YYYY-MM-DD) --time --no-due
  --flag/--unflag --priority --tag/--untag --repeat/--no-repeat --notes
  --url; inline fields in add's text work too.
- The "view" setting (today | … | list:NAME | tag:NAME) is shared by all
  clients: the GUI and TUI open on it and save it as it changes (not for
  searches); `list` with no VIEW shows it, `add` without --list adds to it
  (else the first list); the CLI never writes it.
- Output looks like the Markdown files: "# List", "## Section",
  "- [ ] Title #tag ⏫ 🚩 📅 2026-10-03 17:30", notes indented, list name in
  brackets in mixed views; no ^id, no ➕ date. Colour only for list names and
  overdue dates, on a terminal (NO_COLOR respected).
- `edit NAME` with no field options (TUI: e) opens the reminder in
  $VISUAL/$EDITOR (else nano or vi) as YAML-style fields: title, done, due,
  time, repeat, priority, flagged, tags, list, section, url, notes (block),
  subtasks (Markdown lines, matched by title so they keep their notes). On a
  bad value, ask "Edit it again, or revert to how it was? [E/r]". No change
  or an emptied file cancels; saving is one undo step. `edit-list` (TUI: E)
  opens a copy of the list's file as it is; saving writes it as any change
  is written, merging with a sync that came in meanwhile (ours wins), and
  insists the front matter keeps `reminders: 1`.
- TUI: sidebar (bold "Reminders" title, groups as in the GUI and the same
  settings, lists in their colour, hidden entries marked) and a reminders
  pane in the Markdown form above, with the header count right-aligned and
  dimmed, shortened to "6/3" or left out when it doesn't fit. Keys are in
  docs/TERMINAL.md and docs/KEYS.md: ↑↓/j k, Tab or ←→ between panes,
  Enter, g go to, / search, : command line, n new, Enter/F2 edit title in
  place (a line editor shared with every prompt: Ctrl+A/E/U/K, Tab
  completes), e edit, E edit list, x/Space done, d due, t/T today/tomorrow,
  f flag, 0–3 priority, # tag, m move, J/K or Ctrl+↓/↑ move (sidebar
  entries too; Ctrl+Shift for groups), ]/[ indent/outdent, + or Shift+→/←
  fold, Delete, v / * / Esc marks (then the keys act on every marked
  reminder), c completed, h hide, H show hidden, N new list, O import, s / S
  sync, `,` settings in $EDITOR, u/r undo/redo, Ctrl+L redraw, ?/F1 help,
  q quit. The GNOME keys work where a terminal can send them (Ctrl+N/T/K/F/H
  /E/D/B/O, F2, Ctrl+arrows by terminfo kUP5 etc., Ctrl+PgUp/PgDn,
  Ctrl+Q/W). No Alt keys and no Ctrl+S (XOFF); clear IXON so Ctrl+Q reaches
  the app; Ctrl+Z suspends. `:` takes every CLI command (a command with no
  NAME acts on the marked reminders, else the selected one; one that edits
  is one undo step), plus :undo, :redo, :set (completed, sidebar,
  note-lines), :q; history in $XDG_STATE_HOME/reminders/command-history.
- setlocale before initscr; wide characters throughout (get_wch, WACS_*);
  set_escdelay(25); timeout(1000) so the loop polls sync and the folder each
  second; background sync with problems on the bottom line.

5. Docs: README.md (overview, build, install, layout), docs/USING.md (GNOME
guide), docs/TERMINAL.md (terminal guide), docs/KEYS.md (every key, GNOME |
Terminal | CLI | Action), docs/FORMAT.md (the format),
docs/settings.example.ini, backends/<id>/README.md, CLAUDE.md working notes
(plus one per client), INSTRUCTIONS.md (decisions and state of play),
TODO.md (the only to-do list).
````

### Directions

Build in this order, and check each stage before going on.

1. **Check the machine and ask before installing.** Needed: a C++23 compiler,
   CMake ≥ 3.25, Ninja, `gtk4-devel`, `libadwaita-devel`, `glib2-devel`,
   `ncurses-devel`, `libcurl-devel`, `libxml2-devel`; `git` and Python 3 at
   run and test time; `clang-format`.
2. **Write `docs/FORMAT.md` first.** Every later client depends on it.
3. **Core library and tests:** model → format (parse/serialize) → recurrence
   → merge → store → history → settings and paths → sources, the back end
   registry and `Library`. Aim for byte-for-byte round trips. Also run the
   tests under `valgrind` and with `-D_GLIBCXX_DEBUG`.
4. **Back ends:** local, then syncthing (test with a scratch folder holding
   `.stfolder`; fake Syncthing by writing conflict copies by hand), then the
   sync runner, then caldav and webdav against the fake DAV server, then git
   against a local bare repository.
5. **The shared `app` layer:** actions, selection, the sidebar model, view
   model, preferences, with unit tests.
6. **GTK helpers (`gtk_util.hpp`):** `Obj<T>`, `connect<Sig>()` (a signal
   handler's C signature must match the signal: `notify::` passes a
   GParamSpec), actions, toggles, timeouts, idle callbacks, shortcuts,
   `attach()` to tie a C++ object's lifetime to a widget.
7. **Test tools:** `tools/gui-test.sh`, the screenshot hook, then
   `tools/gui-keys.py`.
8. **App skeleton → views → dialogs → editing → undo**, wrapping every change
   so it records exactly one step; screenshot each with sample data.
9. **Keys, multi-select, copy/paste, drag and drop, sections, Go To, search,
   import/export, the Sources dialog.**
10. **Terminal client:** the command table and CLI first (quick to test from
    a shell, with `--json`), then the TUI on a private tmux server
    (`send-keys`, `capture-pane`; leave `TERM` alone, since
    `screen-256color` lacks the Ctrl+arrow capabilities), then the `:`
    prompt.
11. **Docs**, and a Release build with no warnings.

**Verifying UI work:** run the app only through `tools/gui-test.sh`. Drive it
with real keys and clicks (`STEPS=file`) or D-Bus actions (`gdbus call
--session --dest com.stephenhouser.Reminders --object-path
/com/stephenhouser/Reminders/window/1 --method org.gtk.Actions.Activate`),
and check the list files or the rendered PNG. Popovers and combo-row lists
don't show in the PNG (separate surfaces): test them by what choosing an item
does. Run a control for every key test, so a pass means something. Keep logic
in `core/` or `app/` where it can be unit-tested, and say plainly which parts
were only compiled.
