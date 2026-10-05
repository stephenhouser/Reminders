# Local Folder back end

Just a folder: list files are read and saved as they are, and changes made by other programs (an editor, another sync tool) show up straight away. Nothing is synced by the app. Built with `-DBUILD_BACKEND_LOCAL=ON` (the default). Source id: `local`.

```ini
[source.notes]
backend=local
folder=~/Documents/Reminders
```

- A source naming a back end this build doesn't have is opened as a local folder.
- This computer's records for it are in `$XDG_STATE_HOME/reminders/DEVICE/NAME/` (`~/.local/state/…`).
- **Erase all source data** in Remove Source… erases the folder and everything in it.
