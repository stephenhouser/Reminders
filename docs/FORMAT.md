# Reminders folder format (v1)

This is the on-disk contract shared by every client (Linux, iOS, Android, macOS,
Windows). Syncthing syncs the folder. Anyone can read or edit the files in a
plain text editor, and anything a client doesn't understand must survive a
round trip unchanged.

The syntax is a superset of the [Obsidian Tasks](https://publish.obsidian.md/tasks/)
emoji format, so the folder also works as an Obsidian vault.

## Folder layout

```
<synced folder>/
  Groceries.md          ← one file per list; list name = file name (without .md)
  Work.md
  Ideas.md              ← no marker: an ordinary note, not a list
  Groceries.sync-conflict-20261001-120000-ABCDEFG.md   ← Syncthing conflict copy (clients merge & delete)
  .reminders/           ← per-device client state (see "Per-device state")
    laptop-3f9a/
```

- A **list** is a `*.md` file directly in the folder whose front matter has the
  `reminders` key (see below). Other Markdown files, subfolders, dotfiles and
  other files are ignored and left alone, so the folder can be shared with notes,
  for example in an Obsidian vault.
- Clients may offer to turn a Markdown file that contains a checklist into a
  list by adding the marker, but must not do so without the user's say-so.
- Renaming a list means renaming its file.
- File encoding is UTF-8 with `\n` line endings. Readers must also accept `\r\n`.

## List file

```markdown
---
reminders: 1
color: orange
icon: cart
order: 2
---

- [ ] Buy milk #errands ⏫ 🚩 📅 2026-10-03 09:00 ^k3x9qa
  2% if they have it
  second line of notes
  - [ ] Check expiry date ^p0d2mf
- [ ] Bread ^b81zzc

## Party
- [ ] Balloons 🔁 every week 📅 2026-10-04 ^h2n7aa
- [x] Cake ✅ 2026-09-30 ^c4ke00
```

### Front matter

A YAML block at the very top of the file. Values are flat `key: value` strings.
The `reminders` key is required; the rest are optional.

| key         | meaning                                         | default |
|-------------|-------------------------------------------------|---------|
| `reminders` | marks the file as a list; the value is the format version (`1`). Readers accept any value. Writers put it first. | — |
| `color`     | list colour, one of the names below             | `blue`  |
| `icon`      | list icon, one of the names below               | `list`  |
| `order`     | integer sort position in the sidebar            | after all ordered lists, then by name |

Clients must keep keys they don't know.

**Colours:** `red orange yellow green cyan blue indigo purple pink brown gray`

**Icons:** `list tag bookmark cart gift home work school calendar flag star heart
music game book food travel nature person people money pill computer camera`
(Each client maps these names to its own icon set, e.g. SF Symbols or Adwaita.)
An unknown icon name is shown as `list`.

### Sections

A heading of level 2 (`## Name`) starts a section. Reminders before the first
heading are in no section. Other headings and any non-task text are kept
verbatim, in place.

### Reminder line

```
- [ ] <title and inline fields> ^<id>
```

- The marker is `- [ ] ` (open) or `- [x] ` (done). Readers also accept `*` or `+`
  as the bullet and `X` as the mark.
- A top-level reminder starts at column 0.
- **Subtasks** are reminder lines indented by 2 spaces (or a tab) under their parent.
  There is only one level, as in Apple Reminders. Readers treat deeper nesting as
  that same single level.
- **Notes** are any other non-empty lines indented under a reminder (or its
  subtask), with the indentation removed. Writers indent notes by 2 spaces under a
  top-level reminder and 4 under a subtask.
- A blank line ends the reminder block, unless the next non-blank line is
  indented; then the blank lines belong to the notes.

Inline fields can appear anywhere after the marker. Writers emit them in this
order, separated by single spaces:

| field      | syntax                         | notes |
|------------|--------------------------------|-------|
| tags       | `#tag`                         | `#` followed by a letter or `_`, then `[A-Za-z0-9_/-]`. Many allowed. |
| priority   | `⏫` high · `🔼` medium · `🔽` low | Obsidian's `🔺` reads as high and `⏬` as low. |
| flagged    | `🚩`                           | |
| repeat     | `🔁 every <rule>`              | see below |
| due        | `📅 YYYY-MM-DD` or `📅 YYYY-MM-DD HH:MM` | local time (24h); date-only means "all day" |
| completed  | `✅ YYYY-MM-DD`                 | date the reminder was completed |
| created    | `➕ YYYY-MM-DD`                 | optional, preserved |
| url        | `🔗 <url>`                     | up to the next space |
| id         | `^<id>`                        | must be last on the line; `[a-z0-9]{6,}`; unique within the folder |

Whatever remains after the fields are removed (whitespace collapsed) is the **title**.
Emoji may carry a trailing variation selector (U+FE0F); readers ignore it.

**IDs.** Lines written by hand often have no `^id`. Clients assign one the next
time they write that file, and must not rewrite a file just to add IDs.

**Repeat rules:** `every day`, `every weekday`, `every weekend`, `every week`,
`every month`, `every year`, and `every N days|weeks|months|years`.
Clients keep unknown rules as-is but don't act on them.

**Completing a repeating reminder** (same behaviour as Obsidian Tasks): the
completed line gets `✅ <today>` and no longer repeats. A new open copy, with a
new id and the next due date, is inserted directly above it. Subtasks are copied
as open and notes are copied.

## Writing

- Write atomically: write to `.<name>.md.tmp` in the same folder, then rename it
  over the target.
- Only write a file that has actually changed. Unchanged files keep their exact bytes.
- Line order within a file is the display order (manual sort).

## Per-device state

(See also [the Syncthing back end](../backends/syncthing/README.md).)

Each client keeps its own working state for a Syncthing folder in
`.reminders/<device>/`, where `<device>` is a name unique to that device (for
example the host name plus a short code). The Linux client keeps, per list,
`base/<list>.md` (the last version received from another device, the base for
three-way merges) and `written/<list>` (a fingerprint of the last version it
wrote), plus `declined.txt`.

- A client only ever reads and writes its own `<device>` folder and ignores the
  others.
- The folder should not be synced. In a Syncthing folder, clients add the line
  `(?d).reminders` to the `.stignore` at the Syncthing folder's root (the folder
  holding `.stfolder`). The `(?d)` lets Syncthing remove it when the folder
  around it is deleted on another device. If it is synced anyway (another sync tool, or the line
  was removed), nothing breaks: each device writes only to its own subfolder.
