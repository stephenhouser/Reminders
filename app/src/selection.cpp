#include "reminders/selection.hpp"

#include <algorithm>

namespace rem {

void Selection::select_only(const std::string& id) {
	ids_ = {id};
	anchor_ = id;
}

void Selection::toggle(const std::string& id) {
	if (!ids_.erase(id)) {
		ids_.insert(id);
	}
	anchor_ = id;
}

bool Selection::select_range(const std::vector<std::string>& shown,
							 const std::string& to, bool add,
							 const std::optional<std::string>& fallback) {
	auto from = anchor_.value_or(fallback.value_or(to));
	auto a = std::ranges::find(shown, from), b = std::ranges::find(shown, to);
	if (b == shown.end()) {
		return false;
	}
	if (a == shown.end()) {
		a = b;
		from = to;
	}
	if (a > b) {
		std::swap(a, b);
	}
	if (!add) {
		ids_.clear();
	}
	ids_.insert(a, b + 1);
	anchor_ = from;
	return true;
}

void Selection::select_all(const std::vector<std::string>& shown) {
	ids_ = {shown.begin(), shown.end()};
	if (!anchor_ && !shown.empty()) {
		anchor_ = shown.front();
	}
}

bool Selection::prune(const std::vector<std::string>& shown) {
	return std::erase_if(ids_, [&](const std::string& id) {
			   return std::ranges::find(shown, id) == shown.end();
		   }) > 0;
}

std::vector<std::string> Selection::in_order(
	const std::vector<std::string>& shown) const {
	std::vector<std::string> out;
	for (auto& id : shown) {
		if (ids_.contains(id)) {
			out.push_back(id);
		}
	}
	return out;
}

std::vector<std::string> Selection::targets(
	const std::string& id, const std::vector<std::string>& shown) const {
	if (!ids_.contains(id)) {
		return {id};
	}
	return in_order(shown);
}

}  // namespace rem
