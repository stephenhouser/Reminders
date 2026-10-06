#!/bin/bash
# Runs the GNOME app for a test in a private, headless session, never on the
# user's desktop: its own D-Bus session (the app is single-instance), a
# headless mutter on a private Wayland socket (never wayland-0, the desktop's),
# GDK forced to Wayland so a failed connection can't fall back to the user's
# X display, and DISPLAY and WAYLAND_DISPLAY unset. Services the private bus
# starts (the file-chooser portal, from Import…) see only the private socket:
# they once inherited wayland-0 and drew on the user's screen. Settings, data
# and state go under HOME_DIR.
#
#   tools/gui-test.sh HOME_DIR [APP_ARGS...]
#   STEPS=steps.txt tools/gui-test.sh HOME_DIR [APP_ARGS...]
#
# With STEPS, the app runs in the background while tools/gui-keys.py types
# and clicks into it as the file says, then it's closed (or, with a
# screenshot asked for, left to take it and quit).
#
# Pass-through: REMINDERS_SCREENSHOT=out.png renders the window and quits,
# after REMINDERS_SCREENSHOT_DELAY ms (default 1500; longer than the steps).
set -euo pipefail
home="$1"
shift
tools="$(cd "$(dirname "$0")" && pwd)"
app="$tools/../build/bin/Reminders"
steps="${STEPS:+$(realpath "$STEPS")}"
sock="reminders-test-$$"
exec env -u DISPLAY -u WAYLAND_DISPLAY -u STEPS HOME="$home" XDG_CONFIG_HOME= XDG_DATA_HOME="$home/data" XDG_STATE_HOME="$home/state" \
	GTK_A11Y=none GDK_BACKEND=wayland TEST_SOCK="$sock" TEST_STEPS="$steps" TEST_KEYS="$tools/gui-keys.py" \
	timeout 120 dbus-run-session -- bash -c '
		mutter --headless --wayland --no-x11 --wayland-display="$TEST_SOCK" --virtual-monitor 1100x750 >/dev/null 2>&1 &
		for i in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/$TEST_SOCK" ] && break; sleep 0.1; done
		if [ ! -S "$XDG_RUNTIME_DIR/$TEST_SOCK" ]; then echo "gui-test: no private display" >&2; kill %1; exit 1; fi
		dbus-update-activation-environment WAYLAND_DISPLAY="$TEST_SOCK"
		if [ -z "$TEST_STEPS" ]; then
			WAYLAND_DISPLAY="$TEST_SOCK" "$@" || true
		else
			WAYLAND_DISPLAY="$TEST_SOCK" "$@" &
			app_pid=$!
			python3 "$TEST_KEYS" "$TEST_STEPS" || echo "gui-test: steps failed" >&2
			[ -n "${REMINDERS_SCREENSHOT:-}" ] || kill $app_pid 2>/dev/null || true
			wait $app_pid 2>/dev/null || true
		fi
		kill %1' _ "$app" "$@"
