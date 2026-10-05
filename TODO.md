# To do

## Refactor (in stages)

1. Split the big UI files (window, tui, cli) by area. Done.
2. Shared app layer for the GNOME app and the TUI. Done.
3. Back-end registry with string ids. Done.
4. One self-contained module per back end, with its own build option and docs.
5. Split settings into ini, preferences and source config.

## Features

- Other platforms: iOS first, then Android, Windows, macOS. iOS needs a Mac.
- KDE version (maybe).
- Query language, for the CLI and for saved smart lists.
- TUI: a key to sync the selected source, and one to sync all sources.
- Clicking the flag in the GUI should toggle flagged on and off

## Known gaps

- CalDAV and WebDAV untested against real servers.
- CalDAV: reordering in other apps isn't picked up.
- CalDAV: unusual repeat rules are kept but not shown.
- A notes line like "- [ ] …" reads back as a subtask.

## Needs trying by hand

- Nothing right now.
