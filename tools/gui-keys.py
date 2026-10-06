#!/usr/bin/env python3
"""Types and clicks into the GNOME app during a test, as a person would.

Run by tools/gui-test.sh when STEPS=FILE is set, inside its private D-Bus
session and headless mutter, never on the user's desktop:

    STEPS=steps.txt tools/gui-test.sh HOME_DIR [APP_ARGS...]

Input goes through mutter's RemoteDesktop API on that private bus, so it
takes the app's real path: Wayland events, GTK shortcuts and key handlers,
popovers and their grabs. Check the results in the list files (and undo,
and paste-back) or with REMINDERS_SCREENSHOT plus
REMINDERS_SCREENSHOT_DELAY, long enough to come after the steps.

Steps, one per line:

    key [MOD ...] KEY   tap KEY with modifiers held: key CTRL SHIFT a,
                        key SHIFT F10, key CTRL 7, key ALT RET, key CTRL ,
    move DX DY          move the pointer by DX, DY pixels
    click left|right    click a pointer button where the pointer is
    sleep SECONDS       wait
    shell COMMAND       run COMMAND (inspect files, push a commit, …)
    # comment

KEY is one character, a name below (CTRL SHIFT ALT, RET TAB ESC SPACE DEL
BACKSPACE, UP DOWN LEFT RIGHT HOME END PAGEUP PAGEDOWN, MENU, F1 … F12), or
an X keysym in hex (0xff67). The pointer starts outside the window; on the
1100x750 virtual monitor, two moves of 200 150 put it at (325, 270) on the
window's surface, which has a 25 px shadow margin: (300, 245) in a
screenshot. Then 200 150, -215 95 lands on the first of your lists in the
sidebar of a 900x640 window. To see where it is, run with
WAYLAND_DEBUG=client set for the app and read its wl_pointer.enter / motion
events (and set_window_geometry for the margin).

Lessons built in, each of which once made a correct app look broken:
- One D-Bus connection for the whole run: mutter ends a RemoteDesktop
  session when the connection that made it closes, so per-call `gdbus`
  invocations can't drive it.
- A lone Shift tap first: the app's wl_keyboard only appears with the
  virtual keyboard, which mutter makes on the first key, so the first real
  key would otherwise be lost.
- Keys held for ~100 ms: released within a millisecond, a popover opened
  from Shift+F10 was dismissed by mutter at once.
"""
import subprocess
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

KEYS = {
    "CTRL": 0xFFE3, "SHIFT": 0xFFE1, "ALT": 0xFFE9,
    "RET": 0xFF0D, "TAB": 0xFF09, "ESC": 0xFF1B, "SPACE": 0x20,
    "DEL": 0xFFFF, "BACKSPACE": 0xFF08,
    "UP": 0xFF52, "DOWN": 0xFF54, "LEFT": 0xFF51, "RIGHT": 0xFF53,
    "HOME": 0xFF50, "END": 0xFF57, "PAGEUP": 0xFF55, "PAGEDOWN": 0xFF56,
    "MENU": 0xFF67,
    **{f"F{n}": 0xFFBE + n - 1 for n in range(1, 13)},
}
BUTTONS = {"left": 0x110, "right": 0x111}  # BTN_LEFT, BTN_RIGHT

RD = "org.gnome.Mutter.RemoteDesktop"
bus = Gio.bus_get_sync(Gio.BusType.SESSION)


def call(path, iface, method, args=None):
    return bus.call_sync(RD, path, iface, method, args, None,
                         Gio.DBusCallFlags.NONE, -1, None)


def keysym(name):
    if name in KEYS:
        return KEYS[name]
    if len(name) == 1:
        return ord(name)
    if name.lower().startswith("0x"):
        return int(name, 16)
    sys.exit(f"gui-keys: unknown key {name!r}")


def main(path):
    session = call("/org/gnome/Mutter/RemoteDesktop", RD,
                   "CreateSession").unpack()[0]
    s = RD + ".Session"
    call(session, s, "Start")

    def send(sym, down):
        call(session, s, "NotifyKeyboardKeysym",
             GLib.Variant("(ub)", (sym, down)))

    def button(code, down):
        call(session, s, "NotifyPointerButton",
             GLib.Variant("(ib)", (code, down)))

    time.sleep(2)  # the window up
    send(KEYS["SHIFT"], True)  # makes the app's keyboard: see above
    send(KEYS["SHIFT"], False)
    time.sleep(0.7)

    for number, line in enumerate(open(path), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        word, _, rest = line.partition(" ")
        if word == "key":
            syms = [keysym(k) for k in rest.split()]
            for sym in syms:
                send(sym, True)
                time.sleep(0.03)
            time.sleep(0.1)
            for sym in reversed(syms):
                send(sym, False)
                time.sleep(0.03)
            time.sleep(0.3)
        elif word == "move":
            dx, dy = (float(v) for v in rest.split())
            call(session, s, "NotifyPointerMotionRelative",
                 GLib.Variant("(dd)", (dx, dy)))
            time.sleep(0.2)
        elif word == "click":
            code = BUTTONS[rest.strip()]
            button(code, True)
            time.sleep(0.1)
            button(code, False)
            time.sleep(0.4)
        elif word == "sleep":
            time.sleep(float(rest))
        elif word == "shell":
            subprocess.run(rest, shell=True, check=False)
        else:
            sys.exit(f"gui-keys: {path}:{number}: unknown step {line!r}")

    call(session, s, "Stop")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: gui-keys.py STEPS_FILE (run by tools/gui-test.sh)")
    main(sys.argv[1])
