# To do

## Refactor (in stages)

1. Split the big UI files (window, tui, cli) by area. Done.
2. Shared app layer for the GNOME app and the TUI. Done.
3. Back-end registry with string ids. Done.
4. One self-contained module per back end, with its own build option and docs. Done.
5. Split settings into ini, preferences and source config. Done.

## Features

- Other platforms: iOS first, then Android, Windows, macOS. iOS needs a Mac.
- KDE version (maybe).
- Query language, for the CLI and for saved smart lists.
- TUI: a key to sync the selected source, and one to sync all sources.
- Settings: keep the parsed file in memory, reload when it changes on disk.
- make the default new source "local"
- import should allow selection of which source to import to "New List ... in ..."
- New List dialog should let me pick which source to create in
- Add import to TUI, perhaps the I key
- only show a fixed number of lines in the notes field (set in settings)

## Known gaps

- CalDAV and WebDAV untested against real servers.
- CalDAV: reordering in other apps isn't picked up.
- CalDAV: unusual repeat rules are kept but not shown.
- A notes line like "- [ ] …" reads back as a subtask.

## Needs trying by hand

- GUI: clicking the flag on a row flags / unflags it (with the selection).
- GUI: `row-buttons=always` / `hover` in settings.ini.
- Add Source for CalDAV and WebDAV after the back-end split (stage 4).
