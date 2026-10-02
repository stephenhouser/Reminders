// Text shared by the CLI and the TUI: colours, due labels, priority marks.
#pragma once

#include <format>
#include <string>
#include <string_view>

#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/model.hpp"

namespace term {

// The list colours as RGB (the same palette as the GNOME app).
struct Rgb {
    int r, g, b;
};

inline Rgb color_rgb(std::string_view name) {
    struct Entry {
        std::string_view name;
        Rgb rgb;
    };
    static constexpr Entry table[] = {
        {"red", {0xe6, 0x2d, 0x42}},    {"orange", {0xed, 0x5b, 0x00}}, {"yellow", {0xc8, 0x88, 0x00}},
        {"green", {0x3a, 0x94, 0x4a}},  {"cyan", {0x21, 0x90, 0xa4}},   {"blue", {0x35, 0x84, 0xe4}},
        {"indigo", {0x5b, 0x5f, 0xc7}}, {"purple", {0x91, 0x41, 0xac}}, {"pink", {0xd5, 0x61, 0x99}},
        {"brown", {0x98, 0x6a, 0x44}},  {"gray", {0x6f, 0x83, 0x96}},
    };
    for (auto& e : table)
        if (e.name == name) return e.rgb;
    return {0x35, 0x84, 0xe4};
}

inline std::string priority_marks(rem::Priority p) {
    return std::string(static_cast<std::size_t>(p), '!');
}

inline const char* priority_name(rem::Priority p) {
    switch (p) {
        case rem::Priority::Low: return "low";
        case rem::Priority::Medium: return "medium";
        case rem::Priority::High: return "high";
        case rem::Priority::None: break;
    }
    return "none";
}

// "Today 17:30", "Fri", "Oct 3".
inline std::string due_label(const rem::Reminder& r, rem::Date today) {
    if (!r.due_date) return {};
    auto s = rem::relative_date(*r.due_date, today);
    if (r.due_time) s += " " + rem::format_time(*r.due_time);
    return s;
}

inline bool is_overdue(const rem::Reminder& r, rem::Date today) {
    if (r.done || !r.due_date) return false;
    if (*r.due_date != today) return *r.due_date < today;
    return r.due_time && r.due_time->hour * 60 + r.due_time->minute < rem::local_minutes_now();
}

inline std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace term
