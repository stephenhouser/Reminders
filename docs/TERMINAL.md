# Using Reminders in a terminal

`reminders` (lowercase) is the terminal client: a command-line interface for
quick changes and scripts, and an interactive full-screen interface (TUI) when
you run it with no command. It works on the same folder as the GNOME app
(`Reminders`), uses the same settings, and runs without a graphical desktop, so
it also suits SSH sessions and headless machines running Syncthing.

## The folder

`reminders` uses the folder chosen in the GNOME app (saved in
`~/.config/reminders/settings.ini`, or under `$XDG_CONFIG_HOME`). To set or check it from the terminal:

```sh
reminders folder ~/Sync/Reminders     # set (shared with the app)
reminders folder                      # show
reminders --folder ~/Sync/Work list   # use another folder for one command
```

The folder belongs to the default *source*, which also says how it's synced
(`syncthing` or `local`); `reminders folder PATH` picks that by itself. With
several sources (see [sources](USING.md#sources)), every command and the
interactive interface use all of them:

- `reminders lists` shows each source's lists under its title.
- A list is named by its name, or `source/name` when two sources have a list
  of that name (`reminders list work/Todo`; a bare `Todo` then asks which).
- `reminders new-list NAME --source S` creates it in source S; without
  `--source`, in the default source. In the interactive interface, `N`
  creates it in the source whose group is selected.

[CalDAV](../backends/caldav/README.md), [WebDAV](../backends/webdav/README.md) and
[git](../backends/git/README.md) sources are synced with their servers
around each command: before it runs, and after one that changes something.
`--offline` skips that and works on the local copy. `reminders sync [SOURCE]`
syncs now and says what changed. The interactive interface syncs in the
background (on start, every `interval=` minutes, and shortly after a change)
and shows any problem at the bottom of the screen; `s` syncs the selected
source now and `S` every source, showing "Synced" when done.

## Commands

```
reminders                        open the interactive interface
reminders lists                  lists, with how many reminders are open
reminders list [VIEW] [-a]       reminders in VIEW (default: the last list you had open)
reminders show NAME              everything about one reminder
reminders add TEXT… [FIELDS]     add a reminder
reminders edit NAME [FIELDS]     change one
reminders done NAME              complete
reminders undone NAME            mark as not completed
reminders move NAME --to LIST [--section S]
reminders delete NAME [--yes]
reminders search TEXT
reminders new-list NAME [--color C] [--icon I] [--source S]
reminders import FILE [--list LIST] [--source S] [--format F] [--duplicates]
reminders export [LIST] [--format F] [-o FILE] [-a]
reminders folder [PATH]
reminders sync [SOURCE]
```

**VIEW** is a list name (case doesn't matter), `today`, `scheduled`, `all`,
`all-reminders` (open and completed), `flagged`, `completed` or `#tag`. `-a`
includes completed reminders.

**The last list you had open** is shared by the GNOME app, the TUI and the CLI
(the `view` line in `$XDG_CONFIG_HOME/reminders/settings.ini`):

- **The TUI** opens on it and updates it as you move around. Searches don't
  count.
- **`reminders list`** with no VIEW shows it.
- **`reminders add`** without `--list` adds to it. If it isn't a list (Today,
  say), `add` uses your first list.
- **The CLI never changes it**, so scripts don't move your default around.
- **`--folder` with a different folder** ignores it and leaves it alone.

**NAME** is a reminder's title, or enough of it to pick it out; quotes are
optional. The best match wins: an exact title, then one starting with NAME,
then one containing it, then one containing all of NAME's words in any order
(`pay rent` finds "Pay the rent"). Open reminders win over completed ones.

- `--in LIST` looks in one list only: `reminders done milk --in Groceries`.
- If several reminders still match, you're shown them, numbered, and asked
  which one. In a script (no terminal to ask), they're listed with their lists
  and the command fails, so add `--in` or more of the title.

**FIELDS** (for `add` and `edit`):

| Option | Meaning |
|---|---|
| `--title TEXT` | Title |
| `--list LIST` | List (`add`: which list; `edit`: move it there). Default for `add`: the last list you had open |
| `--section NAME` | Section to add into |
| `--parent NAME` | Add as a subtask of NAME |
| `--in LIST` | Find NAME (or `--parent NAME`) in this list only |
| `--due DATE` | `today`, `tomorrow`, a weekday (`fri`, `friday`: the next one), `+3d`, `+2w`, `+1m`, or `2026-10-31` |
| `--time HH:MM` | Due time (sets the date to today if there is none) |
| `--no-due` | Remove the date and time |
| `--flag` / `--unflag` | Flag |
| `--priority P` | `none`, `low`, `medium` or `high` |
| `--tag TAG` / `--untag TAG` | Add or remove a tag (repeatable) |
| `--repeat RULE` / `--no-repeat` | e.g. `"every week"`, `"every 2 months"`, `"every weekday"` |
| `--notes TEXT` | Notes |
| `--url URL` | Link (`--url ""` removes it) |

The inline fields from the file format also work in the text you add:
`reminders add "Call dentist #health 📅 2026-10-12"`.

**`import`** reads reminders from a file into LIST: the tasks in an
iCalendar file (`.ics`; events are skipped), a Markdown checklist, a
todo.txt file, a CSV file with a header row, or plain text with one
reminder per line (see [Importing reminders](USING.md#importing-reminders)).
The kind is found from the file's name and content; `--format md|txt|
todo.txt|csv|ics` says which instead, and the message says which was used. Without `--list` they go to a
list named after the calendar, else after the file. The list is made (in
`--source`, else the default source) if there's none. Importing a file
twice doesn't double anything: reminders with ids (from a calendar, a
Reminders list or an export) are skipped if they're anywhere already,
others if the list has an open reminder of that title. If nothing is new,
no list is made. `--duplicates` adds them all the same, as copies with new
ids. With `--json`, the result is
`{"list", "created", "added", "already", "skipped"}`.

**`export`** writes a list in the same formats (see [Exporting
lists](USING.md#exporting-lists)): `md`, the list file itself (the
default); `txt`, a line per open reminder (`-a` adds completed ones);
`todo.txt`; `csv`; or `ics`, iCalendar tasks. Without `--format`, the name
of `-o FILE` says which (`todo.txt` and `*.todo.txt` are todo.txt). Without
`-o` (or with `-o -`) the export goes to the terminal; with a folder, it's
written there as `LIST.md` (or `.txt`, `.todo.txt`, `.csv`, `.ics`).
Without LIST, every list is exported into the folder `-o` names, or
into one `.zip` archive when `-o` names a `.zip` file. With
`--json`, the result is `{"list", "format", "file"}`, or for every list
`{"format", "files"}`.

### Examples

```sh
reminders add "Pay rent" --list Home --due +3d --time 09:00 --priority high --flag
reminders list                     # today and overdue
reminders list groceries -a        # a list, including completed ones
reminders done milk                # by name: no quotes needed
reminders done milk --in Work      # when more than one list has a Milk
reminders edit pay rent --due tomorrow --tag bills --notes "by transfer"
reminders move bread --to Work
reminders list '#errands'
reminders search dentist
reminders import ~/Downloads/tasks.ics --list Work
reminders import packing.txt       # one reminder per line, into a new list "packing"
reminders export Groceries --format txt | wl-copy   # the open items, to paste elsewhere
reminders export Work -o ~/Backup/                  # ~/Backup/Work.md
reminders export -o ~/Backup/ --format csv          # every list, a .csv each
reminders export -o ~/lists.zip                     # every list, in one archive
reminders import ~/Downloads/todo.txt --list Inbox  # a todo.txt file
```

### Scripting

- `--json` prints machine-readable output for `list`, `show`, `search`,
  `lists`, `add`, `edit`, `done` and `move`. Each reminder includes `id`,
  `list`, `section`, `parent`, `title`, `done`, `due`, `time`, `completed`,
  `flagged`, `priority`, `tags`, `repeat`, `url` and `notes`.
- Colours appear only when printing to a terminal; `--no-color` or the
  `NO_COLOR` environment variable turns them off. `--json` and `--no-color`
  can go anywhere on the command line.
- `delete` asks for confirmation when run interactively; `--yes` skips it.
- Exit status: 0 success, 1 error (e.g. nothing matched), 2 bad usage.

```sh
# Titles of everything due today, one per line
reminders --json list today | jq -r '.[].title'
```

### What the output looks like

Reminders are shown as they are written in their files, with the list as a
`#` heading and sections as `##` headings. (In the interactive interface the
heading has the same count as in the app, dimmed against the right edge:
`6 Reminders / 3 Complete`, shortened to `6/3` when that doesn't fit beside
the title, and left out when even that doesn't.)

```
# Groceries
- [ ] Milk #errands ⏫ 🚩 📅 2026-10-03 17:30
  2% if they have it
- [x] Eggs ✅ 2026-09-30

## Party
- [ ] Cake 🔼
  - [ ] Candles
```

Views that mix lists (Today, Scheduled, Flagged, search) add the list name in
brackets: `- [ ] Book flights 📅 2026-10-01  (Work)`. Overdue dates are red.
The `^id` at the end of each line in the file isn't shown; `--json` includes it
for scripts.

## The interactive interface

Run `reminders` with no command. The sidebar (smart lists, your lists, tags)
is on the left, and the selected view on the right. When a group is set to
`collapsible`, move onto its heading and press Enter or Space to fold or
unfold it. Alt+↑ / Alt+↓ (or `K` / `J`) move the selected entry within its group, and
Alt+Shift+↑ / Alt+Shift+↓ move the group, as in the app. Which groups show, and in what order,
follow the same settings as the app (see
[the settings file](USING.md#the-settings-file)). It opens on the last list
you had open, here or in the GNOME app. Changes made elsewhere (in
the GNOME app, on another device through Syncthing, or in an editor) appear
within a second.

| Key | Action |
|---|---|
| ↑ ↓ / j k, Page Up/Down | Move |
| Tab, ← → | Switch between the sidebar and the reminders (`l` also moves to the reminders) |
| Enter | Open the selected sidebar entry |
| 1–9, 0 | Jump to sidebar entry 1–10 (Today, Scheduled, All, All Reminders, Flagged, Completed, your lists); `0` is the 10th |
| g | Go to a list or tag by typing part of its name |
| / | Search |
| c | Show / hide completed |
| J / K, Alt+↓ / Alt+↑ | In the sidebar: move the selected smart list, list or tag down / up in its group (on a heading: the group) |
| Alt+Shift+↓ / Alt+Shift+↑ | In the sidebar: move the selected entry's group down / up |
| s | Sync the selected source now: the selected list's, else the list showing's, else the default source (CalDAV, WebDAV and git sources; like Sync Now in the app) |
| S | Sync every source now (like ☰ → Sync All in the app) |
| Ctrl+S | Edit the settings file in your editor (applied when you quit it) |
| h | Hide the selected list, smart list or tag from the sidebar; on a hidden one, show it again |
| H | Show / stop showing hidden lists, smart lists and tags (dimmed), like the app's Show Hidden Lists |
| N | New list |
| I | Import a file: asks for its path (`~` works; others are from where you started `reminders`), then the list, filled in with the list showing, else a new one named after the calendar or the file. A name is looked for in the selected source first, then in any (`source/name` picks one); a new list goes in the selected source. One undo step |
| u / r | Undo / redo |
| ? | Help (scrolls with ↑↓ when the terminal is short; any other key closes it) |
| q | Quit |

On the selected reminder:

| Key | Action |
|---|---|
| x, Space | Done / not done |
| n | New reminder (inline fields work; in Today it gets today's date, in Flagged a flag) |
| Enter, F2 | Edit the title in place (see below) |
| e | Edit every field in your editor (see below) |
| d | Due date: `today`, `tomorrow 09:00`, `fri`, `+3d`, `2026-10-31`, or `none` |
| t / T | Due today / tomorrow |
| f | Flag / unflag |
| 0 1 2 3 | Priority none / low / medium / high |
| # | Add a tag (`-tag` removes it) |
| m | Move to another list (type the start of its name) |
| J / K | Move down / up (in a list) |
| ] / [ | Indent / outdent (in a list) |
| Delete | Delete (asks first; undo with u) |

**Several reminders at once.** Mark them, then use the keys above:

| Key | Action |
|---|---|
| v | Mark / unmark the selected reminder, and go to the next |
| * | Mark every reminder showing |
| Esc | Unmark them all |

Marked reminders have a `*` in the margin, and the title line says how many
("3 marked"). While any are marked, x / Space, f, t / T, d, 0–3 (and
Alt+0–3), #, m and Delete act on all of them, each as one undo step (u).
Completing and flagging set them all alike: all done (or all not done, when
they all already were); likewise flagged. A marked reminder's subtasks go
with it when moving or deleting. The other keys (Enter, e, J / K, ] / [)
still work on the selected reminder. Going to another list unmarks them, as
does completing them when completed reminders are hidden. The same as
selecting several in the GNOME app, with v instead of Ctrl+click.

**Editing text**, whether a title in place or a prompt at the bottom (add,
search, go to, due date):

| Key | Action |
|---|---|
| Enter, Ctrl+S | Accept |
| Esc | Cancel |
| ← → | Move the cursor |
| Home / End, Ctrl+A / Ctrl+E | Jump to the start / end |
| Backspace / Delete | Delete a character |
| Ctrl+U | Clear the line |
| Ctrl+K | Cut to the end of the line |

When you edit a title in place, fields you type into it (`#tag`,
`📅 2026-10-03`, `🚩`) are applied, and clearing the title deletes the reminder
(`u` brings it back), as in the GNOME app.

**The GNOME app's shortcuts work too,** where a terminal can send them:

| Key | Action |
|---|---|
| Ctrl+N | New reminder |
| Ctrl+T | Due today |
| Ctrl+K | Go to |
| Ctrl+F | Search |
| Ctrl+H | Show / hide completed |
| Ctrl+E | Show / hide subtasks |
| Ctrl+B | Show / hide the sidebar |
| F2 | Edit the title in place |
| F1 | Help |
| Alt+0 … Alt+3 | Priority none / low / medium / high |
| Alt+↑ / Alt+↓ | Move up / down |
| Ctrl+Page Up / Down | Previous / next sidebar entry |
| Ctrl+Q, Ctrl+W | Quit |

Some GUI shortcuts can't reach a terminal app, so their letter keys above stand
in for them:

- **Ctrl+Shift+letter** arrives as plain Ctrl+letter. So Ctrl+Shift+N, F, T,
  H and Z are `N`, `f`, `T`, `H` and `r` (Ctrl+E toggles, as in the app).
- **Ctrl+I** is the same as Tab, which switches panes, so editing every field
  is `e` or `i`.
- **Ctrl+]** and **Ctrl+[** (indent / outdent) are `]` and `[`: Ctrl+[ is the
  same as Esc.
- **Ctrl+1–9 and Ctrl+0** usually arrive as plain digits, so sidebar entries
  are `1`–`9` and `0`.
- **Ctrl+Z** still suspends the program, as in any terminal app (`fg` brings it
  back). Undo is `u`.

### Editing a reminder

`e` (or `i`) in the TUI, and `reminders edit NAME` with no field options,
open the reminder in your editor (`$VISUAL`, then `$EDITOR`, else nano
or vi) as simple `name: value` fields:

```yaml
# Editing “Milk” in Groceries. Change values, save and quit; an empty value
# clears a field. Quit without saving to cancel.
#   due: today, tomorrow, fri, +3d, 2026-10-31     time: 17:30
#   repeat: never, every day, every weekday, every week, every 2 weeks, every month, every year
#   priority: none, low, medium, high              list: Groceries, Work, Home
#   subtasks: Markdown lines; add, remove, or tick them with [x]
title: Milk
done: false
due: 2026-10-03
time: 17:30
repeat: never
priority: high
flagged: true
tags: errands
list: Groceries
section:
url:
notes: |
  2% if they have it
subtasks:
  - [ ] Check the date
```

- **Save and quit** to apply everything as one change (`u` undoes it in the
  TUI). **Quit without saving**, or empty the file, to cancel.
- **An empty value** clears a field (`due:` removes the date).
- **Changing `list:` or `section:`** moves the reminder.
- **Notes** are the indented lines under `notes: |`, blank lines included.
- **Subtasks** are Markdown lines: add or remove lines, tick with `[x]`, and add
  fields such as `📅 2026-10-05`. Subtasks you keep retain their notes.
- **If something can't be read** (say `due: someday`), you're told what and
  asked: `Edit it again, or revert to how it was? [E/r]`.
  - **Enter or `e`** reopens the editor with your text, and the problem noted
    on the first line.
  - **`r`** discards the edit and leaves the reminder as it was. From the CLI,
    the command then exits with status 1.

## Showing the number keys

Add `show-key-numbers=true` to `$XDG_CONFIG_HOME/reminders/settings.ini` to label the
first ten sidebar entries with their key: `(1)Today` … `(0)…` in the TUI, and
`Ctrl+1` … `Ctrl+0` after the names in the GNOME app. See [the settings file](USING.md#the-settings-file).

For a single run, `reminders --show-key-numbers` or
`reminders --hide-key-numbers` overrides the setting without changing it.

## Running both clients at once

The GNOME app, the CLI and the TUI can all run at the same time on the same
folder. Each saves straight to the list files and picks up the others' changes
from the folder. Each keeps its own undo history, covering only its own
changes.
