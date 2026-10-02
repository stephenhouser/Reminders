// The interactive terminal interface (ncurses).
#pragma once

#include <filesystem>
#include <optional>

#include "reminders/library.hpp"

// Runs until the user quits; returns the process exit code. With `remember`,
// it opens on the view saved in settings.ini and saves the view as it changes
// (shared with the GNOME app and the CLI).
// `key_numbers` overrides the show-key-numbers setting.
int run_tui(rem::Library& store, bool remember, std::optional<bool> key_numbers = std::nullopt);
