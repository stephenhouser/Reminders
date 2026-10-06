# To do

## Features

- Query language, SQL-like, for the CLI (`reminders query "…"`) and for saved
  smart lists in the sidebar.
- TUI: export, matching the GNOME app's Export… (lists, formats).
- TUI: tab completion for filenames on import/export?

## To reconsider (GNOME keys vs the HIG)

- Alt shortcuts: Alt+0–3 priority, Alt+↑/↓ move (the HIG says no Alt).
- Keys with other HIG meanings: Ctrl+H, Ctrl+B, Ctrl+I, Ctrl+E, Ctrl+Shift+F.
- Access keys: none in dialog fields; ⋮ menu clashes on c, d, m; Cut has none.
- Keyboard Shortcuts dialog: General section should come first.

## Known gaps

- CalDAV and WebDAV untested against real servers (only the fake one).
- CalDAV: reordering in other apps isn't picked up.
- CalDAV: unusual repeat rules are kept but not shown.
- A notes line like "- [ ] …" reads back as a subtask.
- Only the Linux clients exist.

## Needs trying by hand

- Add Source for CalDAV and WebDAV, after the back-end split.

## Possible future features

- Other platforms: iOS, Android, macOS, Windows — each native, iOS first.
- Alternate KDE front end, Qt/Kirigami on the same core (undecided).
