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
- **Linux only.** The GNOME app and the terminal client are the only
  versions so far (see Other platforms).

## Needs trying by hand

These were checked by calling the code directly, because real mouse clicks,
drags and key presses can't be sent in the headless test setup:

- Dragging lists and groups in the sidebar.
- Selecting reminders: Ctrl+click, Shift+click, dragging a selection, ↑/↓
  moving the selection.
- Right-click on a reminder: the menu opens at the pointer, and the title
  doesn't start editing.
- Clicking outside a title being edited saves it; clicking outside the
  reminders clears the selection.
- Ctrl+A and Escape from anywhere in the window.
- Dragging text in from another app (an editor, a browser): onto a reminder,
  a sidebar list, or anywhere else. Also check that dragging a reminder onto
  the New Reminder field moves it rather than typing its text in.
- Dragging reminders out to another app (an editor, Files): they should
  arrive as Markdown text and stay in the app.
