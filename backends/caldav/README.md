# CalDAV back end

Lists kept on a CalDAV server, one calendar per list, with a local copy in the usual folder format. Built with `-DBUILD_BACKEND_CALDAV=ON` (the default; needs libcurl). Source id: `caldav`.

## Using it

A CalDAV source shows the task lists of an account on a CalDAV server
(Nextcloud, Fastmail, Radicale, iCloud with an app-specific password, …):
each of its task lists is a list here, and each list made here becomes a task
list there. Add one with ☰ → Sources… → **Add Source…**, Type **CalDAV**, or by hand:

```ini
[source.fastmail]
backend=caldav
url=https://caldav.fastmail.com/
username=you@fastmail.com
password-command=secret-tool lookup service reminders-caldav
interval=15
title=Fastmail
```

- **`url=`** is the server's address. The app finds the calendars from it
  (through `/.well-known/caldav` and your principal), so the server's root
  usually works; the address of your calendar home works too.
- **`password-command=`** is a command that prints the password, so it
  isn't kept in the settings file. For example, store it in the GNOME
  keyring once with `secret-tool store --label "Reminders CalDAV" service
  reminders-caldav`, then use the line above; `pass show caldav` works too.
  Use an app-specific password where the server offers them.
- **When it syncs:** when the app opens, every `interval=` minutes (default
  15), and a couple of seconds after you change something. **☰ → Sync All** (Ctrl+Shift+S)
  syncs every source straight away; **Sync Now** in the menu of the source's
  sidebar heading (right-click it), or Ctrl+S in one of its lists, syncs just that one. Offline changes wait in the local copy until the
  next sync.
- **The local copy** is in `$XDG_DATA_HOME/reminders/caldav/NAME/`
  (`~/.local/share/…` when `$XDG_DATA_HOME` isn't set; `folder=` moves it): one Markdown file per task list, like any other
  source, which you can open in an editor.
- **Changes on both sides** are merged reminder by reminder, field by field,
  as for [Syncthing conflicts](../syncthing/README.md); when both sides changed the same field, this
  device's change wins. Properties the app doesn't use (alarms, start
  dates, other apps' extras) are kept as they are on the server.
- **Limits:** the order of reminders and their sections are sent to the
  server (as `X-APPLE-SORT-ORDER` and `X-REMINDERS-SECTION`), but moves made
  in another app aren't picked up yet. Repeat rules that the app can't
  express (say, "the first Monday of the month") are kept but not shown.
  Deleting a list here deletes its task list on the server.
- **Problems** (wrong password, server unreachable) show as a message at
  the bottom of the window; the next sync tries again.

## On the server and on disk

A client can also keep lists on a CalDAV server: one calendar (collection)
per list, one VTODO per reminder. Its local copy is a folder in the [folder format](../../docs/FORMAT.md). Every client that does this maps the fields the same way:

| Reminder | VTODO |
|---|---|
| title | `SUMMARY` (line breaks become spaces) |
| notes | `DESCRIPTION` |
| done / completed date | `STATUS:COMPLETED` (or `CANCELLED`), `COMPLETED` (UTC; written as local noon of that day) |
| due | `DUE`: `VALUE=DATE` for all-day, else a floating local time; UTC and `TZID` times are read in local time |
| priority | `PRIORITY`: 1–4 high, 5 medium, 6–9 low; written as 1, 5, 9 |
| repeat | `RRULE` `FREQ=DAILY/WEEKLY/MONTHLY/YEARLY` with `INTERVAL`, and `FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR` / `SA,SU` for weekday / weekend. Other rules are kept but not shown. |
| tags | `CATEGORIES` (characters a tag can't have become `-`) |
| url | `URL` |
| created | `CREATED` |
| flagged | `X-REMINDERS-FLAGGED:TRUE` |
| section | `X-REMINDERS-SECTION` |
| order | `X-APPLE-SORT-ORDER` (integers, ascending; existing values are kept where they still increase) |
| subtask | `RELATED-TO` (`RELTYPE=PARENT`, the default) naming the parent's `UID`; deeper nesting is shown under the top parent |
| list colour | the calendar's `calendar-color` (Apple's namespace), mapped to the nearest colour |
| list name | the calendar's `displayname` |

- A reminder's `^id` and its VTODO's `UID` are linked in the client's
  per-device records. A UID that is already a valid id is used as the id;
  reminders created by the client get a random UUID as their UID.
- When writing back a VTODO, clients change only the properties whose
  meaning changed and keep every other property (alarms, `DTSTART`, other
  clients' `X-` properties) exactly as the server sent it. A change bumps
  `SEQUENCE` and sets `DTSTAMP` and `LAST-MODIFIED`.
- Changes are merged three-way, as for [conflicts](../../docs/FORMAT.md#conflicts): the base is the
  list as of the last sync, "main" is the local copy and the "conflict copy"
  is the server's version.

## Records this client keeps

In its per-device state folder (`$XDG_STATE_HOME/reminders/<device>/<source>/caldav/`):

| File | Holds |
|---|---|
| `calendars.tsv` | the calendars found, with each one's CTag |
| `<calendar>/items.tsv` | per reminder: its `^id`, the VTODO's `UID` and `ETag` |
| `<calendar>/base.md` | the list as of the last sync — the merge base |
| `<calendar>/<id>.ics` | the raw VTODO as the server sent it, so unknown properties survive a round trip |

A sync pulls (CTag, then ETags, then multiget) into "theirs" = `base.md` plus
the server's changes, merges against `base.md`, writes the list under the back
end's write lock (aborting if the file changed meanwhile), then pushes changed
VTODOs. A failed precondition (412) leaves the base at the server's version and
is retried at the next sync. Only a delete made in an app removes the server
calendar; a list file that merely vanished is fetched again.
