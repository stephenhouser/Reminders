// The interactive terminal interface (ncurses).
#pragma once

#include <filesystem>

#include "reminders/store.hpp"

// Runs until the user quits; returns the process exit code.
int run_tui(rem::Store& store, const std::filesystem::path& folder);
