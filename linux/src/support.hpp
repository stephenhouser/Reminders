// Odds and ends shared by the UI: icon names, dates, settings.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "reminders/model.hpp"

namespace ui {

inline constexpr const char* kAppId = "com.stephenhouser.Reminders";

// Icon theme name for a list icon from the format spec ("cart", "home", ...).
const char* list_icon_name(std::string_view icon);

// Human label for a colour name ("red" → "Red").
std::string color_label(std::string_view color);

rem::Date today();
// "Today", "Tomorrow", "Yesterday", "Friday", "Oct 3", "Oct 3, 2027".
std::string relative_date(rem::Date d, rem::Date today);
// Formatted according to the desktop's 12/24-hour setting.
std::string format_time(rem::TimeOfDay t);
// "Today, 09:00" or just the date for all-day reminders.
std::string due_label(const rem::Reminder& r, rem::Date today);
bool is_overdue(const rem::Reminder& r, rem::Date today);

// Persistent settings in $XDG_CONFIG_HOME/reminders/settings.ini.
std::optional<std::filesystem::path> load_folder();
// The last view, as "today", "list:Groceries", "tag:errands", ...
std::string load_last_view();
void save_last_view(const std::string& view);

// This computer's name for its per-device state folder inside the synced
// folder (<folder>/.reminders/<device>/), e.g. "laptop-3f9a". Stable across runs.
std::string device_name();

}  // namespace ui
