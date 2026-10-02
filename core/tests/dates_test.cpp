#include <cstdlib>
#include <fstream>

#include "reminders/dates.hpp"
#include "reminders/settings.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {
Date d(int y, unsigned m, unsigned day) { return Date{year{y}, month{m}, std::chrono::day{day}}; }
}  // namespace

TEST(human_dates) {
    auto today = d(2026, 10, 1);  // a Thursday
    CHECK(parse_human_date("today", today) == today);
    CHECK(parse_human_date("Tomorrow", today) == d(2026, 10, 2));
    CHECK(parse_human_date("2026-12-25", today) == d(2026, 12, 25));
    CHECK(parse_human_date("+3d", today) == d(2026, 10, 4));
    CHECK(parse_human_date("+2w", today) == d(2026, 10, 15));
    CHECK(parse_human_date("+1m", today) == d(2026, 11, 1));
    CHECK(parse_human_date("fri", today) == d(2026, 10, 2));
    CHECK(parse_human_date("thursday", today) == d(2026, 10, 8));  // next week, never today
    CHECK(!parse_human_date("someday", today));
    CHECK(!parse_human_date("fr", today));
    CHECK(!parse_human_date("+x", today));
}

TEST(relative_dates) {
    auto today = d(2026, 10, 1);
    CHECK_EQ(relative_date(today, today), "Today");
    CHECK_EQ(relative_date(d(2026, 10, 2), today), "Tomorrow");
    CHECK_EQ(relative_date(d(2026, 10, 5), today), "Monday");
    CHECK_EQ(relative_date(d(2026, 12, 25), today), "Dec 25");
    CHECK_EQ(relative_date(d(2027, 1, 3), today), "Jan 3, 2027");
}

TEST(settings_round_trip_keeps_other_lines) {
    auto dir = fs::temp_directory_path() / ("reminders-settings-" + new_id());
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    fs::create_directories(dir / "reminders");
    std::ofstream(dir / "reminders" / "settings.ini") << "# mine\n[other]\nfolder=x\n[general]\nview=all\n";
    CHECK_EQ(load_setting("folder"), "");
    CHECK_EQ(load_setting("view"), "all");
    save_setting("folder", "/tmp/lists");
    save_setting("view", "today");
    CHECK_EQ(load_setting("folder"), "/tmp/lists");
    std::ifstream in(dir / "reminders" / "settings.ini");
    std::string all((std::istreambuf_iterator<char>(in)), {});
    CHECK_EQ(all, "# mine\n[other]\nfolder=x\n[general]\nview=today\nfolder=/tmp/lists\n");
    CHECK(device_name().find('-') != std::string::npos);
    unsetenv("XDG_CONFIG_HOME");
    fs::remove_all(dir);
}
