#include "support.hpp"

#include <adwaita.h>

#include <format>

#include "gtk_util.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"

namespace ui {

namespace {

GDateTime* to_gdatetime(rem::Date d, int hour = 0, int minute = 0) {
	return g_date_time_new_local(
		static_cast<int>(d.year()),
		static_cast<int>(static_cast<unsigned>(d.month())),
		static_cast<int>(static_cast<unsigned>(d.day())), hour, minute, 0);
}

std::string format_date(rem::Date d, const char* fmt) {
	GDateTime* dt = to_gdatetime(d);
	auto s = take_string(g_date_time_format(dt, fmt));
	g_date_time_unref(dt);
	return s;
}

bool uses_12h_clock() {
	auto* source = g_settings_schema_source_get_default();
	if (!source) {
		return false;
	}
	auto* schema = g_settings_schema_source_lookup(
		source, "org.gnome.desktop.interface", TRUE);
	if (!schema) {
		return false;
	}
	bool has_key = g_settings_schema_has_key(schema, "clock-format");
	g_settings_schema_unref(schema);
	if (!has_key) {
		return false;
	}
	auto settings =
		Obj<GSettings>::adopt(g_settings_new("org.gnome.desktop.interface"));
	return take_string(g_settings_get_string(settings.get(), "clock-format")) ==
		   "12h";
}

}  // namespace

const char* list_icon_name(std::string_view icon) {
	static constexpr std::pair<std::string_view, const char*> map[] = {
		{"list", "view-list-bullet-symbolic"},
		{"tag", "sr-tag-symbolic"},
		{"bookmark", "user-bookmarks-symbolic"},
		{"cart", "sr-cart-symbolic"},
		{"gift", "sr-gift-symbolic"},
		{"home", "user-home-symbolic"},
		{"work", "sr-work-symbolic"},
		{"school", "sr-school-symbolic"},
		{"calendar", "x-office-calendar-symbolic"},
		{"flag", "sr-flag-symbolic"},
		{"star", "starred-symbolic"},
		{"heart", "sr-heart-symbolic"},
		{"music", "audio-x-generic-symbolic"},
		{"game", "applications-games-symbolic"},
		{"book", "sr-book-symbolic"},
		{"food", "emoji-food-symbolic"},
		{"travel", "emoji-travel-symbolic"},
		{"nature", "emoji-nature-symbolic"},
		{"person", "avatar-default-symbolic"},
		{"people", "system-users-symbolic"},
		{"money", "sr-money-symbolic"},
		{"pill", "sr-pill-symbolic"},
		{"computer", "computer-symbolic"},
		{"camera", "camera-photo-symbolic"},
	};
	for (auto& [k, v] : map) {
		if (k == icon) {
			return v;
		}
	}
	return map[0].second;
}

std::string color_label(std::string_view color) {
	std::string s(color);
	if (!s.empty()) {
		s[0] =
			static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
	}
	return s;
}

rem::Date today() {
	GDateTime* now = g_date_time_new_now_local();
	rem::Date d{
		std::chrono::year{g_date_time_get_year(now)},
		std::chrono::month{static_cast<unsigned>(g_date_time_get_month(now))},
		std::chrono::day{
			static_cast<unsigned>(g_date_time_get_day_of_month(now))}};
	g_date_time_unref(now);
	return d;
}

std::string relative_date(rem::Date d, rem::Date today) {
	using namespace std::chrono;
	auto diff = (sys_days{d} - sys_days{today}).count();
	if (diff == 0) {
		return "Today";
	}
	if (diff == 1) {
		return "Tomorrow";
	}
	if (diff == -1) {
		return "Yesterday";
	}
	if (diff > 1 && diff < 7) {
		return format_date(d, "%A");
	}
	if (d.year() == today.year()) {
		return format_date(d, "%b %-d");
	}
	return format_date(d, "%b %-d, %Y");
}

std::string format_time(rem::TimeOfDay t) {
	if (uses_12h_clock()) {
		return std::format("{}:{:02} {}", (t.hour + 11) % 12 + 1, t.minute,
						   t.hour < 12 ? "AM" : "PM");
	}
	return std::format("{:02}:{:02}", t.hour, t.minute);
}

std::string due_label(const rem::Reminder& r, rem::Date today) {
	if (!r.due_date) {
		return {};
	}
	auto s = relative_date(*r.due_date, today);
	if (r.due_time) {
		s += ", " + format_time(*r.due_time);
	}
	return s;
}

bool is_overdue(const rem::Reminder& r, rem::Date today) {
	if (r.done || !r.due_date) {
		return false;
	}
	if (*r.due_date != today) {
		return *r.due_date < today;
	}
	if (!r.due_time) {
		return false;
	}
	GDateTime* now = g_date_time_new_now_local();
	int minutes = g_date_time_get_hour(now) * 60 + g_date_time_get_minute(now);
	g_date_time_unref(now);
	return r.due_time->hour * 60 + r.due_time->minute < minutes;
}

std::optional<std::filesystem::path> load_folder() {
	return rem::saved_folder(rem::Profile());
}

std::string load_last_view() {
	return rem::load_setting(rem::Profile(), "view");
}

void save_last_view(const std::string& view) {
	rem::save_setting(rem::Profile(), "view", view);
}

std::string device_name() { return rem::device_name(); }

}  // namespace ui
