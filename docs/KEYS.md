# Keys

Every key in the GNOME app (`Reminders`) and the terminal interface
(`reminders` with no command), side by side, with the command-line
equivalent where there is one. In the GNOME app, Ctrl+? shows its keys; in the
terminal interface, `?` or F1.

- **—** means there's no key (or command) for it there.
- **CLI** commands are `reminders COMMAND …` in a shell, or `:COMMAND …` at the
  terminal interface's `:` prompt, which takes every one of them (see
  [Commands at the `:` prompt](TERMINAL.md#commands-at-the--prompt)).
- Why some terminal keys differ from the app's (Ctrl+Shift, Ctrl+[ and others
  can't reach a terminal program) is explained in
  [TERMINAL.md](TERMINAL.md#the-interactive-interface).

## Reminders

On the selected reminder. In the GNOME app, click a row's empty space or use
↑ / ↓ to select one.

| GNOME | Terminal | CLI | Action |
|---|---|---|---|
| ↑ / ↓ | ↑ / ↓, j / k, Page Up / Down | — | Select another reminder |
| Ctrl+N | n, Ctrl+N | `add TEXT…` | New reminder |
| Space | x, Space | `done NAME`, `undone NAME` | Complete / not complete |
| Enter, F2 | Enter, F2 | `edit NAME --title TEXT` | Edit the title |
| Ctrl+S / Esc | Enter / Esc | — | Save / cancel while editing |
| Ctrl+E, Alt+Enter | e, Ctrl+E | `edit NAME` (no options) | Details: every field (the terminal opens your editor) |
| — | E | `edit-list [LIST]` | Edit the list's Markdown file in your editor |
| Menu, Shift+F10 | — | — | The reminder's ⋮ menu |
| — | — | `show NAME` | Everything about one reminder |
| Ctrl+D | f, Ctrl+D | `edit NAME --flag`, `--unflag` | Flag / unflag |
| Ctrl+T / Ctrl+Shift+T | t / T, Ctrl+T | `edit NAME --due today`, `tomorrow` | Due today / tomorrow |
| — (Details) | d | `edit NAME --due DATE` | Due date |
| Ctrl+0 … Ctrl+3 | 0 … 3 | `edit NAME --priority P` | Priority none / low / medium / high |
| — (Details) | # | `edit NAME --tag TAG`, `--untag TAG` | Add / remove a tag |
| ⋮ → Move To | m | `move NAME --to LIST` | Move to another list |
| Ctrl+] / Ctrl+[ | ] / [ | — | Indent / outdent (in a list) |
| Ctrl+↑ / Ctrl+↓ | K / J, Ctrl+↑ / Ctrl+↓ | — | Move up / down (in a list) |
| Shift+→ / Shift+← | +, Shift+→ / Shift+← | — | Show / hide its subtasks (on a subtask, its parent's) |
| Ctrl+X | — | — | Cut (copy, then delete) |
| Ctrl+C | — | — | Copy (as Markdown) |
| Ctrl+V | — | — | Paste reminders (outside a text field) |
| Ctrl+Shift+V | — | — | Paste Special: one reminder, or one per line |
| Delete | Delete | `delete NAME [--yes]` | Delete |

## Several reminders at once

In the terminal interface, *marked* reminders; then x / Space, f, t / T, d,
0–3, #, m and Delete act on all of them.

| GNOME | Terminal | CLI | Action |
|---|---|---|---|
| Ctrl+A | * | — | Select (mark) every reminder showing |
| Ctrl+click | v | — | Add to / remove from the selection |
| Shift+click, Shift+↑ / Shift+↓ | — | — | Select a range |
| Esc, Ctrl+Shift+A | Esc | — | Clear the selection |

## Lists and the sidebar

| GNOME | Terminal | CLI | Action |
|---|---|---|---|
| Ctrl+K | g, Ctrl+K | `list VIEW` | Go to a list, smart list or tag |
| — | 1–9, 0 | `list VIEW` | Jump to sidebar entry 1–10 |
| Enter (in the sidebar) | Enter | — | Open the sidebar entry |
| Ctrl+Page Down / Ctrl+Page Up | Ctrl+Page Down / Ctrl+Page Up | — | Next / previous sidebar entry |
| Ctrl+↑ / Ctrl+↓ (in the sidebar) | K / J, Ctrl+↑ / Ctrl+↓ | — | Move the sidebar entry up / down |
| Ctrl+Shift+↑ / Ctrl+Shift+↓ | Ctrl+Shift+↑ / Ctrl+Shift+↓ | — | Move its sidebar group up / down |
| — | Enter, Space (on a heading) | — | Fold / unfold a collapsible group |
| Ctrl+Shift+N | N | `new-list NAME` | New list |
| — | — | `lists` | Every list, with how many are open |
| Ctrl+H | c, Ctrl+H | `list VIEW -a` | Show / hide completed |
| Right-click it in the sidebar → Hide | h | — | Hide the list, smart list or tag; on a hidden one, show it |
| Ctrl+Shift+H | H | — | Show / hide hidden lists, smart lists and tags |

## General

| GNOME | Terminal | CLI | Action |
|---|---|---|---|
| Ctrl+Z / Ctrl+Shift+Z | u / r | — (`:undo`, `:redo`) | Undo / redo |
| Ctrl+F | /, Ctrl+F | `search TEXT` | Search |
| Ctrl+B, F9 | Ctrl+B | — | Show / hide the sidebar |
| Ctrl+L | Tab, ← / → | — | Switch between the sidebar and the reminders |
| — | : | — | Type a command (any CLI command) |
| F10 | — | — | Main menu |
| Ctrl+S | s | `sync SOURCE` | Sync this source: the selected list's or heading's, else the list showing's (a smart list, tag or search syncs them all); while editing, Ctrl+S saves instead |
| Ctrl+Shift+S | S | `sync` | Sync every source |
| Ctrl+O | O, Ctrl+O | `import FILE` | Import a file |
| ☰ → Export… | — | `export [LIST]` | Export lists |
| Ctrl+, | `,`, Ctrl+, | — | Settings (the terminal opens the settings file in your editor) |
| — | — | `folder [PATH]` | Show or set the folder |
| Ctrl+? | ?, F1 | `--help`, `help [COMMAND]` | Keys / help |
| — | Ctrl+L | — | Redraw the screen |
| Ctrl+W / Ctrl+Q | q, Ctrl+W, Ctrl+Q | — | Close the window / quit |

## Editing text

In the terminal interface, while editing a title in place or typing at a
prompt at the bottom (add, search, go to, due date, `:`). The GNOME app's text
fields have GTK's usual keys.

| Key | Action |
|---|---|
| Enter / Esc | Accept / cancel |
| ← → | Move the cursor |
| Home / End, Ctrl+A / Ctrl+E | Jump to the start / end |
| Backspace / Delete | Delete a character |
| Ctrl+U | Clear the line |
| Ctrl+K | Cut to the end of the line |
| Tab | Complete a file, list, tag or command; when several fit, they're listed above the line |
| ↑ / ↓ (at `:`) | Earlier commands |