- Losing this state is harmless: the next conflict is merged two-way (no
  deletions).
- Sources that aren't Syncthing folders (a plain local folder, a CalDAV or
  WebDAV account) keep their state where the platform keeps app state; the
  Linux client uses `$XDG_STATE_HOME/reminders/<device>/<source>/`
  (`~/.local/state/…` when `$XDG_STATE_HOME` isn't set), with sync records
  in `caldav/` or `webdav/`.

## Server and git back ends

How the folder maps onto a CalDAV server, a WebDAV folder or a git repository is
described with each back end: [CalDAV](../backends/caldav/README.md),
[WebDAV](../backends/webdav/README.md), [Git](../backends/git/README.md).

## Conflicts

When two devices edit the same list before syncing, Syncthing keeps one version
as `List.md` and renames the other to `List.sync-conflict-<date>-<time>-<device>.md`.
A client that sees a conflict copy:

A conflict copy is only merged if the main file or the copy carries the marker;
conflict copies of ordinary notes are left alone.

1. Merges it into `List.md` by reminder `id`, using a three-way merge when it has a
   cached copy of the last version it wrote or read (the *base*). Otherwise it does a
   two-way union.
   - For each field: if only one side changed it from the base, take that side.
     If both changed it, the main file wins.
   - A reminder deleted on one side and unchanged on the other is deleted.
     Without a base, reminders are never dropped.
   - A reminder that only the conflict copy has goes right after the nearest
     earlier reminder (in the conflict copy) that the main file also has, or
     at the top of its section.
   - Reminders without an id are matched by exact title within the same parent.
2. Writes the merged `List.md` and deletes the conflict copy.
