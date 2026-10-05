# To do

## Features

- Query language, SQL-like, for the CLI (`reminders query "…"`) and for saved
  smart lists in the sidebar.
- TUI: export, matching the GNOME app's Export… (lists, formats).
- TUI: tab completion for filenames on import/export?

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
