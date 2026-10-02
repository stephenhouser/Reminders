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
folder. Reminders remembers it. To switch later, use **☰ → Change Folder…**.

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
Show them again with **⋮ → Show Completed** (Ctrl+H) or the **Show** link at
the bottom of the list.

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
  order: Today, Scheduled, All, Flagged, Completed, then your lists.

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

## The settings file

`~/.config/reminders/settings.ini` is shared by the GNOME app and the terminal
client. Most of it is filled in for you; `show-key-numbers` is only set here.
In the terminal client, `S` opens it in your editor and applies the changes
when you quit the editor; the app reads it at start-up.
[settings.example.ini](settings.example.ini) lists every setting with its
default, ready to copy. Settings must be under the `[general]` line; anything
outside it is ignored.

```ini
[general]
# The folder to open (set with Change Folder…)
folder=/home/you/Sync/Reminders
# The list that last had focus
view=list:Groceries
# Show each sidebar entry's jump key (Ctrl+1 …)
show-key-numbers=true
# The order of the sidebar's groups
sidebar-order=smart-lists, my-lists, tags
# Which smart lists to show, in this order
smart-lists=today, scheduled, all, flagged, completed
# How each group appears: visible, collapsible or hidden
# (your lists can be visible or collapsible, not hidden)
smart-lists-display=visible
my-lists-display=visible
tags-display=visible
# Set by the app when you fold a collapsible group
smart-lists-collapsed=false
my-lists-collapsed=false
tags-collapsed=false
```

Comments go on their own lines, starting with `#`.

`show-key-numbers` labels the first ten sidebar entries with the key that jumps
to them. In the app the shortcut is shown to the right of the name, e.g.
`Today  Ctrl+1  4`, through `Ctrl+0`. In the terminal client it's a prefix,
e.g. `(1)Today`, for the keys `1`–`9` and `0`. It accepts `true`, `yes` or `1`, is off when missing, and is
read at start-up.

**Sidebar groups.** The sidebar has three groups: the smart lists, your lists
(My Lists) and tags.

`sidebar-order` sets their order, e.g. `sidebar-order=my-lists, tags,
smart-lists`. A group you leave out or misspell goes at the end, so a typo
can't make your lists disappear. You can also rearrange them without editing
the file:

- **In the app:** right-click (or long-press) any entry or heading in the
  sidebar and choose Move Up or Move Down, or press Alt+↑ / Alt+↓ on a
  sidebar entry. The same menu's **Collapsible** item switches the group
  between `visible` and `collapsible`.
- **In the terminal client:** select a sidebar entry and press `K` / `J` (or
  Alt+↑ / Alt+↓).

`smart-lists-display`, `my-lists-display` and `tags-display` each take one of
these values:

| Value | Shows the group |
|---|---|
| `visible` (default) | With a plain heading, except the group at the top, which has none |
| `collapsible` | Under a heading you click (or press Enter on) to fold or unfold it |
| `hidden` | Not at all (not allowed for My Lists) |

So the group at the top has no heading unless it's collapsible, and the
groups below it always have one.

The app remembers a folded group (`smart-lists-collapsed`,
`my-lists-collapsed`, `tags-collapsed`); this only applies in `collapsible`
mode.

- **`smart-lists`** chooses which smart lists appear and in what order, e.g.
  `smart-lists=today, flagged`.
- **Numbering:** the shortcuts (Ctrl+1 …), Ctrl+Page Up/Down and their labels
  follow what's showing, in order. So hiding or folding the smart lists makes
  your lists start at Ctrl+1.
- **Go To (Ctrl+K)** still finds entries in folded groups, but not in hidden
  ones.
- **A hidden last list:** if the list you last had open is now hidden, the app
  opens on Today or the first entry showing.

For one run, `--show-key-numbers` or `--hide-key-numbers` on the command line
overrides it without changing the file (`Reminders --show-key-numbers`, or
`reminders --hide-key-numbers`). Passing one to an app that's already running
switches its sidebar.

## Troubleshooting

| Problem | What to do |
|---|---|
| A Markdown file doesn't appear as a list | It needs `reminders: 1` in its front matter. Use the banner's **Review…**, or add it by hand. |
| Changes from another device don't appear | Check that Syncthing is running and the folder is up to date (Syncthing's web UI at http://127.0.0.1:8384). Reminders reloads as soon as files change. |
| A `.sync-conflict-` file stays in the folder | Reminders merges conflict copies of lists only. Copies of other files are left for you. |
| A reminder came back after you deleted it | Without a merge base (for example the first sync on a new computer), conflict merges keep reminders rather than risk losing them. Delete it again. |
| Settings | See [The settings file](#the-settings-file). |
