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
for a name, a colour and an icon. The list becomes `<name>.md` in your folder.

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

- **Title:** click it, or select the reminder and press Enter or F2. Enter or
  Ctrl+S saves; Esc puts back the old title. Clearing the title deletes the
  reminder.
- **Everything else:** the ✏ button (shown on hover), Ctrl+I, or **Details…**
  in the ⋮ menu. The details dialog has the title, notes, URL, date, time,
  repeat, flag, priority, list, tags and subtasks. **Done** or Ctrl+S saves;
  **Cancel** or Esc discards.

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
| Flag / unflag | Ctrl+Shift+F |
| Priority none / low / medium / high | Alt+0 / Alt+1 / Alt+2 / Alt+3 |

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
- **Alt+↑ / Alt+↓** moves the selected reminder up or down, across section
  boundaries.
- **The List field** in the details dialog moves it to another list.

### Copying and pasting

- **Ctrl+C** on a selected reminder copies it as the Markdown line from its
  file, with its notes and subtasks (no id), so it also pastes into an editor
  or a chat as a checklist.
- **Ctrl+V** while not typing in a text field pastes reminders:
  - **A copied reminder** (or any `- [ ] …` checklist lines) comes back with
    every field, notes and subtasks.
  - **Plain text** becomes one reminder per line, titled with the line;
    inline fields such as `#tag` or `📅 2026-10-05` apply, as when typing.
  - **Where they go:** after the selected reminder, in its list and section;
    otherwise at the end of the list being shown. In a smart list they go
    into your first list, and are made to show there: due today in Today or
    Scheduled, flagged in Flagged, tagged in a tag's view.
  - Undo with Ctrl+Z.
- **In a text field** (a title being edited, New Reminder, search), Ctrl+C and
  Ctrl+V copy and paste text as usual.

### Deleting

