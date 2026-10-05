# WebDAV back end

List files kept in a folder on a WebDAV server, as they are, with a local copy. Built with `-DBUILD_BACKEND_WEBDAV=ON` (the default; needs libcurl). Source id: `webdav`.

## Using it

A WebDAV source keeps the list files themselves in a folder on a WebDAV
server: Nextcloud or ownCloud (their Files), a NAS, Apache's mod_dav, and so
on. Unlike CalDAV nothing is translated, since the files on the server are
the same Markdown files. Sections, order and notes all come through, and
every computer running Reminders can use the same folder. Add one with
☰ → Sources… → **Add Source…**, Type **WebDAV**, or by hand:

```ini
[source.cloud]
backend=webdav
url=https://cloud.example.com/remote.php/dav/files/you/Reminders/
username=you
password-command=secret-tool lookup service reminders-webdav
interval=15
title=Cloud
```

- **`url=`** is the folder on the server. For Nextcloud, take the WebDAV
  address from Files → Files settings (bottom left) and add a folder name.
  The folder is made on the first sync if it isn't there, but the folder it
  goes in must exist. Spaces can be written as they are.
- **`password-command=`** and **`interval=`** work as for CalDAV (above).
  On Nextcloud, use an app password (Settings → Security).
- **What syncs:** the folder's `.md` files, not its subfolders. Every file
  on the server is copied here, and one that isn't a list yet shows up to
  review, as in any folder. A file made here goes to the server once it's a
  list.
- **Changes on both sides** of a list are merged reminder by reminder,
  field by field, as for [Syncthing conflicts](../syncthing/README.md). When both changed the same
  field, this computer's change wins. Each write is conditional on the
  server's version (its ETag): a change another device made meanwhile is
  merged at the next sync, never overwritten. A file that isn't a list and
  changed on both sides can't be merged, so the server's version wins and
  this computer's is kept beside it as `NAME (this device).md`.
- **Renaming a list** renames its file on the server. **Deleting a list**
  deletes it there too, unless it changed there since; then it comes back.
  A file deleted on the server goes here too, unless it changed here since;
  then it's sent again.
- **The local copy** is in `$XDG_DATA_HOME/reminders/webdav/NAME/`
  (`~/.local/share/…` when `$XDG_DATA_HOME` isn't set; `folder=` moves it).
- **Problems** show as a message at the bottom of the window; the next sync
  tries again.

## On the server and on disk

A client can also keep a folder of list files on a WebDAV server, with a
local copy in the [folder format](../../docs/FORMAT.md). The files on the server are the list files
themselves (`NAME.md`, at the folder's top level), so every client reads
them as it would a synced folder. Clients that sync with such a folder:

- write with `If-Match` (the ETag of the version they merged with), or with
  `If-None-Match: *` for a new file. When the precondition fails, they merge
  again at the next sync;
- merge three-way as for [conflicts](../../docs/FORMAT.md#conflicts): the base is the file as of the
  last sync, "main" is the local copy and the "conflict copy" is the
  server's version;
- rename with `MOVE` and `Overwrite: F`, and delete with `DELETE` and
  `If-Match`.

No `.reminders/` folder or conflict copies are kept on the server.
