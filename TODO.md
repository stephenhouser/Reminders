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

These were checked by calling the code directly, because real mouse clicks,
drags and key presses can't be sent in the headless test setup:

- Dragging lists and groups in the sidebar.
- Ctrl+A and Escape from anywhere in the window.
- **Pasting or dropping several lines:** a bulleted or numbered list
  should become a reminder each, anything else one reminder with notes, and
  the message's Split / Combine button should switch it. Also Ctrl+Shift+V
  (Paste Special), which asks.
- **Pasting or dropping a link:** a bare address, and a link dragged from
  Firefox, should both show the address as a clickable link under the
  title.
- Dragging a reminder onto the New Reminder field: it should move there,
  not type its text in.

Confirmed by you (4 October): selecting reminders, the right-click menu,
clicking outside a title, dragging text in from a text editor and from
Firefox, and dragging reminders out to Files.
