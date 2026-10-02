# Using Reminders in a terminal

`reminders` (lowercase) is the terminal client: a command-line interface for
quick changes and scripts, and an interactive full-screen interface (TUI) when
you run it with no command. It works on the same folder as the GNOME app
(`Reminders`), uses the same settings, and runs without a graphical desktop, so
it also suits SSH sessions and headless machines running Syncthing.

## The folder

`reminders` uses the folder chosen in the GNOME app (saved in
`~/.config/reminders/settings.ini`). To set or check it from the terminal:

```sh
reminders folder ~/Sync/Reminders     # set (shared with the app)
reminders folder                      # show
reminders --folder ~/Sync/Work list   # use another folder for one command
```

## Commands

```
reminders                        open the interactive interface
reminders lists                  lists, with how many reminders are open
reminders list [VIEW] [-a]       reminders in VIEW (default: today)
reminders show REF               everything about one reminder
reminders add TEXT… [FIELDS]     add a reminder
reminders edit REF [FIELDS]      change one
reminders done REF…              complete
reminders undone REF…            mark as not completed
reminders move REF LIST [--section S]
reminders delete REF… [--yes]
reminders search TEXT
reminders new-list NAME [--color C] [--icon I]
reminders folder [PATH]
```

**VIEW** is a list name (case doesn't matter), `today`, `scheduled`, `all`,
`flagged`, `completed` or `#tag`. `-a` includes completed reminders.

**REF** is a reminder's id, shown at the end of each line by `list` (`milk01`,
or `^milk01`), or part of its title. When several reminders match, an open one
is preferred; if that's still ambiguous, the matches are listed with their ids.

**FIELDS** (for `add` and `edit`):

| Option | Meaning |
|---|---|
| `--title TEXT` | Title |
| `--list LIST` | List (`add`: which list; `edit`: move it there). Default for `add`: the first list |
| `--section NAME` | Section to add into |
| `--parent REF` | Add as a subtask of REF |
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

### Examples

```sh
reminders add "Pay rent" --list Home --due +3d --time 09:00 --priority high --flag
reminders list                     # today and overdue
reminders list groceries -a        # a list, including completed ones
reminders done milk                # by title…
reminders done milk01 eggs01       # …or by id
reminders edit milk --due tomorrow --tag errands --notes "2%"
reminders move bread Work
reminders list '#errands'
reminders search dentist
```

### Scripting

- `--json` prints machine-readable output for `list`, `show`, `search`,
  `lists`, `add`, `edit`, `done` and `move`. Each reminder includes `id`,
  `list`, `section`, `parent`, `title`, `done`, `due`, `time`, `completed`,
  `flagged`, `priority`, `tags`, `repeat`, `url` and `notes`.
- Colours appear only when printing to a terminal; `--no-color` or the
  `NO_COLOR` environment variable turns them off.
- `delete` asks for confirmation when run interactively; `--yes` skips it.
- Exit status: 0 success, 1 error (e.g. nothing matched), 2 bad usage.

```sh
# Titles of everything due today, one per line
reminders --json list today | jq -r '.[].title'
```

## The interactive interface

Run `reminders` with no command. The sidebar (smart lists, your lists, tags)
is on the left, and the selected view on the right. Changes made elsewhere (in
the GNOME app, on another device through Syncthing, or in an editor) appear
within a second.

| Key | Action |
|---|---|
| ↑ ↓ / j k, Page Up/Down | Move |
| Tab, ← → | Switch between the sidebar and the reminders |
| Enter | Open the selected sidebar entry |
| 1–9 | Jump to sidebar entry 1–9 (Today, Scheduled, All, Flagged, Completed, your lists) |
| g | Go to a list or tag by typing part of its name |
| / | Search |
| c | Show / hide completed |
| N | New list |
| u / Ctrl+R | Undo / redo |
| ? | Help |
| q | Quit |

On the selected reminder:

| Key | Action |
|---|---|
| Space | Complete / not complete |
| a | Add a reminder (inline fields work; in Today it gets today's date, in Flagged a flag) |
| e, Enter | Edit the title |
| i | Details |
| d | Due date: `today`, `tomorrow 09:00`, `fri`, `+3d`, `2026-10-31`, or `none` |
| t / T | Due today / tomorrow |
| f | Flag / unflag |
| 0 1 2 3 | Priority none / low / medium / high |
| # | Add a tag (`-tag` removes it) |
| m | Move to another list (type the start of its name) |
| J / K | Move down / up (in a list) |
| > / < | Indent / outdent (in a list) |
| x, Delete | Delete (asks first; undo with u) |

In prompts, Enter accepts, Esc cancels and Ctrl+U clears.

## Running both clients at once

The GNOME app, the CLI and the TUI can all run at the same time on the same
folder. Each saves straight to the list files and picks up the others' changes
from the folder. Each keeps its own undo history, covering only its own
changes.
