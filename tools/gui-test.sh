#!/bin/bash
# Runs the GNOME app for a test in a private, headless session, never on the
# user's desktop: its own D-Bus session (the app is single-instance), a
# headless mutter on a private Wayland socket (never wayland-0, the desktop's),
# GDK forced to Wayland so a failed connection can't fall back to the user's
# X display, and DISPLAY unset. Settings, data and state go under HOME_DIR.
#
#   tools/gui-test.sh HOME_DIR [APP_ARGS...]
#
# Pass-through: REMINDERS_SCREENSHOT=out.png renders the window and quits.
set -euo pipefail
home="$1"
shift
app="$(cd "$(dirname "$0")/.." && pwd)/build/bin/Reminders"
sock="reminders-test-$$"
exec env -u DISPLAY HOME="$home" XDG_CONFIG_HOME= XDG_DATA_HOME="$home/data" XDG_STATE_HOME="$home/state" \
	GTK_A11Y=none GDK_BACKEND=wayland TEST_SOCK="$sock" \
	timeout 60 dbus-run-session -- bash -c '
		mutter --headless --wayland --no-x11 --wayland-display="$TEST_SOCK" --virtual-monitor 1100x750 >/dev/null 2>&1 &
		for i in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/$TEST_SOCK" ] && break; sleep 0.1; done
		if [ ! -S "$XDG_RUNTIME_DIR/$TEST_SOCK" ]; then echo "gui-test: no private display" >&2; kill %1; exit 1; fi
		WAYLAND_DISPLAY="$TEST_SOCK" "$@" || true
		kill %1' _ "$app" "$@"
