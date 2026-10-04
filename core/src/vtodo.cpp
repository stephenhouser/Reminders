#include "reminders/vtodo.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <map>

namespace rem {

namespace {

using namespace std::chrono;

constexpr std::string_view kFlagged = "X-REMINDERS-FLAGGED";
constexpr std::string_view kSection = "X-REMINDERS-SECTION";
constexpr std::string_view kSortOrder = "X-APPLE-SORT-ORDER";

std::string lower(std::string_view s) {
	std::string out(s);
	for (auto& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

std::optional<long long> number(std::string_view s) {
	while (!s.empty() && s.front() == ' ') {
		s.remove_prefix(1);
	}
	while (!s.empty() && s.back() == ' ') {
		s.remove_suffix(1);
	}
	long long n = 0;
	auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
	if (ec != std::errc{} || p != s.data() + s.size()) {
		return std::nullopt;
	}
	return n;
}

std::string one_line(std::string s) {
	std::ranges::replace(s, '\n', ' ');
	std::erase(s, '\r');
	return s;
}

std::string clean_notes(std::string s) {
	std::erase(s, '\r');
	while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) {
		s.pop_back();
	}
	return s;
}

Priority priority_of(const IcalComponent& t) {
	auto* p = t.prop("PRIORITY");
	auto n = p ? number(p->value) : std::nullopt;
	if (!n || *n <= 0) {
		return Priority::None;
	}
	if (*n <= 4) {
		return Priority::High;
	}
	if (*n == 5) {
		return Priority::Medium;
	}
	return Priority::Low;
}

std::optional<Date> date_of(const IcalComponent& t, std::string_view name,
							const time_zone* local) {
	auto* p = t.prop(name);
	auto w = p ? ical_when(*p, local) : std::nullopt;
	return w ? std::optional{w->date} : std::nullopt;
}

// COMPLETED / CREATED are UTC DATE-TIMEs; this app only keeps the date, so
// write local noon of that day.
std::string utc_for(Date d, const time_zone* local) {
	auto t = local_days{d} + hours{12};
	return ical_utc(local ? local->to_sys(t, choose::earliest)
						  : sys_seconds{t.time_since_epoch()});
}

std::optional<std::string> parent_of(const IcalComponent& t) {
	for (auto* p : t.all("RELATED-TO")) {
		auto type = p->param("RELTYPE");
		if (!type || lower(*type) == "parent") {
			return ical_text(p->value);
		}
	}
	return std::nullopt;
}

}  // namespace

const IcalComponent* todo_of(const IcalComponent& calendar) {
	return calendar.child("VTODO");
}
IcalComponent* todo_of(IcalComponent& calendar) {
	return calendar.child("VTODO");
}

std::string tag_from_category(std::string_view category) {
	std::string out;
	for (char c : category) {
		auto u = static_cast<unsigned char>(c);
		if (std::isalnum(u) || c == '_' || c == '/' || c == '-') {
			out += c;
		} else if (std::isspace(u) && !out.empty() && out.back() != '-') {
			out += '-';
		}
	}
	while (!out.empty() && out.back() == '-') {
		out.pop_back();
	}
	if (!out.empty() && !std::isalpha(static_cast<unsigned char>(out[0])) &&
		out[0] != '_') {
		out.insert(0, "_");
	}
	return out;
}

std::optional<std::string> repeat_from_rrule(std::string_view rrule) {
	std::map<std::string, std::string> parts;
	for (std::size_t i = 0; i < rrule.size();) {
		auto semi = rrule.find(';', i);
		auto part = rrule.substr(i, semi == std::string_view::npos
										? std::string_view::npos
										: semi - i);
		i = semi == std::string_view::npos ? rrule.size() : semi + 1;
		auto eq = part.find('=');
		if (eq == std::string_view::npos) {
			return std::nullopt;
		}
		std::string k(part.substr(0, eq)), v(part.substr(eq + 1));
		for (auto& c : k) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		for (auto& c : v) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		parts[k] = v;
	}
	auto freq = parts["FREQ"];
	long long n = 1;
	if (parts.contains("INTERVAL")) {
		auto v = number(parts["INTERVAL"]);
		if (!v || *v < 1) {
			return std::nullopt;
		}
		n = *v;
	}
	parts.erase("FREQ");
	parts.erase("INTERVAL");
	parts.erase("WKST");
	if (parts.contains("BYDAY")) {
		auto byday = parts["BYDAY"];
		parts.erase("BYDAY");
		if (!parts.empty() || freq != "WEEKLY" || n != 1) {
			return std::nullopt;
		}
		if (byday == "MO,TU,WE,TH,FR") {
			return "every weekday";
		}
		if (byday == "SA,SU" || byday == "SU,SA") {
			return "every weekend";
		}
		return std::nullopt;
	}
	if (!parts.empty()) {
		return std::nullopt;  // COUNT, UNTIL, BYMONTHDAY, …: not expressible
	}
	std::string unit = freq == "DAILY"	 ? "day"
					 : freq == "WEEKLY"	 ? "week"
					 : freq == "MONTHLY" ? "month"
					 : freq == "YEARLY"	 ? "year"
										 : "";
	if (unit.empty()) {
		return std::nullopt;
	}
	return n == 1 ? "every " + unit : std::format("every {} {}s", n, unit);
}

std::optional<std::string> rrule_from_repeat(std::string_view repeat) {
	auto r = lower(repeat);
	if (r == "every weekday") {
		return "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR";
	}
	if (r == "every weekend") {
		return "FREQ=WEEKLY;BYDAY=SA,SU";
	}
	// Reuse the recurrence parser's grammar: "every [N] unit[s]".
	std::vector<std::string> words;
	for (std::size_t i = 0; i < r.size();) {
		while (i < r.size() && r[i] == ' ') {
			++i;
		}
		auto start = i;
		while (i < r.size() && r[i] != ' ') {
			++i;
		}
		if (i > start) {
			words.push_back(r.substr(start, i - start));
		}
	}
	if (words.size() < 2 || words.size() > 3 || words[0] != "every") {
		return std::nullopt;
	}
	long long n = 1;
	if (words.size() == 3) {
		auto v = number(words[1]);
		if (!v || *v < 1) {
			return std::nullopt;
		}
		n = *v;
	}
	auto unit = words.back();
	if (unit.size() > 1 && unit.back() == 's') {
		unit.pop_back();
	}
	std::string freq = unit == "day"   ? "DAILY"
					 : unit == "week"  ? "WEEKLY"
					 : unit == "month" ? "MONTHLY"
					 : unit == "year"  ? "YEARLY"
									   : "";
	if (freq.empty()) {
		return std::nullopt;
	}
	return n == 1 ? "FREQ=" + freq
				  : std::format("FREQ={};INTERVAL={}", freq, n);
}

Todo read_todo(const IcalComponent& t, const time_zone* local) {
	Todo out;
	if (auto* p = t.prop("UID")) {
		out.uid = ical_text(p->value);
	}
	auto& r = out.reminder;
	if (auto* p = t.prop("SUMMARY")) {
		r.title = one_line(ical_text(p->value));
	}
	if (auto* p = t.prop("DESCRIPTION")) {
		r.notes = clean_notes(ical_text(p->value));
	}
	auto* status = t.prop("STATUS");
	auto st = status ? lower(status->value) : "";
	r.done = st == "completed" || st == "cancelled" ||
			 (!status && t.prop("COMPLETED"));
	if (r.done) {
		r.completed = date_of(t, "COMPLETED", local);
	}
	r.created = date_of(t, "CREATED", local);
	if (auto* p = t.prop("DUE")) {
		if (auto w = ical_when(*p, local)) {
			r.due_date = w->date;
			r.due_time = w->time;
		}
	}
	r.priority = priority_of(t);
	if (auto* p = t.prop("RRULE")) {
		r.repeat = repeat_from_rrule(p->value);
	}
	for (auto* p : t.all("CATEGORIES")) {
		for (auto& c : ical_text_list(p->value)) {
			if (auto tag = tag_from_category(c);
				!tag.empty() &&
				std::ranges::find(r.tags, tag) == r.tags.end()) {
				r.tags.push_back(tag);
			}
		}
	}
	if (auto* p = t.prop("URL")) {
		r.url = ical_text(p->value);
	}
	if (auto* p = t.prop(kFlagged)) {
		r.flagged = lower(p->value) == "true" || p->value == "1";
	}
	if (auto* p = t.prop(kSection); p && !p->value.empty()) {
		out.section = one_line(ical_text(p->value));
	}
	if (auto* p = t.prop(kSortOrder)) {
		out.sort_order = number(p->value);
	}
	out.parent_uid = parent_of(t);
	return out;
}

bool write_todo(IcalComponent& t, const Todo& want, sys_seconds now,
				const time_zone* local) {
	auto have = read_todo(t, local);
	auto& h = have.reminder;
	auto& w = want.reminder;
	bool changed = false;
	auto text = [&](std::string_view name, const std::string& value) {
		if (value.empty()) {
			t.remove(name);
		} else {
			t.set(name, ical_escape(value));
		}
		changed = true;
	};

	if (h.title != w.title) {
		text("SUMMARY", w.title);
	}
	if (h.notes != clean_notes(w.notes)) {
		text("DESCRIPTION", clean_notes(w.notes));
	}
	if (h.done != w.done || (w.done && h.completed != w.completed)) {
		if (w.done) {
			t.set("STATUS", "COMPLETED");
			t.set("PERCENT-COMPLETE", "100");
			t.set("COMPLETED",
				  w.completed ? utc_for(*w.completed, local) : ical_utc(now));
		} else {
			t.set("STATUS", "NEEDS-ACTION");
			t.remove("PERCENT-COMPLETE");
			t.remove("COMPLETED");
		}
		changed = true;
	}
	if (h.created != w.created) {
		if (w.created) {
			t.set("CREATED", utc_for(*w.created, local));
		} else {
			t.remove("CREATED");
		}
		changed = true;
	}
	if (h.due_date != w.due_date || h.due_time != w.due_time) {
		if (!w.due_date) {
			t.remove("DUE");
		} else if (w.due_time) {
			t.set("DUE", ical_local(*w.due_date, *w.due_time));
		} else {
			t.set("DUE", ical_date(*w.due_date), {{"VALUE", "DATE"}});
		}
		// A DTSTART after the new due date makes the task invalid (RFC 5545).
		if (auto* s = t.prop("DTSTART"); s && w.due_date) {
			auto sw = ical_when(*s, local);
			if (sw && sw->date > *w.due_date) {
				t.remove("DTSTART");
			}
		}
		changed = true;
	}
	if (h.priority != w.priority) {
		if (w.priority == Priority::None) {
			t.remove("PRIORITY");
		} else {
			t.set("PRIORITY", w.priority == Priority::High	   ? "1"
							  : w.priority == Priority::Medium ? "5"
															   : "9");
		}
		changed = true;
	}
	if (h.repeat != w.repeat) {
		auto rule = w.repeat ? rrule_from_repeat(*w.repeat) : std::nullopt;
		// An unknown rule from this app can't be sent; the server's stays.
		if (rule) {
			t.set("RRULE", *rule);
		} else if (!w.repeat) {
			t.remove("RRULE");
		}
		changed = changed || rule || !w.repeat;
	}
	if (h.tags != w.tags) {
		std::string v;
		for (auto& tag : w.tags) {
			v += (v.empty() ? "" : ",") + ical_escape(tag);
		}
		if (v.empty()) {
			t.remove("CATEGORIES");
		} else {
			t.set("CATEGORIES", v);
		}
		changed = true;
	}
	if (h.url != w.url) {
		text("URL", w.url.value_or(""));
	}
	if (h.flagged != w.flagged) {
		if (w.flagged) {
			t.set(kFlagged, "TRUE");
		} else {
			t.remove(kFlagged);
		}
		changed = true;
	}
	if (have.section != want.section) {
		text(kSection, want.section.value_or(""));
	}
	if (have.sort_order != want.sort_order) {
		if (want.sort_order) {
			t.set(kSortOrder, std::to_string(*want.sort_order));
		} else {
			t.remove(kSortOrder);
		}
		changed = true;
	}
	if (have.parent_uid != want.parent_uid) {
		std::erase_if(t.props, [](const IcalProperty& p) {
			if (p.name != "RELATED-TO") {
				return false;
			}
			auto type = p.param("RELTYPE");
			return !type || lower(*type) == "parent";
		});
		if (want.parent_uid) {
			t.props.push_back(
				IcalProperty{"RELATED-TO", {}, ical_escape(*want.parent_uid)});
		}
		changed = true;
	}

	if (changed) {
		t.set("DTSTAMP", ical_utc(now));
		t.set("LAST-MODIFIED", ical_utc(now));
		auto* seq = t.prop("SEQUENCE");
		auto n = seq ? number(seq->value) : std::nullopt;
		t.set("SEQUENCE", std::to_string(n ? *n + 1 : 0));
	}
	return changed;
}

IcalComponent new_todo_calendar(const Todo& want, sys_seconds now,
								const time_zone* local) {
	IcalComponent cal{"VCALENDAR", {}, {}};
	cal.set("VERSION", "2.0");
	cal.set("PRODID", "-//stephenhouser//Reminders//EN");
	IcalComponent todo{"VTODO", {}, {}};
	todo.set("UID", ical_escape(want.uid));
	todo.set("DTSTAMP", ical_utc(now));
	write_todo(todo, want, now, local);
	todo.set("SEQUENCE", "0");
	if (!todo.prop("STATUS")) {
		todo.set("STATUS", "NEEDS-ACTION");
	}
	cal.children.push_back(std::move(todo));
	return cal;
}

}  // namespace rem
