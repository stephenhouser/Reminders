# To do

What's planned or still open for Reminders. This is a readable copy of the
lists in INSTRUCTIONS.md ("Future features" and "Known gaps"), kept in step
with them.

*Last updated: 4 October 2026*

## Waiting on you

- **App icon in the desktop and app switcher.** The app has an icon and a
  launcher file, but neither is installed where GNOME looks, so you see a
  generic icon. Two choices are open:
  - Install the launcher and icon for your user, into
    `$XDG_DATA_HOME/applications` and `$XDG_DATA_HOME/icons`, pointing at
    the built app.
  - Design a nicer icon first. Ideas: a polished checklist card, a bell with
    a checkmark, or stacked coloured lists.

## Features

### Other platforms
iOS first, then Android, Windows and macOS. Each would be a native app:
SwiftUI on iOS and macOS. The iOS app would sync with Syncthing built into
it. The iOS Reminders app is the model for features, and the file format
(docs/FORMAT.md) is what every version shares.

*Blocker:* iOS can't be built on this Linux machine (no Swift or Xcode). It
needs a Mac, or a decision about tooling. Nothing gets installed without
asking.

### KDE version (maybe)
A native Plasma app (Qt/Kirigami) beside the GNOME one, sharing the same
core library. Not decided.

### Query language
SQL-like queries, for two uses:
- the terminal client: `reminders query "…"`;
- your own smart lists, saved in the sidebar.

Not designed yet.

## Known gaps

- **Untested against real servers.** CalDAV and WebDAV have only been tested
  against the fake test server, not Nextcloud, Fastmail or another real one.
- **Git untested against hosted repositories.** Only local repositories have
  been tested, not GitHub or another host over SSH or HTTPS.
- **CalDAV ordering from other apps.** Reordering reminders, or moving them
  between sections, in another CalDAV app isn't picked up here; this app's
  order wins.
- **Unusual repeat rules from CalDAV.** Repeat rules beyond the ones in
  FORMAT.md are kept, but not shown.
- **Notes lines that look like checklist items.** A notes line written as
  "- [ ] something" is read back as a subtask, because the file format has
  no way to mark it as plain text. Typing one into a reminder's notes in
  Details does this. (Combining pasted lines into one reminder avoids it by
  writing such lines as "☐ something".)
- **Linux only.** The GNOME app and the terminal client are the only
  versions so far (see Other platforms).

## Needs trying by hand

Things that can only be checked with a real mouse and keyboard (the
automatic tests here can't send clicks, drags or key presses).

Nothing is waiting for a check. Not tried by you yet, but no problems
reported:
- **⋮ → Mark as Completed** on a selection of reminders, in the GNOME app
  (the right-click / ⋮ menu item).

### Tried by you and working (4 October)

GNOME app:
- Dragging lists, and whole groups, in the sidebar.
- Selecting reminders: Ctrl+click, Shift+click, a plain click, and
  Ctrl+A / Escape from anywhere.
- The right-click menu, opening where you click.
- Clicking outside a title being edited (saves it) or outside the reminders
  (clears the selection).
- Dragging text in from a text editor and from Firefox, including a Firefox
  link.
- Dragging reminders out to Files (it makes a file named after the
  reminder).
- Dropping a reminder on a section's New Reminder row: it moves to the end
  of that section, and the field stays empty.
- Pasting or dropping several lines: a list splits, anything else becomes
  one reminder with notes; the Split / Combine button and Ctrl+Shift+V
  (Paste Special).
- Pasting a link: it shows as a clickable link.

Terminal app:
- Marking reminders (`v`, `*`, Esc) and acting on all of them, including
  completing them with Space / `x`.
- The help screen (`?`): one column, fits the screen, scrolls, and leaves
  nothing behind when closed.
