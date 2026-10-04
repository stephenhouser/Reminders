#include "reminders/view.hpp"

#include <format>
#include <utility>

namespace rem {

namespace {

constexpr std::pair<View::Kind, std::string_view> kNames[] = {
	{View::Today, "today"},
	{View::Scheduled, "scheduled"},
	{View::All, "all"},
	{View::Flagged, "flagged"},
	{View::Completed, "completed"},
	{View::AllReminders, "all-reminders"},
	{View::List, "list"},
	{View::Tag, "tag"}};

}  // namespace

std::string_view smart_view_name(View::Kind kind) {
	if (kind == View::List || kind == View::Tag || kind == View::Search) {
		return {};
	}
	for (auto& [k, n] : kNames) {
		if (k == kind) {
			return n;
		}
	}
	return {};
}

std::string view_to_string(const View& v) {
	for (auto& [k, n] : kNames) {
		if (k == v.kind) {
			return v.name.empty() ? std::string(n)
								  : std::format("{}:{}", n, v.name);
		}
	}
	return "today";
}

View view_from_string(std::string_view s) {
	auto colon = s.find(':');
	auto kind = s.substr(0, colon);
	for (auto& [k, n] : kNames) {
		if (n == kind) {
			return View{k, colon == std::string_view::npos
							   ? ""
							   : std::string(s.substr(colon + 1))};
		}
	}
	return View{View::Today, ""};
}

}  // namespace rem
