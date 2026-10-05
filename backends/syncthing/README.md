# Syncthing back end

A folder that Syncthing keeps in step with your other devices. Reminders merges the conflict copies Syncthing makes when a list changed on two devices. Built with `-DBUILD_BACKEND_SYNCTHING=ON` (the default). Source id: `syncthing`; Add Source… picks it for a folder inside one holding `.stfolder`.

```ini
[source.personal]
backend=syncthing
folder=~/Sync/Reminders
```

## Using it

```
Groceries.md                      ← a list (synced)
.reminders/<this-computer>/       ← this computer's working state (not synced)
```

- **Conflicts:** if two devices change the same list before syncing, Syncthing
  saves a `Groceries.sync-conflict-….md` copy. Reminders merges it into
  `Groceries.md` and deletes it:
  - Changes to different reminders, or to different fields of one reminder,
    are all kept.
  - If both devices changed the same field, the version Syncthing kept as
    `Groceries.md` wins.
- **`.reminders/`** holds, for each list, the last version received from
  another device (the starting point for merges) and a fingerprint of this
  computer's last save. Reminders adds `(?d).reminders` to the `.stignore` at
  the root of your Syncthing folder so that it isn't synced.
- **Deleting a list** moves its file to the Trash on that computer, and
  Syncthing deletes it on the others.

## Per-device state

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

How conflict copies are merged is in [the folder format](../../docs/FORMAT.md#conflicts).
