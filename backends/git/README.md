# Git back end

A folder in a git repository: changed lists are committed, pulled and pushed by running `git`. Built with `-DBUILD_BACKEND_GIT=ON` (the default; needs `git` at run time). Source id: `git`.

## Using it

A git source keeps its lists in a folder of a git repository, such as a
notes repository you already have, or one made for the purpose on GitHub,
GitLab, Gitea or your own server. Each list is a file in the repository,
as in any folder, so the history of every list is in git. Add one with
☰ → Sources… → **Add Source…**, Type **Git**, or by hand:

```ini
[source.notes]
backend=git
folder=~/notes/todo
url=git@github.com:you/notes.git
interval=15
title=Notes
```

- **`folder=`** is the folder that holds the lists: the top of a working
  tree (a clone), a folder in one, or any folder at all. Only the list files
  directly in it (`*.md`) are committed. Other files in the repository, and
  the folders under it, are left alone. Without `folder=` (or with no folder
  chosen in Add Source…) it's `$XDG_DATA_HOME/reminders/git/NAME`
  (`~/.local/share/…` when `$XDG_DATA_HOME` isn't set).
- **A folder that isn't in a repository yet** (empty, missing, or already
  holding lists) becomes one on the first sync: cloned from `url=` when
  there is one, else a new repository of its own (`git init`), whose
  commits are your lists' history.
- **`url=`** (Clone From) is optional: the repository to clone, or to push
  to. Added to a repository that doesn't have that remote yet (for example
  one Reminders made), it's added as the remote, and the next sync pulls
  and pushes.
- **`remote=`** and **`branch=`** are optional: by default `origin` and
  the branch checked out.
- **Signing in** is git's own business. SSH keys and credential helpers
  work as they do on the command line. Reminders never asks for a password:
  a sync that needs one fails with git's message. On a server that wants
  one, set up a key or a credential helper first.
- **When it syncs:** when the app opens, every `interval=` minutes (default
  15), and a couple of seconds after you change something; **☰ → Sync
  All** syncs every source straight away, and **Sync Now** in the menu of
  the source's sidebar heading (right-click it) just this one. Each sync commits the lists that changed, as
  "Reminders (this computer): Groceries, Home", then pulls and pushes.
- **Changes on both sides** of a list are merged by Reminders, reminder by
  reminder, field by field, as for [Syncthing conflicts](../syncthing/README.md), not line by line
  as git would. When both changed the same field, this computer's change
  wins. A conflict in a file that isn't a list stops the merge (it's
  undone) and the message names the file: merge it with git.
- **A repository without a remote** works too: every change is committed,
  so you get a history of your lists and nothing else. Give it a `url=`
  (Clone From in Source Info…) later to start pushing.
- **Removing the source** leaves the folder as it is, even a clone in the
  default place, since it may hold commits that haven't been pushed, unless
  you tick **Erase all source data**, which erases the folder from this
  computer (the remote keeps everything pushed).

## On the server and on disk

A client can also keep a folder of list files in a git repository. The
list files are committed as they are (only the folder's top-level `*.md`).
When a merge leaves a list file in conflict, clients merge it as for
conflicts (below), with git's merge base as the base, the local version as
"main" and the incoming one as the "conflict copy", and commit the result.
They don't leave git's conflict markers in a list file.
