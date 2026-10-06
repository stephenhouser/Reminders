# Using Reminders (GNOME)

This guide covers the GNOME app (`Reminders`). For the terminal client
(`reminders`), see [TERMINAL.md](TERMINAL.md).

## Getting started

### 1. Pick a folder

Reminders keeps your lists in a folder, one Markdown file per list. To use them
on several devices, make it a [Syncthing](https://syncthing.net) folder, or put
it inside one, and share it with your other devices. Any folder works if you
only want Reminders on one computer.

### 2. Open it

The first time you start Reminders, choose **Choose Folder…** and pick the
folder. Reminders remembers it. To change it later, or to add more folders,
use **☰ → Sources…** (see [Sources](#sources)).

To open a different folder for one session without changing the saved one,
name it on the command line:

```sh
Reminders ~/Sync/Work
```

If Reminders is already running, it switches that window to the folder.

### 3. Make a list

**New List** (Ctrl+Shift+N, or the button at the bottom of the sidebar) asks
for a name, a colour and an icon (and, with several sources, which one it goes
into). The list becomes `<name>.md` in your folder.

## Reminders

### Adding

Click **New Reminder** at the bottom of a list (or press Ctrl+N), type, and
press Enter. The field stays open for the next one.

You can type fields straight into the title:

| Type | Sets |
|---|---|
| `#errands` | a tag |
| `📅 2026-10-03` or `📅 2026-10-03 17:30` | due date (and time) |
| `🚩` | flagged |
| `⏫` `🔼` `🔽` | priority high / medium / low |
| `🔁 every week` | repeat |
| `🔗 https://…` | link |

On GNOME, Ctrl+. (or Ctrl+;) opens the emoji picker. The details dialog and
the shortcuts below set the same fields without any emoji.

### Editing

- **Title:** click it, or select the reminder and press Enter or F2. Enter,
  Ctrl+S or clicking anywhere else saves; Esc puts back the old title.
  Clearing the title deletes the reminder.
- **Everything else:** double-click the reminder, the ✏ button (shown on hover), Ctrl+I, or **Details…**
  in the ⋮ menu. The details dialog has the title, notes, URL, date, time,
  repeat, flag, priority, list, tags and subtasks. **Done** or Ctrl+S saves;
  **Cancel** or Esc discards.
- **The reminder's menu:** **⋮**, or right-click (or long-press) anywhere on
  the reminder, title included, which opens it where you clicked: Mark as
  Completed, Details…, Flag, Due Today / Tomorrow, Copy, Move To, Indent /
  Outdent, Delete. While you're editing a title, right-clicking it gives the
  usual text menu (cut, copy, paste) instead.

### Completing

Click the circle, or press Space. Completed reminders fade out after a moment.
Show them again with **⋮ → Show Completed** (Ctrl+H). The header under the
list's name counts them: "6 Reminders / 3 Complete" (every reminder, subtasks
included, and how many of those are done).

Every view has a count under its name, of what it contains:

| View | Count |
|---|---|
| A list, a tag, All Reminders | "6 Reminders / 3 Complete" (the second part only when some are done) |
| Today, Scheduled, All, Flagged | "5 Reminders" (these only hold open reminders) |
| Completed | "2 Completed" |
| A search | "3 Results" |

- **Completing a reminder** also completes its subtasks.
- **Completing a repeating reminder** marks this occurrence done and adds the
  next one above it, with the next due date.

### Due dates, flags and priority

| | |
|---|---|
| Due today / tomorrow | Ctrl+T / Ctrl+Shift+T (keeps any time already set) |
| Flag / unflag | Ctrl+Shift+F, or click the flag at the right of the row (it shows when you point at an unflagged reminder) |
| Priority none / low / medium / high | Ctrl+0 / Ctrl+1 / Ctrl+2 / Ctrl+3 |

Overdue dates show in red. Priority shows as `!`, `!!` or `!!!` before the title.

### Subtasks

Subtasks are one level deep, as in Apple Reminders.

- **Make a reminder a subtask** of the one above: Ctrl+], or **⋮ → Indent**.
- **Make it top-level again:** Ctrl+[, or **⋮ → Outdent**.
- **Add subtasks** from the details dialog, under *Subtasks*.
- **Collapse or expand** with the ⌄/› button. Ctrl+E hides every reminder's
  subtasks, and pressing it again shows them all.

### Moving and reordering

- **Drag** a reminder within a list. The line shows where it will land: above
  or below a reminder, or at the end of a section when you drop on its "New
  Reminder" row.
- **Drag onto a list in the sidebar** to move it to that list.
- **⋮ → Move To** (or right-click) moves it to another list.
- **Drop a file** from another app to import its reminders (see [Importing
  reminders](#importing-reminders)).
- **Drop text** from another app (an editor, a browser, a chat) to make
  reminders of it (a link from a browser becomes its page title and
  address), read as pasted text is (see [Copying and
  pasting](#copying-and-pasting)): on a reminder in a list, above or below
  it (a line shows where); on a list in the sidebar, at its end; anywhere
  else, at the end of the list being shown (in a smart list, into your
  first list, made to show there). Undo with Ctrl+Z.
- **Drag reminders out** to another app: it gets them as text, the same
  Markdown Copy gives (a selection drags them all). They stay here.
- **Ctrl+↑ / Ctrl+↓**, or **Move Up / Move Down** in a reminder's **⋮** or
  right-click menu, moves it one place, across section boundaries (in a list, one reminder at a time).
- **The List field** in the details dialog moves it to another list.

### Copying and pasting

- **Ctrl+C** on a selected reminder (or **Copy** in its menu) copies it as
  the Markdown line from its file, with its notes and subtasks (no id), so it
  also pastes into an editor or a chat as a checklist.
- **Ctrl+X** (or **Cut** in its menu) copies it the same way and deletes it,
  as one step: Ctrl+Z, or Undo in the message that appears, puts it back.
- **Ctrl+V** while not typing in a text field pastes reminders:
  - **A copied reminder** (or any `- [ ] …` checklist lines) comes back with
    every field, notes and subtasks.
  - **A list**, lines with bullets (`- `, `* `, `• `) or numbers (`1. `,
    `2) `), becomes a reminder per line (a heading line above them too).
  - **Any other text** becomes one reminder: its first line the title, the
    rest the notes. Inline fields such as `#tag` or `📅 2026-10-05` in a
    title apply, as when typing.
  - **A web address** (`http://` or `https://`) in a title goes in the
    reminder's URL too, so it shows as a link. Pasted on its own it stays
    the title as well; with other words around it ("Example Page
    https://…", as a link dragged from a browser gives), it's taken out of
    the title. For one reminder with notes, an address alone on a line of
    the notes becomes the URL.
  - **The message after several lines** offers the other way: **Split into
    N** (a reminder per line) or **Combine into One**. It undoes the paste
    and adds them again, as one step.
  - **Where they go:** after the selected reminder, in its list and section;
    otherwise at the end of the list being shown. In a smart list they go
    into your first list, and are made to show there: due today in Today or
    Scheduled, flagged in Flagged, tagged in a tag's view.
  - Undo with Ctrl+Z.
- **⋮ → Paste** (or right-click → Paste) pastes the same way, after that
  reminder. It's greyed out while the clipboard holds no text.
- **Ctrl+Shift+V** (Paste Special) asks first, when the text has several
  lines: **One Reminder** or **N Reminders**.
- **Dropped text** from another app is read the same way, with the same
  message (see [Moving and reordering](#moving-and-reordering)).
- **In a text field** (a title being edited, New Reminder, search), Ctrl+X,
  Ctrl+C and Ctrl+V cut, copy and paste text as usual.

### Deleting

Press Delete, or **⋮ → Delete**. Undo from the message that appears, or with
Ctrl+Z.

A reminder's **⋮** menu also opens from the keyboard: **Menu** or
**Shift+F10** on the selected reminder, then the arrow keys and Enter.

### Selecting several reminders

Select several reminders to change them all at once:

- **Ctrl+click** a reminder (anywhere on its row) to add it to the selection
  or take it out.
- **Shift+click** selects every reminder from the last one clicked to this
  one; **Ctrl+Shift+click** adds that range to what's selected.
- **Shift+↑ / Shift+↓** extends the selection from the keyboard.
- **Ctrl+A** selects every reminder showing (not hidden completed ones, nor
  the subtasks of a collapsed reminder), wherever the focus is: just after
  opening a list from the sidebar, too. Only while you're typing (a title,
  New Reminder, search) does Ctrl+A select text instead.
- **A plain click** on a reminder's empty space selects just that one;
  clicking its title (to edit it) or its circle leaves nothing selected.
  **↑ / ↓** then move the selection along with the focus.
- **Right-click** (or ⋮) shows the reminder as selected while its menu is
  open, unless it's already part of a selection, which the menu then acts
  on.
- **Escape** or **Ctrl+Shift+A** (again, unless you're typing) clears it.
  So does clicking
  anywhere outside the reminders (the empty space below them, the header,
  the sidebar), or going to another list.

Selected rows are tinted, and with two or more the header says how many
("4 Selected"). The
usual keys and the **⋮** or right-click menu of a selected reminder then act
on all of them: Space, Ctrl+Shift+F, Ctrl+T / Ctrl+Shift+T, Ctrl+0 … Ctrl+3,
Ctrl+X, Ctrl+C, Delete, and in the menu **Mark as Completed** (or Not Completed)
and **Move To**. Dragging a selected reminder
drags them all; they land together, in their order. Each change is one step
for Ctrl+Z.

- **Complete and flag** set them all the same way: all completed (or all not
  completed, when they all already were); likewise flagged.
- **A selected reminder's subtasks** go with it when moving, copying or
  deleting, selected or not.
- **Clicking a selected reminder's circle** completes just that one.
- **Details, Indent / Outdent and Ctrl+↑ / Ctrl+↓** still work on the one
  reminder with the focus.

## Importing reminders

**☰ → Import…** (Ctrl+O) reads reminders from a file, and so does **dropping a
file** on the window, from Files or any other app. Dropped on a list in the
sidebar, it goes into that list; dropped anywhere else, into the list
you're viewing. You can still change the list before importing, and
several files dropped at once are imported one after another. Five kinds
work. Reminders
works out which one a file is from its name and content and shows it under
**Read As**, where you can change it.

- **An iCalendar file** (`.ics`), such as tasks exported from another
  reminders or calendar app. Each task's title, notes, completion, due date
  and time, priority, repeat, tags, link and subtasks come along. Its
  section comes too if the file came from Reminders. Events are left out;
  only tasks are imported.
- **A Markdown checklist** (`- [ ] …` lines), such as a list file from
  another Reminders folder or notes from another app. Everything comes
  along: fields, notes, subtasks, sections and the list's colour.
- **A todo.txt file** ([todotxt.org](http://todotxt.org)): `x` for done,
  `(A)`–`(C)` for high, medium and low priority, `+project` and `@context`
  as tags, `due:` dates, and the common extras `rec:` (repeat), `id:` and
  `p:` (a subtask's parent).
- **A CSV file** with a header row, such as a spreadsheet or another app's
  export. The columns are found by their names: **Title** (or Name, Task,
  Content), **Due Date**, **Priority**, **Tags** (or Labels), **Notes** (or
  Description), **Done**, **Flagged**, **Repeat**, **URL**, **Section**,
  **Parent** and others. Commas, semicolons and tabs all work as
  separators.
- **A plain text file**, one reminder per line. Bullets (`-`, `*`, `•`) and
  numbering (`1.`) are dropped. A `# Heading` line starts a section, and an
  indented line becomes a subtask of the line above. Inline fields work as
  in New Reminder: `Pay rent #home 📅 2026-10-31`.

Then:

- **Where they go:** choose the list under **Into**. The first choice is a
  new list named after the calendar (or the file); with several sources
  there's one for each ("New List "Groceries" in Work"), starting with the
  source you're viewing. A list that already has that name is chosen to
  begin with.
- **Importing the same file again** doesn't double anything. Reminders that
  have ids (from a calendar, a Reminders list, or an export from here) are
  recognised wherever they have moved since. Others are recognised by their
  titles: one is skipped when the list already has an open reminder of that
  name. If nothing is new, no new list is made.
- **Import Duplicates** turns that off: reminders that are already here are
  added again as copies, with ids of their own. Use it to copy a list, or
  to bring back an export as a second set.
- **Undo** (Ctrl+Z) takes the whole import back.

In a terminal: `reminders import FILE [--list LIST] [--format F]`, or `O`
(Ctrl+O) in the interactive interface (see [TERMINAL.md](TERMINAL.md)).

## Exporting lists

**☰ → Export…** asks which lists to export, ticked in a list (**All
Lists** ticks or clears them all), and in which format. One list is saved
as a file you name; several are saved into a folder you choose, a file
each, or with **Compressed Archive** into one `.zip` file. **⋮ → Export…** on a list (or right-click it in the sidebar) opens
the same dialog with that list ticked. The formats are the ones Import
reads:

- **Markdown:** the list file itself, with everything. Import it into
  another Reminders folder, or keep it as a copy.
- **Plain Text:** a line per open reminder, written as you'd type it in New
  Reminder (`Pay rent #home 📅 2026-10-31`), with subtasks indented and
  sections as `# Headings`. **Include Completed** adds the completed ones.
  Notes are left out.
- **todo.txt** (`List.todo.txt`): a line per reminder, completed ones too,
  for todo.txt apps. Due times are written as `time:`, flags as `flag:yes`
  and links as `url:`. Notes and sections are left out, and so is an
  "every weekend" repeat, which todo.txt can't express.
- **CSV:** a row per reminder and subtask, with a header row (List,
  Section, Title, Done, Due Date, Due Time, Priority, Flagged, Tags, Repeat,
  URL, Notes, Completed, Created, ID, Parent ID), for spreadsheets and other
  apps.
- **iCalendar** (`.ics`): the list as tasks for other calendar and
  reminders apps, named after the list and in its colour. Notes, subtasks,
  repeats, flags and sections all come along.

An exported list imports back as it was, less what its format can't hold.
Every format but plain text keeps the reminders' ids, so importing an
export where the list still is adds nothing twice (unless you ask for
duplicates). Exporting several lists names each file after its list, as `source-List` where two sources have a list of the
same name, and replaces files of the same name in that folder.

In a terminal: `reminders export LIST [--format F] [-o FILE]`, or without
LIST, `reminders export -o FOLDER` for every list.

## Sections

A list can be divided into sections (they are `## Headings` in the file).

- **Add one:** **⋮ (top right) → Add Section…**
- **Rename or delete one:** the ⋮ next to the section heading. Deleting lets you
  keep its reminders, which move to the section above, or delete them with it.

## Smart lists, tags and search

| Sidebar entry | Shows |
|---|---|
| **Today** | Open reminders due today or overdue |
| **Scheduled** | Open reminders with a date, grouped by day |
| **All** | Every open reminder, grouped by list |
| **Flagged** | Open flagged reminders |
| **Completed** | Completed reminders, newest first |
| **#tag** | Reminders with that tag (one entry per tag in use) |

- **Search** (Ctrl+F, or 🔍) looks in titles and notes. Press Enter or ↓ to
  move into the results.
- **Go To** (Ctrl+K) jumps to any list, smart list or tag by typing part of
  its name. The last entry searches for what you typed.
- **Ctrl+Page Down / Ctrl+Page Up** step to the next or previous sidebar entry.
- **Enter** on a sidebar entry opens it and moves the focus to its first
  reminder (or New Reminder, in an empty list), ready for the reminder keys.

## Undo

Ctrl+Z undoes the last change, and Ctrl+Shift+Z redoes it. That covers edits,
completions, moves, deletions, sections, and creating, renaming or deleting
lists. If a list changed on another device in the meantime, undo reverses only
your change and keeps theirs. The history lasts until you quit or change
folders.

## Notifications

Reminders with a due time notify you at that time. All-day reminders notify at
09:00. Clicking the notification opens the reminder. Notifications only appear
while Reminders is running.

## Keyboard shortcuts

Press Ctrl+? in the app for this list.

**Reminders** (on the selected reminder; click a row's empty space or use ↑/↓ to select)

| Shortcut | Action |
|---|---|
| Ctrl+N | New reminder |
| Space | Complete / not complete |
| Enter, F2 | Edit title |
| Ctrl+S / Esc | Save / cancel while editing |
| Ctrl+I, Alt+Enter | Details |
| Menu, Shift+F10 | The reminder's ⋮ menu |
| Ctrl+Shift+F | Flag / unflag |
| Ctrl+T / Ctrl+Shift+T | Due today / tomorrow |
| Ctrl+0 … Ctrl+3 | Priority none / low / medium / high |
| Ctrl+] / Ctrl+[ | Indent / outdent |
| Ctrl+↑ / Ctrl+↓ | Move up / down (in a list) |
| Ctrl+X | Cut (copy, then delete) |
| Ctrl+C | Copy (as Markdown) |
| Ctrl+V | Paste reminders (outside a text field); ⋮ → Paste puts them after that reminder |
| Ctrl+Shift+V | Paste Special: one reminder, or one per line |
| Delete | Delete |

**Selecting several reminders** (see [Selecting several reminders](#selecting-several-reminders))

| Shortcut | Action |
|---|---|
| Ctrl+A | Select all |
| Ctrl+click | Add to / remove from the selection |
| Shift+click, Shift+↑ / Shift+↓ | Select a range |
| Esc, Ctrl+Shift+A | Clear the selection |

**Lists**

| Shortcut | Action |
|---|---|
| Ctrl+K | Go to… |
| Enter (in the sidebar) | Open the entry and move into its reminders |
| Ctrl+Page Down / Ctrl+Page Up | Next / previous sidebar entry |
| Ctrl+↑ / Ctrl+↓ | Move the sidebar entry up / down |
| Ctrl+Shift+↑ / Ctrl+Shift+↓ | Move its sidebar group up / down |
| Ctrl+Shift+N | New list |
| Ctrl+H | Show / hide completed |
| Ctrl+Shift+H | Show / hide hidden lists, smart lists and tags |
| Ctrl+E | Show / hide all subtasks |

**General**

| Shortcut | Action |
|---|---|
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo |
| Ctrl+F | Search |
| Ctrl+B, F9 | Show / hide sidebar |
| F10 | Main menu |
| Ctrl+R | Sync all (with sources that sync) |
| Ctrl+O | Import… |
| Ctrl+, | Settings |
| Ctrl+? | Keyboard shortcuts |
| Ctrl+W / Ctrl+Q | Close window / quit |

## Editing the files by hand

The files are meant to be edited by hand, and changes show up in the app
straight away. The full syntax is in [FORMAT.md](FORMAT.md). The short version:

```markdown
---
reminders: 1              ← required: marks the file as a list
color: green              ← red orange yellow green cyan blue indigo purple pink brown gray
icon: home                ← list bookmark cart gift home work school calendar flag star …
order: 1                  ← optional position in the sidebar
---
- [ ] Water plants 🔁 every 3 days 📅 2026-10-02
- [ ] Call plumber 🚩
  Notes go on indented lines under the reminder.
  - [ ] A subtask

## A section
- [x] Done thing ✅ 2026-09-30
```

- **No ids needed.** Lines you write can leave off the `^id`; the app adds one
  the next time it saves that list for a real change. Leave existing ids alone,
  because they let devices match reminders when merging.
- **Your formatting is kept.** A line keeps its exact text until you change
  that reminder in the app.
- **Other content is kept too.** Other headings, paragraphs and unknown
  front-matter keys stay where they are.
- **Turning a Markdown file into a list:** if a file has a checklist but no
  `reminders: 1`, a banner offers to turn it into a list. Choose which files in
  **Review…**; files you switch off aren't suggested again on this computer.
- **Obsidian:** the folder works as an Obsidian vault, and the Obsidian Tasks
  plugin understands the same dates, priorities and repeats.

## Sync, conflicts and what's in the folder

Syncing is up to the source's back end. For a Syncthing folder, Reminders
merges the conflict copies Syncthing makes and keeps its records in
`.reminders/` (kept out of the sync); see [the Syncthing back end](../backends/syncthing/README.md).
Server back ends merge with the server instead; see the pages linked from [Sources](#sources).

## Where files are kept

Reminders follows the XDG base directory rules: each place below moves with
its variable when that's set to an absolute path.

| What | Where |
|---|---|
| Settings | `$XDG_CONFIG_HOME/reminders/settings.ini` (`~/.config/reminders/settings.ini`) |
| CalDAV and WebDAV sources' lists (local copies) | `$XDG_DATA_HOME/reminders/caldav/NAME/` or `…/webdav/NAME/` (`~/.local/share/…`) |
| This computer's records for a Syncthing source (merge bases) | `.reminders/DEVICE/` in its folder, kept out of the sync by `.stignore` |
| … for other sources (CalDAV and WebDAV sync records) | `$XDG_STATE_HOME/reminders/DEVICE/NAME/` (`~/.local/state/…`) |
| Things found again if lost (CalDAV servers' calendar addresses) | `$XDG_CACHE_HOME/reminders/` (`~/.cache/…`) |

`DEVICE` is the computer's name plus a short code, so computers sharing a
home folder keep separate records. Deleting the state or cache folders is
safe: merges after that keep everything rather than guess at deletions, and
CalDAV and WebDAV sources fetch their lists again (and merge them with
the local copy).

## The settings file

`$XDG_CONFIG_HOME/reminders/settings.ini` (`~/.config/reminders/settings.ini`) is shared by the GNOME app and the terminal
client. Most of it is filled in for you; `show-key-numbers` (terminal only) is only set here.
In the app, **main menu → Settings…** (Ctrl+,) opens it in your default text editor
(creating it if needed), and changes apply as soon as you save. In the
terminal client, `,` (Ctrl+,) opens it in your editor and applies the changes when you
quit the editor.
[settings.example.ini](settings.example.ini) lists every setting with its
default, ready to copy. Settings go under the `[general]` line, except
sources, which have a section each (below).

```ini
[general]
# The source new lists go into (Default Source in Source Info…)
default-source=personal
# The list that last had focus
view=list:Groceries
# Whether the sidebar is shown (Ctrl+B in either app)
show-sidebar=true
# Terminal client: show each sidebar entry's jump key ((1) …)
show-key-numbers=true
# The order of the sidebar's groups
sidebar-order=smart-lists, local-lists, tags
# Which smart lists to show, in this order
smart-lists=today, scheduled, all, all-reminders, flagged, completed
# Lists and tags hidden from the sidebar, and whether to show them anyway
lists-hidden=Work
tags-hidden=frontend
show-hidden=false
# The tags' order: these first, then the rest alphabetically
tags-order=work, errands
# A tag's colour and icon (Tag Info…)
tag-color.errands=orange
tag-icon.errands=cart
# How each group appears: visible, collapsible or hidden
# (your lists can be visible or collapsible, not hidden)
smart-lists-display=visible
local-lists-display=visible
tags-display=visible
# A reminder row's buttons (flag, Details, ⋮): shown on hover, or always (dimmed)
row-buttons=hover
# How many lines of a reminder's notes the lists show (0 or unset: all)
note-lines=0
# Set by the app when you fold a collapsible group
smart-lists-collapsed=false
lists-collapsed.personal=false
tags-collapsed=false
```

Comments go on their own lines, starting with `#`.

`show-key-numbers` labels the terminal client's first ten sidebar entries with
the key that jumps to them, as a prefix, e.g. `(1)Today`, for the keys `1`–`9`
and `0`. The GNOME app has no such keys (the HIG gives Ctrl+number to other
uses), so it ignores the setting. It accepts `true`, `yes` or `1`, and is off when missing.

`row-buttons` sets when a reminder row's buttons (its flag, Details and ⋮)
show in the GNOME app: `hover` (the default) while you point at the row or
reach it with the keyboard, `always` all the time, dimmed until you point at
the row. Everything they do is also in the row's right-click menu.

`note-lines` sets how many lines of a reminder's notes show under it in the
lists, in the GNOME app and the terminal client, with `…` when there are more
(`note-lines=2`). `0`, or leaving it out, shows them all. Details always shows
the whole note.

**Sidebar groups.** The sidebar has the smart lists, your lists and tags.
Your lists are one group per [source](#sources): with one source it's
"My Lists"; with several, each source's group is headed by its title.

`sidebar-order` sets their order, e.g. `sidebar-order=local-lists, tags,
smart-lists`. With several sources, `lists:NAME` places one source's group
(`sidebar-order=lists:work, smart-lists, lists:home, tags`), and `local-lists`
stands for the sources not named. A group you leave out or misspell goes at
the end, so a typo can't make your lists disappear. You can also rearrange them without editing
the file:

- **In the app:** drag a group's heading with the mouse and drop it on
  another group: on that group's top half to go above it, its bottom half to
  go below (a line shows where). Or right-click (or long-press) a group's
  heading and choose Move Up or Move Down, or press Ctrl+Shift+↑ /
  Ctrl+Shift+↓ on any of its entries. The group at the top has no heading
  unless it's collapsible, so move it by dragging another group above it
  (or with the keys). The same menu's **Collapsible** item
  switches the group between `visible` and `collapsible`.
- **In the terminal client:** select a sidebar entry and press Ctrl+Shift+↑ /
  Ctrl+Shift+↓ (or `K` / `J` on the group's heading).

Right-clicking (or long-pressing) one of your lists in the sidebar shows the
same menu as **⋮** in the header, for that list, without opening it: Show
Completed, Add Section…, List Info…, Move Up / Move Down, Hide and Delete
List…. (Show Completed is one setting for the whole window, as in the ⋮
menu.)

`smart-lists-display`, `local-lists-display` and `tags-display` each take one of
these values:

| Value | Shows the group |
|---|---|
| `visible` (default) | With a plain heading, except the group at the top, which has none |
| `collapsible` | Under a heading you click (or press Enter on) to fold or unfold it |
| `hidden` | Not at all (not allowed for My Lists) |

So the group at the top has no heading unless it's collapsible, and the
groups below it always have one.

The app remembers a folded group (`smart-lists-collapsed`,
`lists-collapsed.NAME` for each source, `tags-collapsed`); this only applies
in `collapsible` mode. `local-lists-display` applies to every source's group.

- **`smart-lists`** chooses which smart lists appear and in what order, e.g.
  `smart-lists=today, flagged`.
- **Numbering:** Ctrl+Page Up/Down, and the terminal client's number keys and
  their labels, follow what's showing, in order. So hiding or folding the
  smart lists makes your lists start at `1`.
- **Go To (Ctrl+K)** still finds entries in folded groups, but not in hidden
  ones.

**Hiding entries.** Right-click (or long-press) any smart list, list or tag
in the sidebar and choose **Hide** to take it out of the sidebar. Its
reminders still appear in Today, All, search and so on; only the sidebar
entry goes.

- **Where it's saved:** `smart-lists` (a hidden smart list is left out of it),
  `lists-hidden` and `tags-hidden`. They're settings for this computer, not
  synced.
- **Seeing them again:** the main menu's **Show Hidden Lists** (Ctrl+Shift+H) shows hidden
  entries dimmed; choose **Show** on one to bring it back. It's saved as
  `show-hidden`.
- **The terminal client** follows the same settings.

**Reordering entries.** The same keys work in the app and the terminal
client:

| Keys | Moves |
|---|---|
| Ctrl+↑ / Ctrl+↓ | The selected smart list, list or tag, within its group (in the terminal, also `K` / `J`); on a group's heading, the group |
| Ctrl+Shift+↑ / Ctrl+Shift+↓ | Its whole group |

In the app, an entry's right-click menu has **Move Up** / **Move Down** too,
and a heading's moves its group.
 You can also drag an entry
with the mouse to another place in its group: a line shows where it will
go. Entries stay in their own group (a list can't be dragged into another
source's group, or among the tags); dropping a reminder on a list still
moves the reminder into it. The order is kept in
settings.ini, for this computer only: smart lists in `smart-lists`, your
lists in `lists-order` and tags in `tags-order`. Lists and tags not named
there follow the others (lists in their usual order, tags alphabetically).
Moves skip hidden entries.

**Tag colours and icons.** Right-click a tag and choose **Tag Info…** to give
it a colour and icon, as for a list. They're saved in settings.ini
(`tag-color.NAME`, `tag-icon.NAME`), so they're per computer.
- **A hidden last list:** if the list you last had open is now hidden, the app
  opens on Today or the first entry showing.

For one run, `reminders --show-key-numbers` or `--hide-key-numbers` on the
terminal client's command line overrides it without changing the file.

### Sources

Where the lists come from is a *source*: a folder and the back end that
handles it, in a section of its own at the end of the file:

```ini
[source.personal]
backend=syncthing
folder=~/Sync/Reminders
```

A `folder=` can start with `~` (your home folder) and use environment
variables (`$HOME/Sync`, `${XDG_DATA_HOME}/lists`); a relative one is taken
from your home folder (`Sync/Reminders`). The app writes folders in your home
folder as `~/…`, so the file works on another computer with a different home.

| Back end | What it does |
|---|---|
| `local` | Just the folder: list files are read and saved as they are, and changes made by other programs still show up (see [Local Folder](../backends/local/README.md)). |
| `syncthing` | The folder is synced by Syncthing. Conflict copies are merged; per-device records live in `<folder>/.reminders/`, which `.stignore` keeps out of the sync (see [Syncthing](../backends/syncthing/README.md)). |
| `caldav` | Task lists on a CalDAV server (see [CalDAV accounts](../backends/caldav/README.md)). The folder is a local copy, kept in step with the server. |
| `webdav` | List files in a folder on a WebDAV server (see [WebDAV folders](../backends/webdav/README.md)). The folder is a local copy, kept in step with the server's. |
| `git` | A folder in a git repository (see [Git repositories](../backends/git/README.md)). Changed lists are committed, and pulled and pushed with the remote. |

- **☰ → Sources… → Add Source…** asks for the source's **Name**, its
  **Type** (Local Folder, the starting choice, Syncthing, CalDAV, WebDAV or Git) and its
  **Folder**. Choosing a folder picks the type for you (Syncthing inside a
  Syncthing folder, one with `.stfolder`; Git inside a git repository;
  else Local Folder); you can change it. For Git, a **Repository**
  section has the rest (see [Git repositories](../backends/git/README.md)). For
  CalDAV and WebDAV the folder is the **Local Copy**, which starts out in
  `$XDG_DATA_HOME/reminders/caldav/NAME` (or `webdav/NAME`), and a
  **Server** section asks for the account (see [CalDAV
  accounts](../backends/caldav/README.md) and [WebDAV folders](../backends/webdav/README.md)). The settings
  section is named after the Name (`[source.NAME]`, lower case), else the
  server or the folder.
- **Choose a source in Sources…** for **Source Info…**: the same rows, its
  **Default Source** switch (where new lists go), and **Remove Source…**,
  which takes it out of the app. Left at that, its folder and files stay
  as they are. Tick **Erase all source data** to also erase everything it
  has on this computer, for good: its whole folder (the local copy, the
  clone, or the list folder with everything in it) and the app's records
  for it. Remote copies are never touched: a CalDAV or WebDAV server keeps
  its lists, a git remote keeps its history (commits not pushed yet are
  lost). It's ticked to start with for CalDAV and WebDAV, whose local copy
  is only a copy. **Syncthing:** if Syncthing still shares the folder, it
  erases it on your other devices too, so remove the folder from Syncthing
  first. The app won't erase the filesystem's root, your home folder, or a
  folder holding it.
- **Right-click a source's group heading** for **New List…** in that source,
  **Sync Now** (CalDAV, WebDAV and git sources) and its **Source Info…**.
- **`reminders folder PATH`** (and the first-run Choose Folder…) sets the
  default source's folder, creating the source if there's none.
- **`default-source=`** in `[general]` is the default source; without it,
  the first one.
- **A folder given on the command line** uses its source's settings if it is
  one, else the back end it needs, for that run only.
- **Several sources** are open at once: each gets its own sidebar group, and
  the smart lists, tags and search cover all of them. Reminders can be
  moved (dragged, or the List field in Details) between sources. Undo works
  across them.
- **`title=`** in a source's section (Title in Source Info…) is the heading
  shown for its group; without it, the name capitalised (`personal` →
  "Personal").
- **New lists** go into the source of the list you're viewing, else the
  default one; New List's **Source** row (shown with several sources) and
  Import's "New List … in …" choices can pick another. In the terminal:
  `reminders new-list NAME --source NAME`, else the default source.
- **List names** only need to be unique within a source. Where two sources
  have a list of the same name, it's shown as `source/name` (in mixed views,
  the Details list field and the terminal), and settings name it that way:
  `view=list:home/Todo`, `lists-order`, `lists-hidden`. A bare `Todo`
  still works when only one source has it.
- **Settings from before sources** had `folder=` in `[general]`; it's no
  longer read. Add the folder again with ☰ → Sources… → Add Source… (or
  `reminders folder PATH`), or write the section above by hand.

### CalDAV, WebDAV and Git

Each back end has its own page: [Syncthing](../backends/syncthing/README.md),
[Local Folder](../backends/local/README.md), [CalDAV](../backends/caldav/README.md),
[WebDAV](../backends/webdav/README.md) and [Git](../backends/git/README.md).
They give the settings each one takes, when it syncs and how it merges.

## Troubleshooting

| Problem | What to do |
|---|---|
| A Markdown file doesn't appear as a list | It needs `reminders: 1` in its front matter. Use the banner's **Review…**, or add it by hand. |
| Changes from another device don't appear | Check that Syncthing is running and the folder is up to date (Syncthing's web UI at http://127.0.0.1:8384). Reminders reloads as soon as files change. |
| A `.sync-conflict-` file stays in the folder | Reminders merges conflict copies of lists only. Copies of other files are left for you. |
| Text dropped from another app says "Couldn't read the dropped text" | Start Reminders from a terminal with `REMINDERS_DEBUG_DND=1 Reminders`, drop again, and look at what it prints: the formats the other app offered and what could be read from each. |
| A reminder came back after you deleted it | Without a merge base (for example the first sync on a new computer), conflict merges keep reminders rather than risk losing them. Delete it again. |
| Settings | See [The settings file](#the-settings-file). |
