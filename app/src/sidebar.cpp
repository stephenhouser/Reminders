#include "reminders/sidebar.hpp"

#include <algorithm>

#include "reminders/sources.hpp"

namespace rem {

namespace {

constexpr View::Kind kSmartKinds[] = {View::Today,	 View::Scheduled,
									  View::All,	 View::AllReminders,
									  View::Flagged, View::Completed};

bool is_smart(View::Kind k) { return !smart_view_name(k).empty(); }

}  // namespace

void Sidebar::reload() {
	std::vector<std::string> sources;
	for (auto& s : library_.sources()) {
		sources.push_back(s.config.name);
	}
	order_ = load_sidebar_order(sources);
	smart_ = load_smart_lists_layout();
	lists_layouts_.clear();
	tags_ = load_tags_layout();
	hidden_ = load_hidden();
}

GroupLayout* Sidebar::layout_of(const SidebarGroup& group) {
	if (group.kind == SidebarGroup::Lists) {
		auto at = lists_layouts_.find(group.source);
		if (at == lists_layouts_.end()) {
			at = lists_layouts_
					 .emplace(group.source, load_lists_layout(group.source))
					 .first;
		}
		return &at->second;
	}
	if (group.kind == SidebarGroup::Tags) {
		return &tags_;
	}
	return nullptr;
}

std::vector<std::string> Sidebar::list_keys() {
	std::vector<std::string> keys;
	for (auto* l : library_.lists()) {
		keys.push_back(library_.key_of(*l));
	}
	return keys;
}

std::vector<SidebarGroup> Sidebar::groups() {
	std::vector<SidebarGroup> out;
	for (auto& g : order_) {
		if (g.kind == SidebarGroup::SmartLists && smart_views().empty()) {
			continue;
		}
		if (g.kind == SidebarGroup::Tags &&
			(tags_.hidden() || tags().empty())) {
			continue;
		}
		out.push_back(g);
	}
	return out;
}

std::string Sidebar::title(const SidebarGroup& group) {
	if (group.kind == SidebarGroup::Lists && library_.sources().size() > 1) {
		for (auto& s : library_.sources()) {
			if (s.config.name == group.source) {
				return group_title(group, source_title(s.config));
			}
		}
	}
	return group_title(group);
}

bool Sidebar::foldable(const SidebarGroup& group) {
	auto* l = layout_of(group);
	return l ? l->foldable() : smart_.foldable();
}

bool Sidebar::folded(const SidebarGroup& group) {
	auto* l = layout_of(group);
	return l ? l->folded() : smart_.folded();
}

void Sidebar::toggle_fold(const SidebarGroup& group) {
	auto* l = layout_of(group);
	bool& collapsed = l ? l->collapsed : smart_.collapsed;
	collapsed = !collapsed;
	try {
		save_group_collapsed(group, collapsed);
	} catch (const std::exception&) {
		// Folding still works; it just won't be remembered.
	}
}

void Sidebar::set_foldable(const SidebarGroup& group, bool foldable) {
	auto* l = layout_of(group);
	auto& display = l ? l->display : smart_.display;
	bool& collapsed = l ? l->collapsed : smart_.collapsed;
	display = foldable ? GroupDisplay::Collapsible : GroupDisplay::Visible;
	collapsed = false;	// a group made collapsible starts unfolded
	save_group_display(group, display);
	save_group_collapsed(group, false);
}

std::vector<View> Sidebar::smart_views() {
	std::vector<View> out;
	if (smart_.display == GroupDisplay::Hidden) {
		return out;
	}
	for (auto& name : smart_.shown) {
		if (auto v = view_from_string(name);
			is_smart(v.kind) && smart_view_name(v.kind) == name) {
			out.push_back(v);
		}
	}
	if (hidden_.show) {	 // the hidden ones after them
		for (auto k : kSmartKinds) {
			if (std::ranges::find(out, View{k, ""}) == out.end()) {
				out.push_back(View{k, ""});
			}
		}
	}
	return out;
}

std::vector<ListFile*> Sidebar::lists(const std::string& source) {
	std::vector<std::string> keys;
	for (auto* l : library_.lists(source)) {
		keys.push_back(library_.key_of(*l));
	}
	std::vector<ListFile*> out;
	for (auto& key : order_lists(keys)) {
		if (hidden_.show || !hidden_.list_hidden(key)) {
			if (auto* l = library_.list(key)) {
				out.push_back(l);
			}
		}
	}
	return out;
}

std::vector<std::string> Sidebar::tags() {
	std::vector<std::string> out;
	for (auto& t : order_tags(library_.tags())) {
		if (hidden_.show || !hidden_.tag_hidden(t)) {
			out.push_back(t);
		}
	}
	return out;
}

std::vector<View> Sidebar::entries(const SidebarGroup& group) {
	switch (group.kind) {
		case SidebarGroup::SmartLists:
			return smart_views();
		case SidebarGroup::Lists:
			{
				std::vector<View> out;
				for (auto* l : lists(group.source)) {
					out.push_back(View{View::List, library_.key_of(*l)});
				}
				return out;
			}
		case SidebarGroup::Tags:
			{
				std::vector<View> out;
				for (auto& t : tags()) {
					out.push_back(View{View::Tag, t});
				}
				return out;
			}
	}
	return {};
}

std::vector<View> Sidebar::all(bool include_folded) {
	std::vector<View> out;
	for (auto& g : groups()) {
		if (folded(g) && !include_folded) {
			continue;
		}
		for (auto& v : entries(g)) {
			out.push_back(v);
		}
	}
	return out;
}

View Sidebar::home() {
	auto smart = smart_views();
	if (std::ranges::find(smart, View{View::Today, ""}) != smart.end()) {
		return {View::Today, ""};
	}
	auto every = all(true);
	return every.empty() ? View{View::Today, ""} : every.front();
}

std::optional<int> Sidebar::count(const View& v, Date today) {
	auto size = [](const std::vector<Ref>& refs) {
		return static_cast<int>(refs.size());
	};
	switch (v.kind) {
		case View::Today:
			return size(library_.today(today));
		case View::Scheduled:
			return size(library_.scheduled());
		case View::All:
			return size(library_.all());
		case View::AllReminders:
			return size(library_.everything());
		case View::Flagged:
			return size(library_.flagged());
		case View::Completed:
			return size(library_.completed());
		case View::List:
			if (auto* l = library_.list(v.name)) {
				int open = 0;
				l->doc.walk([&](Reminder& r, Reminder*) { open += !r.done; });
				return open;
			}
			return std::nullopt;
		default:
			return std::nullopt;
	}
}

bool Sidebar::hidden(const View& v) {
	if (is_smart(v.kind)) {
		return std::ranges::find(smart_.shown, view_to_string(v)) ==
			   smart_.shown.end();
	}
	if (v.kind == View::List) {
		return hidden_.list_hidden(v.name);
	}
	if (v.kind == View::Tag) {
		return hidden_.tag_hidden(v.name);
	}
	return false;
}

void Sidebar::set_hidden(const View& v, bool hide) {
	if (is_smart(v.kind)) {
		set_smart_list_hidden(view_to_string(v), hide);
	} else if (v.kind == View::List) {
		set_list_hidden(v.name, hide);
	} else if (v.kind == View::Tag) {
		set_tag_hidden(v.name, hide);
	}
	smart_ = load_smart_lists_layout();
	hidden_ = load_hidden();
}

void Sidebar::set_show_hidden(bool show) {
	save_show_hidden(show);
	hidden_.show = show;
}

bool Sidebar::gone(const View& v) {
	if (is_smart(v.kind)) {
		auto smart = smart_views();
		return std::ranges::find(smart, v) == smart.end();
	}
	return (v.kind == View::Tag && tags_.hidden()) ||
		   (!hidden_.show && hidden(v));
}

bool Sidebar::same_group(const View& a, const View& b) {
	if (a == b) {
		return false;
	}
	if (is_smart(a.kind) || is_smart(b.kind)) {
		return is_smart(a.kind) && is_smart(b.kind);
	}
	if (a.kind != b.kind) {
		return false;
	}
	if (a.kind == View::Tag) {
		return true;
	}
	if (a.kind != View::List) {
		return false;
	}
	auto *la = library_.list(a.name), *lb = library_.list(b.name);
	return la && lb && library_.source_of(*la) == library_.source_of(*lb);
}

std::optional<Sidebar::EntryOrder> Sidebar::entry_order(const View& v) {
	if (is_smart(v.kind)) {
		auto order = smart_.shown;	// a hidden smart list has no place to move
		return EntryOrder{order, order, view_to_string(v)};
	}
	if (v.kind == View::Tag) {
		return EntryOrder{order_tags(library_.tags()), tags(), v.name};
	}
	if (v.kind == View::List) {
		auto* l = library_.list(v.name);
		if (!l) {
			return std::nullopt;
		}
		std::vector<std::string> showing;  // this list's group
		for (auto* x : lists(library_.source_of(*l)->config.name)) {
			showing.push_back(library_.key_of(*x));
		}
		return EntryOrder{order_lists(list_keys()), std::move(showing),
						  v.name};	// every source's
	}
	return std::nullopt;
}

void Sidebar::save_order(const View& v, const std::vector<std::string>& order) {
	if (is_smart(v.kind)) {
		save_smart_lists(order);
		smart_ = load_smart_lists_layout();
	} else if (v.kind == View::Tag) {
		save_names_setting("tags-order", order);
	} else if (v.kind == View::List) {
		save_names_setting("lists-order", order);
	}
}

bool Sidebar::can_move(const View& v, int delta) {
	auto e = entry_order(v);
	return e && move_in_order(e->order, e->name, delta, e->showing);
}

bool Sidebar::move(const View& v, int delta) {
	auto e = entry_order(v);
	if (!e || !move_in_order(e->order, e->name, delta, e->showing)) {
		return false;
	}
	save_order(v, e->order);
	return true;
}

bool Sidebar::move_next_to(const View& v, const View& target, bool after) {
	if (!same_group(v, target)) {
		return false;
	}
	auto e = entry_order(v);
	auto t = entry_order(target);
	if (!e || !t || !rem::move_next_to(e->order, e->name, t->name, after)) {
		return false;
	}
	save_order(v, e->order);
	return true;
}

bool Sidebar::can_move_group(const SidebarGroup& group, int delta) {
	auto order = order_;
	return move_sidebar_group(order, group, delta, groups());
}

bool Sidebar::move_group(const SidebarGroup& group, int delta) {
	if (!move_sidebar_group(order_, group, delta, groups())) {
		return false;
	}
	save_sidebar_order(order_);
	return true;
}

bool Sidebar::move_group_next_to(const SidebarGroup& group,
								 const SidebarGroup& target, bool after) {
	if (!move_sidebar_group_next_to(order_, group, target, after)) {
		return false;
	}
	save_sidebar_order(order_);
	return true;
}

}  // namespace rem