Press Delete, or **⋮ → Delete**. Undo from the message that appears, or with
Ctrl+Z.

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
- **Ctrl+1 … Ctrl+9 and Ctrl+0** jump to the first ten sidebar entries, in
  order: Today, Scheduled, All, All Reminders, Flagged, Completed, then your lists.

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
| Ctrl+I | Details |
| Ctrl+Shift+F | Flag / unflag |
| Ctrl+T / Ctrl+Shift+T | Due today / tomorrow |
| Alt+0 … Alt+3 | Priority none / low / medium / high |
| Ctrl+] / Ctrl+[ | Indent / outdent |
| Alt+↑ / Alt+↓ | Move up / down |
| Ctrl+C | Copy (as Markdown) |
| Ctrl+V | Paste reminders (outside a text field) |
| Delete | Delete |

**Lists**

| Shortcut | Action |
|---|---|
| Ctrl+K | Go to… |
| Ctrl+1 … Ctrl+9, Ctrl+0 | Sidebar entry 1–10 |
| Ctrl+Page Down / Ctrl+Page Up | Next / previous sidebar entry |
| Ctrl+Shift+N | New list |
| Ctrl+H | Show / hide completed |
| Ctrl+Shift+H | Show / hide hidden lists, smart lists and tags |
| Ctrl+E | Show / hide all subtasks |

**General**

| Shortcut | Action |
|---|---|
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo |
| Ctrl+F | Search |
| Ctrl+B | Show / hide sidebar |
| F10 | Main menu |
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

```
Groceries.md                      ← a list (synced)
.reminders/<this-computer>/       ← this computer's working state (not synced)
```

- **Conflicts:** if two devices change the same list before syncing, Syncthing
  saves a `Groceries.sync-conflict-….md` copy. Reminders merges it into
  `Groceries.md` and deletes it:
  - Changes to different reminders, or to different fields of one reminder,
    are all kept.
  - If both devices changed the same field, the version Syncthing kept as
    `Groceries.md` wins.
- **`.reminders/`** holds, for each list, the last version received from
  another device (the starting point for merges) and a fingerprint of this
  computer's last save. Reminders adds `(?d).reminders` to the `.stignore` at
  the root of your Syncthing folder so that it isn't synced.
- **Deleting a list** moves its file to the Trash on that computer, and
  Syncthing deletes it on the others.

## Where files are kept

Reminders follows the XDG base directory rules: each place below moves with
its variable when that's set to an absolute path.

| What | Where |
|---|---|
| Settings | `$XDG_CONFIG_HOME/reminders/settings.ini` (`~/.config/reminders/settings.ini`) |
| CalDAV sources' lists | `$XDG_DATA_HOME/reminders/caldav/NAME/` (`~/.local/share/…`) |
| This computer's records for a Syncthing source (merge bases) | `.reminders/DEVICE/` in its folder, kept out of the sync by `.stignore` |
| … for other sources (CalDAV sync records) | `$XDG_STATE_HOME/reminders/DEVICE/NAME/` (`~/.local/state/…`) |
| Things found again if lost (CalDAV servers' calendar addresses) | `$XDG_CACHE_HOME/reminders/` (`~/.cache/…`) |

`DEVICE` is the computer's name plus a short code, so computers sharing a
home folder keep separate records. Deleting the state or cache folders is
safe: merges after that keep everything rather than guess at deletions, and
CalDAV sources fetch their lists again.

## The settings file

`$XDG_CONFIG_HOME/reminders/settings.ini` (`~/.config/reminders/settings.ini`) is shared by the GNOME app and the terminal
client. Most of it is filled in for you; `show-key-numbers` is only set here.
In the app, **main menu → Settings…** opens it in your default text editor
(creating it if needed), and changes apply as soon as you save. In the
terminal client, `S` opens it in your editor and applies the changes when you
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
# Show each sidebar entry's jump key (Ctrl+1 …)
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
# Set by the app when you fold a collapsible group
smart-lists-collapsed=false
lists-collapsed.personal=false
tags-collapsed=false
```

Comments go on their own lines, starting with `#`.

`show-key-numbers` labels the first ten sidebar entries with the key that jumps
to them. In the app the shortcut is shown to the right of the name, e.g.
`Today  Ctrl+1  4`, through `Ctrl+0`. In the terminal client it's a prefix,
e.g. `(1)Today`, for the keys `1`–`9` and `0`. It accepts `true`, `yes` or `1`, and is off when missing.

**Sidebar groups.** The sidebar has the smart lists, your lists and tags.
Your lists are one group per [source](#sources): with one source it's
"My Lists"; with several, each source's group is headed by its title.

`sidebar-order` sets their order, e.g. `sidebar-order=local-lists, tags,
smart-lists`. With several sources, `lists:NAME` places one source's group
(`sidebar-order=lists:work, smart-lists, lists:home, tags`), and `local-lists`
stands for the sources not named. A group you leave out or misspell goes at
the end, so a typo can't make your lists disappear. You can also rearrange them without editing
the file:

- **In the app:** right-click (or long-press) a group's heading in the
  sidebar and choose Move Up or Move Down, or press Alt+Shift+↑ /
  Alt+Shift+↓ on any of its entries. The same menu's **Collapsible** item
  switches the group between `visible` and `collapsible`.
- **In the terminal client:** select a sidebar entry and press Alt+Shift+↑ /
  Alt+Shift+↓ (or `K` / `J` on the group's heading).

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
- **Numbering:** the shortcuts (Ctrl+1 …), Ctrl+Page Up/Down and their labels
  follow what's showing, in order. So hiding or folding the smart lists makes
  your lists start at Ctrl+1.
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
| Alt+↑ / Alt+↓ | The selected smart list, list or tag, within its group (in the terminal, also `K` / `J`) |
| Alt+Shift+↑ / Alt+Shift+↓ | Its whole group |

In the app, the right-click menu's **Move Up** / **Move Down** do the same
for an entry, and on a heading for its group. The order is kept in
settings.ini, for this computer only: smart lists in `smart-lists`, your
lists in `lists-order` and tags in `tags-order`. Lists and tags not named
there follow the others (lists in their usual order, tags alphabetically).
Moves skip hidden entries.

**Tag colours and icons.** Right-click a tag and choose **Tag Info…** to give
it a colour and icon, as for a list. They're saved in settings.ini
(`tag-color.NAME`, `tag-icon.NAME`), so they're per computer.
- **A hidden last list:** if the list you last had open is now hidden, the app
  opens on Today or the first entry showing.

For one run, `--show-key-numbers` or `--hide-key-numbers` on the command line
overrides it without changing the file (`Reminders --show-key-numbers`, or
`reminders --hide-key-numbers`). Passing one to an app that's already running
switches its sidebar.

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
| `syncthing` | The folder is synced by Syncthing. Conflict copies are merged; per-device records live in `<folder>/.reminders/`, which `.stignore` keeps out of the sync. |
| `local` | Just the folder: list files are read and saved as they are, and changes made by other programs still show up. |
| `caldav` | Task lists on a CalDAV server (see [CalDAV accounts](#caldav-accounts)). The folder is a local copy, kept in step with the server. |

- **☰ → Sources… → Add Source…** asks for the source's **Name**, its
  **Type** (Syncthing, Local Folder or CalDAV) and its **Folder**.
  Choosing a folder picks the type for you (Syncthing inside a Syncthing
  folder, one with `.stfolder`, else Local Folder); you can change it. For
  CalDAV the folder is the **Local Copy**, which starts out in
  `~/.local/share/reminders/caldav/NAME`, and a **Server** section asks for
  the account (see [CalDAV accounts](#caldav-accounts)). The settings
  section is named after the Name (`[source.NAME]`, lower case), else the
  server or the folder.
- **Choose a source in Sources…** for **Source Info…**: the same rows, its
  **Default Source** switch (where new lists go), and **Remove Source…**,
  which takes it out of the app and leaves its folder and files as they are
  (a CalDAV source's local copy goes, since the server has the lists).
- **Right-click a source's group heading** for **New List…** in that source
  and its **Source Info…**.
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
  default one. In the terminal: `reminders new-list NAME --source NAME`.
- **List names** only need to be unique within a source. Where two sources
  have a list of the same name, it's shown as `source/name` (in mixed views,
  the Details list field and the terminal), and settings name it that way:
  `view=list:home/Todo`, `lists-order`, `lists-hidden`. A bare `Todo`
  still works when only one source has it.
- **Settings from before sources** had `folder=` in `[general]`; it's no
  longer read. Add the folder again with ☰ → Sources… → Add Source… (or
  `reminders folder PATH`), or write the section above by hand.

### CalDAV accounts

A CalDAV source shows the task lists of an account on a CalDAV server
(Nextcloud, Fastmail, Radicale, iCloud with an app-specific password, …):
each of its task lists is a list here, and each list made here becomes a task
list there. Add one with ☰ → Sources… → **Add Source…**, Type **CalDAV**, or by hand:

```ini
[source.fastmail]
backend=caldav
url=https://caldav.fastmail.com/
username=you@fastmail.com
password-command=secret-tool lookup service reminders-caldav
interval=15
title=Fastmail
```

- **`url=`** is the server's address. The app finds the calendars from it
  (through `/.well-known/caldav` and your principal), so the server's root
  usually works; the address of your calendar home works too.
- **`password-command=`** is a command that prints the password, so it
  isn't kept in the settings file. For example, store it in the GNOME
  keyring once with `secret-tool store --label "Reminders CalDAV" service
  reminders-caldav`, then use the line above; `pass show caldav` works too.
  Use an app-specific password where the server offers them.
- **When it syncs:** when the app opens, every `interval=` minutes (default
  15), and a couple of seconds after you change something. **☰ → Sync Now**
  syncs straight away. Offline changes wait in the local copy until the
  next sync.
- **The local copy** is in `$XDG_DATA_HOME/reminders/caldav/NAME/`
  (`~/.local/share/…`; `folder=` moves it): one Markdown file per task list, like any other
  source, which you can open in an editor.
- **Changes on both sides** are merged reminder by reminder, field by field,
  as for Syncthing conflicts; when both sides changed the same field, this
  device's change wins. Properties the app doesn't use (alarms, start
  dates, other apps' extras) are kept as they are on the server.
- **Limits:** the order of reminders and their sections are sent to the
  server (as `X-APPLE-SORT-ORDER` and `X-REMINDERS-SECTION`), but moves made
  in another app aren't picked up yet. Repeat rules that the app can't
  express (say, "the first Monday of the month") are kept but not shown.
  Deleting a list here deletes its task list on the server.
- **Problems** (wrong password, server unreachable) show as a message at
  the bottom of the window; the next sync tries again.

## Troubleshooting

| Problem | What to do |
|---|---|
| A Markdown file doesn't appear as a list | It needs `reminders: 1` in its front matter. Use the banner's **Review…**, or add it by hand. |
| Changes from another device don't appear | Check that Syncthing is running and the folder is up to date (Syncthing's web UI at http://127.0.0.1:8384). Reminders reloads as soon as files change. |
| A `.sync-conflict-` file stays in the folder | Reminders merges conflict copies of lists only. Copies of other files are left for you. |
| A reminder came back after you deleted it | Without a merge base (for example the first sync on a new computer), conflict merges keep reminders rather than risk losing them. Delete it again. |
| Settings | See [The settings file](#the-settings-file). |
