#include "reminders/preferences.hpp"

#include <algorithm>
#include <cctype>

#include "reminders/model.hpp"

namespace rem {

namespace {

// Moves `item` to just before or after `target` in `order`.
template <class T>
bool place_next_to(std::vector<T>& order, const T& item, const T& target,
				   bool after) {
	if (item == target) {
		return false;
	}
	auto from = std::ranges::find(order, item);
	if (from == order.end() ||
		std::ranges::find(order, target) == order.end()) {
		return false;
	}
	auto before = order;
	order.erase(from);
	auto at = std::ranges::find(order, target);
	order.insert(after ? at + 1 : at, item);
	return order != before;
}

std::string trimmed(std::string_view s) {
	auto b = s.find_first_not_of(" \t");
	if (b == std::string_view::npos) {
		return {};
	}
	auto e = s.find_last_not_of(" \t");
	return std::string(s.substr(b, e - b + 1));
}

// "visible" (the default), "collapsible" (or "collapsable") or "hidden".
GroupDisplay load_display(const std::string& key) {
	std::string v;
	for (char c : load_setting(key)) {
		v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (v == "collapsible" || v == "collapsable") {
		return GroupDisplay::Collapsible;
	}
	if (v == "hidden" || v == "hide" || v == "none") {
		return GroupDisplay::Hidden;
	}
	return GroupDisplay::Visible;
}

}  // namespace

SmartListsLayout load_smart_lists_layout() {
	static const std::vector<std::string> known = {
		"today", "scheduled", "all", "all-reminders", "flagged", "completed"};
	SmartListsLayout layout;
	auto value = load_setting("smart-lists");
	if (!value.empty()) {
		layout.shown.clear();
		std::string word;
		for (char c : value + ",") {
			if (c == ',' || c == ' ' || c == '\t') {
				for (auto& ch : word) {
					ch = static_cast<char>(
						std::tolower(static_cast<unsigned char>(ch)));
				}
				bool ok =
					std::find(known.begin(), known.end(), word) != known.end();
				if (ok && std::find(layout.shown.begin(), layout.shown.end(),
									word) == layout.shown.end()) {
					layout.shown.push_back(word);
				}
				word.clear();
			} else {
				word += c;
			}
		}
	}
	layout.display = load_display("smart-lists-display");
	layout.collapsed = load_bool_setting("smart-lists-collapsed");
	return layout;
}

GroupLayout load_lists_layout(const std::string& source) {
	auto display = load_display("local-lists-display");
	if (display == GroupDisplay::Hidden) {
		display = GroupDisplay::Visible;
	}
	return GroupLayout{display, load_bool_setting("lists-collapsed." + source)};
}

GroupLayout load_tags_layout() {
	return GroupLayout{load_display("tags-display"),
					   load_bool_setting("tags-collapsed")};
}

std::string group_title(const SidebarGroup& group,
						const std::string& lists_title) {
	switch (group.kind) {
		case SidebarGroup::SmartLists:
			return "Smart Lists";
		case SidebarGroup::Lists:
			return lists_title;
		case SidebarGroup::Tags:
			return "Tags";
	}
	return "";
}

void save_group_collapsed(const SidebarGroup& group, bool collapsed) {
	auto key = group.kind == SidebarGroup::SmartLists
				 ? std::string("smart-lists-collapsed")
			 : group.kind == SidebarGroup::Tags
				 ? std::string("tags-collapsed")
				 : "lists-collapsed." + group.source;
	save_setting(key, collapsed ? "true" : "false");
}

void save_group_display(const SidebarGroup& group, GroupDisplay display) {
	const char* value = display == GroupDisplay::Collapsible ? "collapsible"
					  : display == GroupDisplay::Hidden		 ? "hidden"
															 : "visible";
	const char* key = group.kind == SidebarGroup::SmartLists
						? "smart-lists-display"
					: group.kind == SidebarGroup::Tags ? "tags-display"
													   : "local-lists-display";
	save_setting(key, value);
}

std::vector<SidebarGroup> load_sidebar_order(
	const std::vector<std::string>& sources) {
	// The words, as written; "smart-lists", "smart_lists" and "SmartLists"
	// all count, but a source's name is kept exactly.
	std::vector<std::string> words;
	std::string word;
	for (char c : load_setting("sidebar-order") + ",") {
		if (c == ',' || c == ' ' || c == '\t') {
			if (!word.empty()) {
				words.push_back(word);
			}
			word.clear();
		} else {
			word += c;
		}
	}
	auto keyword = [](std::string w) {
		std::string out;
		for (char c : w) {
			if (c != '-' && c != '_') {
				out += static_cast<char>(
					std::tolower(static_cast<unsigned char>(c)));
			}
		}
		return out;
	};
	std::vector<std::string> named;	 // sources with a place of their own
	for (auto& w : words) {
		if (keyword(w).starts_with("lists:")) {
			named.push_back(w.substr(w.find(':') + 1));
		}
	}

	std::vector<SidebarGroup> order;
	auto put = [&](const SidebarGroup& g) {
		if (std::ranges::find(order, g) == order.end()) {
			order.push_back(g);
		}
	};
	for (auto& w : words) {
		auto k = keyword(w);
		if (k == "smartlists" || k == "smart") {
			put(SidebarGroup::smart_lists());
		} else if (k == "tags") {
			put(SidebarGroup::tags());
		} else if (k == "locallists" || k == "lists") {
			for (auto& s : sources) {
				if (std::ranges::find(named, s) == named.end()) {
					put(SidebarGroup::lists(s));
				}
			}
		} else if (k.starts_with("lists:")) {
			auto name = w.substr(w.find(':') + 1);
			if (std::ranges::find(sources, name) != sources.end()) {
				put(SidebarGroup::lists(name));
			}
		}
	}
	put(SidebarGroup::smart_lists());
	for (auto& s : sources) {
		put(SidebarGroup::lists(s));
	}
	put(SidebarGroup::tags());
	return order;
}

void save_sidebar_order(const std::vector<SidebarGroup>& order) {
	auto lists_groups = std::ranges::count_if(
		order, [](auto& g) { return g.kind == SidebarGroup::Lists; });
	std::string value;
	for (auto& g : order) {
		std::string w = g.kind == SidebarGroup::SmartLists ? "smart-lists"
					  : g.kind == SidebarGroup::Tags	   ? "tags"
					  : lists_groups == 1 ? "local-lists"
										  : "lists:" + g.source;
		value += (value.empty() ? "" : ", ") + w;
	}
	save_setting("sidebar-order", value);
}

bool move_sidebar_group_next_to(std::vector<SidebarGroup>& order,
								const SidebarGroup& group,
								const SidebarGroup& target, bool after) {
	return place_next_to(order, group, target, after);
}

bool move_sidebar_group(std::vector<SidebarGroup>& order,
						const SidebarGroup& group, int delta,
						const std::vector<SidebarGroup>& showing) {
	auto at = std::ranges::find(order, group);
	if (at == order.end() || delta == 0) {
		return false;
	}
	auto n = static_cast<long>(order.size());
	for (long i = (at - order.begin()) + (delta < 0 ? -1 : 1); i >= 0 && i < n;
		 i += delta < 0 ? -1 : 1) {
		if (std::ranges::find(showing, order[static_cast<std::size_t>(i)]) ==
			showing.end()) {
			continue;
		}
		std::swap(*at, order[static_cast<std::size_t>(i)]);
		return true;
	}
	return false;
}

std::vector<std::string> load_names_setting(const std::string& key) {
	std::vector<std::string> out;
	std::string value = load_setting(key), name;
	bool quoted = false, any = false;
	auto finish = [&] {
		auto t = trimmed(name);
		if (!t.empty() && std::ranges::find(out, t) == out.end()) {
			out.push_back(t);
		}
		name.clear();
		any = false;
	};
	for (char c : value) {
		if (c == '"') {
			quoted = !quoted;
			any = true;
		} else if (c == ',' && !quoted) {
			finish();
		} else {
			name += c;
		}
	}
	if (any || !name.empty()) {
		finish();
	}
	return out;
}

void save_names_setting(const std::string& key,
						const std::vector<std::string>& names) {
	std::string value;
	for (auto& n : names) {
		if (!value.empty()) {
			value += ", ";
		}
		bool quote =
			n.find_first_of(",\"") != std::string::npos || n != trimmed(n);
		if (quote) {
			std::string escaped;
			for (char c : n) {
				if (c != '"') {
					escaped += c;  // quotes can't be stored; list names rarely
								   // have them
				}
			}
			value += '"' + escaped + '"';
		} else {
			value += n;
		}
	}
	save_setting(key, value);
}

bool list_entry_matches(std::string_view entry, std::string_view key) {
	if (entry == key) {
		return true;
	}
	auto slash = key.find('/');
	return entry.find('/') == std::string_view::npos &&
		   slash != std::string_view::npos && key.substr(slash + 1) == entry;
}

bool HiddenEntries::list_hidden(std::string_view key) const {
	return std::ranges::any_of(
		lists, [&](auto& e) { return list_entry_matches(e, key); });
}
bool HiddenEntries::tag_hidden(std::string_view tag) const {
	return std::ranges::find(tags, tag) != tags.end();
}

HiddenEntries load_hidden() {
	return HiddenEntries{load_names_setting("lists-hidden"),
						 load_names_setting("tags-hidden"),
						 load_bool_setting("show-hidden")};
}

namespace {

void set_in_names(const std::string& key, const std::string& name,
				  bool present) {
	auto names = load_names_setting(key);
	auto at = std::ranges::find(names, name);
	if (present == (at != names.end())) {
		return;
	}
	if (present) {
		names.push_back(name);
	} else {
		names.erase(at);
	}
	save_names_setting(key, names);
}

}  // namespace

void set_list_hidden(const std::string& key, bool hidden) {
	auto names = load_names_setting("lists-hidden");
	if (hidden && std::ranges::find(names, key) != names.end()) {
		return;	 // already
	}
	auto before = names;
	std::erase_if(names, [&](auto& e) {
		return list_entry_matches(e, key);
	});	 // a bare "Work" too
	if (hidden) {
		names.push_back(key);
	}
	if (names != before) {
		save_names_setting("lists-hidden", names);
	}
}
void set_tag_hidden(const std::string& tag, bool hidden) {
	set_in_names("tags-hidden", tag, hidden);
}

void set_smart_list_hidden(const std::string& name, bool hidden) {
	auto shown = load_smart_lists_layout().shown;
	auto at = std::ranges::find(shown, name);
	if (hidden == (at == shown.end())) {
		return;
	}
	if (hidden) {
		shown.erase(at);
	} else {
		shown.push_back(name);
	}
	save_smart_lists(shown);
}

void save_smart_lists(const std::vector<std::string>& shown) {
	std::string value;
	for (auto& s : shown) {
		value += (value.empty() ? "" : ", ") + s;
	}
	save_setting("smart-lists", value.empty() ? "none" : value);
}

std::vector<std::string> order_tags(std::vector<std::string> tags) {
	std::ranges::sort(tags);
	std::vector<std::string> out;
	for (auto& t : load_names_setting("tags-order")) {
		if (std::ranges::find(tags, t) != tags.end()) {
			out.push_back(t);
		}
	}
	for (auto& t : tags) {
		if (std::ranges::find(out, t) == out.end()) {
			out.push_back(t);
		}
	}
	return out;
}

std::vector<std::string> order_lists(const std::vector<std::string>& names) {
	std::vector<std::string> out;
	for (auto& entry : load_names_setting("lists-order")) {
		for (auto& n : names) {
			if (list_entry_matches(entry, n) &&
				std::ranges::find(out, n) == out.end()) {
				out.push_back(n);
				break;
			}
		}
	}
	for (auto& n : names) {
		if (std::ranges::find(out, n) == out.end()) {
			out.push_back(n);
		}
	}
	return out;
}

bool move_in_order(std::vector<std::string>& order, const std::string& name,
				   int delta, const std::vector<std::string>& showing) {
	auto at = std::ranges::find(order, name);
	if (at == order.end() || delta == 0) {
		return false;
	}
	auto n = static_cast<long>(order.size());
	int step = delta < 0 ? -1 : 1;
	for (long i = (at - order.begin()) + step; i >= 0 && i < n; i += step) {
		if (std::ranges::find(showing, order[static_cast<std::size_t>(i)]) ==
			showing.end()) {
			continue;
		}
		std::swap(*at, order[static_cast<std::size_t>(i)]);
		return true;
	}
	return false;
}

bool move_next_to(std::vector<std::string>& order, const std::string& name,
				  const std::string& target, bool after) {
	return place_next_to(order, name, target, after);
}

void save_show_hidden(bool show) {
	save_setting("show-hidden", show ? "true" : "false");
}

TagStyle load_tag_style(const std::string& tag) {
	TagStyle style;
	auto color = load_setting("tag-color." + tag),
		 icon = load_setting("tag-icon." + tag);
	if (std::ranges::find(kColors, color) != std::end(kColors)) {
		style.color = color;
	}
	if (std::ranges::find(kIcons, icon) != std::end(kIcons)) {
		style.icon = icon;
	}
	return style;
}

void save_tag_style(const std::string& tag, const TagStyle& style) {
	save_setting("tag-color." + tag, style.color);
	save_setting("tag-icon." + tag, style.icon);
}

std::size_t load_note_lines() {
	auto value = trimmed(load_setting("note-lines"));
	std::size_t n = 0;
	for (char c : value) {
		if (!std::isdigit(static_cast<unsigned char>(c))) {
			return 0;
		}
		n = n * 10 + static_cast<std::size_t>(c - '0');
		if (n > 1000) {
			return 0;  // as good as all
		}
	}
	return n;
}

std::string first_lines(const std::string& text, std::size_t lines) {
	if (lines == 0) {
		return text;
	}
	std::size_t at = 0;
	for (std::size_t i = 0; i < lines; ++i) {
		at = text.find('\n', at);
		if (at == std::string::npos) {
			return text;
		}
		++at;
	}
	auto out = text.substr(0, at - 1);
	return out + "…";
}

}  // namespace rem
