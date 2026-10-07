# To do

## Features

- Query language, SQL-like, for the CLI (`reminders query "…"`) and for saved
  smart lists in the sidebar.

## Known gaps

- WebDAV untested against real servers (only the fake one).
- CalDAV: reordering in other apps isn't picked up.
- CalDAV: unusual repeat rules are kept but not shown.
- A notes line like "- [ ] …" reads back as a subtask.
- Only the Linux clients exist.

## Needs trying by hand


## Possible future features

- Other platforms: iOS, Android, macOS, Windows — each native, iOS first.
- Alternate KDE front end, Qt/Kirigami on the same core (undecided).
- Alternate profiles (settings.ini) stored in .config, selected on start and a profile picker in GUI versions. alternate .ini files. Picker in GUI to choose which one to launch with. Must also allow multiple copies of the GUI in that instance.