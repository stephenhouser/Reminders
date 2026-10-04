#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#include "dialogs.hpp"
#include "reminders/clipboard.hpp"
#include "reminders/exporter.hpp"
#include "reminders/format.hpp"
#include "reminders/importer.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "reminders/syncthing.hpp"
#include "support.hpp"
#include "window_internal.hpp"

namespace ui {

void Window::rebuild_sidebar() {
	updating_sidebar_ = true;
	auto* list = GTK_LIST_BOX(sidebar_list_);
	gtk_list_box_remove_all(list);
	auto day = today();
	// Optional "Ctrl+1"-style labels after each name (show-key-numbers),
	// numbered in display order over the entries that are showing.
	std::size_t index = 0;
	auto shortcut = [this](std::size_t i) {
		return show_key_numbers_ ? jump_shortcut(i) : std::string();
	};

	// Entries are dragged to another place in their group.
	auto make_reorderable = [this](GtkWidget* row, const View& v) {
		make_entry_draggable(row, v);
		make_entry_drop_target(
			row,
			[this, v](const View& dragged) {
				if (!same_sidebar_group(dragged, v)) {
					return false;
				}
				auto t = entry_order(v);  // a hidden smart list has no place
				return t &&
					   std::ranges::find(t->order, t->name) != t->order.end();
			},
			[this, v](View dragged, bool after) {
				drop_entry(dragged, v, after);
			});
	};

	// The group at the top has no heading unless it can be folded; the
	// groups below it have one.
	bool first = true;
	for (auto g : showing_groups()) {
		// Every row of the group takes a dragged group; the heading drags it.
		auto add = [&](GtkWidget* row) {
			set_row_group(row, g);
			make_group_drop_target(
				row, g, [this, g](rem::SidebarGroup dragged, bool after) {
					drop_group(dragged, g, after);
				});
			gtk_list_box_append(list, row);
		};
		if (group_foldable(g)) {
			auto* heading = fold_heading(group_title(g), group_folded(g), g);
			make_group_draggable(heading, g);
			add(heading);
		} else if (!first) {
			auto* heading = sidebar_heading(group_title(g));
			make_group_draggable(heading, g);
			add(heading);
		}
		first = false;
		if (group_folded(g)) {
			continue;
		}
		switch (g.kind) {
			case rem::SidebarGroup::SmartLists:
				for (auto& v : smart_views()) {
					auto* s = smart_info(v.kind);
					std::size_t count = 0;
					switch (v.kind) {
						case View::Today:
							count = store_->today(day).size();
							break;
						case View::Scheduled:
							count = store_->scheduled().size();
							break;
						case View::All:
							count = store_->all().size();
							break;
						case View::AllReminders:
							count = store_->everything().size();
							break;
						case View::Flagged:
							count = store_->flagged().size();
							break;
						case View::Completed:
							count = store_->completed().size();
							break;
						default:
							break;
					}
					auto* row =
						sidebar_row(s->icon, s->color, s->title,
									static_cast<int>(count), shortcut(index++));
					set_row_view(row, v);
					if (entry_hidden(v)) {
						gtk_widget_add_css_class(row, "hidden-entry");
					}
					make_reorderable(row, v);
					add(row);
				}
				break;
			case rem::SidebarGroup::Lists:	// one source's lists
				for (auto* l : sidebar_lists(g.source)) {
					auto key = store_->key_of(*l);
					auto* row =
						sidebar_row(list_icon_name(l->icon()), l->color(),
									l->name, open_count(*l), shortcut(index++));
					set_row_view(row, View{View::List, key});
					if (hidden_.list_hidden(key)) {
						gtk_widget_add_css_class(row, "hidden-entry");
					}
					make_reorderable(row, View{View::List, key});
					make_drop_target(
						row, DropStyle::Into, "",
						[this, key](Ids dropped, rem::Document::Place) {
							move_to_list(dropped, key);
						});
					make_file_drop_target(
						row, true,
						[this, key](std::vector<std::filesystem::path> files) {
							import_files(std::move(files), key);
						});
					make_text_drop_target(
						row, DropStyle::Into,
						[this, key](std::optional<std::string> text,
									rem::Document::Place) {
							if (!text) {
								toast("Couldn't read the dropped text");
							} else {
								add_text(*text, "Drop", key, std::nullopt,
										 rem::Document::Place::After);
							}
						});
					add(row);
				}
				break;
			case rem::SidebarGroup::Tags:
				for (auto& t : sidebar_tags()) {
					auto style = rem::load_tag_style(t);
					auto* row =
						sidebar_row(list_icon_name(style.icon), style.color,
									"#" + t, std::nullopt, shortcut(index++));
					set_row_view(row, View{View::Tag, t});
					if (hidden_.tag_hidden(t)) {
						gtk_widget_add_css_class(row, "hidden-entry");
					}
					make_reorderable(row, View{View::Tag, t});
					add(row);
				}
				break;
		}
	}

	gtk_list_box_unselect_all(list);
	for (int i = 0;; ++i) {
		auto* row = gtk_list_box_get_row_at_index(list, i);
		if (!row) {
			break;
		}
		if (auto* v = row_view(row); v && *v == view_) {
			gtk_list_box_select_row(list, row);
			break;
		}
	}
	updating_sidebar_ = false;
}

// The smart lists the settings show, in their order.
std::vector<View> Window::smart_views() {
	std::vector<View> out;
	if (smart_.display == rem::GroupDisplay::Hidden) {
		return out;
	}
	for (auto& name : smart_.shown) {
		auto v = view_from_string(name);
		if (smart_info(v.kind)) {
			out.push_back(v);
		}
	}
	if (hidden_.show) {	 // the hidden ones after them
		for (auto& s : kSmart) {
			if (std::ranges::find(out, View{s.kind, ""}) == out.end()) {
				out.push_back(View{s.kind, ""});
			}
		}
	}
	return out;
}

std::vector<rem::ListFile*> Window::sidebar_lists(const std::string& source) {
	std::vector<std::string> keys;
	for (auto* l : store_->lists(source)) {
		keys.push_back(store_->key_of(*l));
	}
	std::vector<rem::ListFile*> out;
	for (auto& key : rem::order_lists(keys)) {
		if (hidden_.show || !hidden_.list_hidden(key)) {
			if (auto* l = store_->list(key)) {
				out.push_back(l);
			}
		}
	}
	return out;
}

std::vector<std::string> Window::list_keys() {
	std::vector<std::string> keys;
	for (auto* l : store_->lists()) {
		keys.push_back(store_->key_of(*l));
	}
	return keys;
}

std::string Window::list_label(const rem::ListFile& list) {
	return store_->label(list);
}

rem::ListFile* Window::list_by_label(const std::string& label) {
	if (auto* l = store_->list(label)) {
		return l;
	}
	for (auto* l : store_->lists()) {
		if (store_->label(*l) == label) {
			return l;
		}
	}
	return nullptr;
}

std::vector<std::string> Window::source_names() {
	std::vector<std::string> out;
	if (store_) {
		for (auto& s : store_->sources()) {
			out.push_back(s.config.name);
		}
	}
	return out;
}

std::string Window::group_title(const rem::SidebarGroup& group) {
	if (group.kind != rem::SidebarGroup::Lists || !store_ ||
		store_->sources().size() <= 1) {
		return rem::group_title(group);
	}
	for (auto& s : store_->sources()) {
		if (s.config.name == group.source) {
			return rem::group_title(group, rem::source_title(s.config));
		}
	}
	return rem::group_title(group);
}

std::vector<std::string> Window::sidebar_tags() {
	std::vector<std::string> out;
	for (auto& t : rem::order_tags(store_->tags())) {
		if (hidden_.show || !hidden_.tag_hidden(t)) {
			out.push_back(t);
		}
	}
	return out;
}

bool Window::entry_hidden(const View& v) {
	if (smart_info(v.kind)) {
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

// Hides or unhides a sidebar entry (saved in settings.ini). Leaving the view
// that was just hidden goes to Today or the first entry showing.
void Window::set_entry_hidden(const View& v, bool hidden) {
	try {
		if (smart_info(v.kind)) {
			rem::set_smart_list_hidden(view_to_string(v), hidden);
		} else if (v.kind == View::List) {
			rem::set_list_hidden(v.name, hidden);
		} else if (v.kind == View::Tag) {
			rem::set_tag_hidden(v.name, hidden);
		}
	} catch (const std::exception& e) {
		toast(std::format("Couldn't save the setting: {}", e.what()));
	}
	smart_ = rem::load_smart_lists_layout();
	hidden_ = rem::load_hidden();
	if (hidden && !hidden_.show && view_ == v) {
		select(home_view());
	}
	rebuild_sidebar();
}

// Tag Info…: the tag's colour and icon, kept in settings.ini.
void Window::edit_tag(const std::string& tag) {
	auto style = rem::load_tag_style(tag);
	show_tag_dialog(
		window_, tag, ListEdit{"#" + tag, style.color, style.icon},
		[this, tag](ListEdit e) {
			try {
				rem::save_tag_style(tag, {e.color, e.icon});
			} catch (const std::exception& err) {
				toast(std::format("Couldn't save the setting: {}", err.what()));
			}
			rebuild_sidebar();
		});
}

// Sidebar entries in display order. `include_folded` adds the entries of
// collapsed groups (Go To finds them; numbers skip them).
std::vector<View> Window::sidebar_views(bool include_folded) {
	std::vector<View> out;
	for (auto g : showing_groups()) {
		if (group_folded(g) && !include_folded) {
			continue;
		}
		switch (g.kind) {
			case rem::SidebarGroup::SmartLists:
				for (auto& v : smart_views()) {
					out.push_back(v);
				}
				break;
			case rem::SidebarGroup::Lists:
				for (auto* l : sidebar_lists(g.source)) {
					out.push_back(View{View::List, store_->key_of(*l)});
				}
				break;
			case rem::SidebarGroup::Tags:
				for (auto& t : sidebar_tags()) {
					out.push_back(View{View::Tag, t});
				}
				break;
		}
	}
	return out;
}

std::vector<rem::SidebarGroup> Window::showing_groups() {
	std::vector<rem::SidebarGroup> out;
	for (auto g : order_) {
		if (g.kind == rem::SidebarGroup::SmartLists && smart_views().empty()) {
			continue;
		}
		if (g.kind == rem::SidebarGroup::Tags &&
			(tags_.hidden() || sidebar_tags().empty())) {
			continue;
		}
		out.push_back(g);
	}
	return out;
}

rem::GroupLayout* Window::layout_of(const rem::SidebarGroup& group) {
	if (group.kind == rem::SidebarGroup::Lists) {
		auto at = lists_layouts_.find(group.source);
		if (at == lists_layouts_.end()) {
			at =
				lists_layouts_
					.emplace(group.source, rem::load_lists_layout(group.source))
					.first;
		}
		return &at->second;
	}
	if (group.kind == rem::SidebarGroup::Tags) {
		return &tags_;
	}
	return nullptr;
}

bool Window::group_foldable(const rem::SidebarGroup& group) {
	auto* l = layout_of(group);
	return l ? l->foldable() : smart_.foldable();
}

bool Window::group_folded(const rem::SidebarGroup& group) {
	auto* l = layout_of(group);
	return l ? l->folded() : smart_.folded();
}

void Window::toggle_fold(const rem::SidebarGroup& group) {
	auto* l = layout_of(group);
	bool& collapsed = l ? l->collapsed : smart_.collapsed;
	collapsed = !collapsed;
	try {
		rem::save_group_collapsed(group, collapsed);
	} catch (const std::exception&) {
		// Folding still works; it just won't be remembered.
	}
	rebuild_sidebar();
}

// Moves a group past its neighbour and saves the order. Focus stays on the
// row that had it (or the group's first row).
void Window::move_group(const rem::SidebarGroup& group, int delta) {
	if (!store_ ||
		!rem::move_sidebar_group(order_, group, delta, showing_groups())) {
		return;
	}
	try {
		rem::save_sidebar_order(order_);
	} catch (const std::exception& e) {
		toast(std::format("Couldn't save the sidebar order: {}", e.what()));
	}
	std::optional<View> focused;
	for (auto* w = gtk_root_get_focus(GTK_ROOT(window_)); w;
		 w = gtk_widget_get_parent(w)) {
		if (GTK_IS_LIST_BOX_ROW(w)) {
			if (auto* v = row_view(GTK_LIST_BOX_ROW(w))) {
				focused = *v;
			}
			break;
		}
	}
	rebuild_sidebar();
	for (int i = 0;; ++i) {
		auto* row =
			gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_list_), i);
		if (!row) {
			break;
		}
		auto* v = row_view(row);
		if (focused ? v && *v == *focused : row_group(row) == group) {
			gtk_widget_grab_focus(GTK_WIDGET(row));
			break;
		}
	}
}

void Window::drop_group(const rem::SidebarGroup& group,
						const rem::SidebarGroup& target, bool after) {
	if (!store_ ||
		!rem::move_sidebar_group_next_to(order_, group, target, after)) {
		return;
	}
	try {
		rem::save_sidebar_order(order_);
	} catch (const std::exception& e) {
		toast(std::format("Couldn't save the sidebar order: {}", e.what()));
	}
	rebuild_sidebar();
}

std::optional<Window::EntryOrder> Window::entry_order(const View& v) {
	if (!store_) {
		return std::nullopt;
	}
	if (smart_info(v.kind)) {
		auto order = smart_.shown;	// a hidden smart list has no place to move
		return EntryOrder{order, order, view_to_string(v)};
	}
	if (v.kind == View::Tag) {
		return EntryOrder{rem::order_tags(store_->tags()), sidebar_tags(),
						  v.name};
	}
	if (v.kind == View::List) {
		auto* l = store_->list(v.name);
		if (!l) {
			return std::nullopt;
		}
		std::vector<std::string> showing;  // this list's group
		for (auto* x : sidebar_lists(store_->source_of(*l)->config.name)) {
			showing.push_back(store_->key_of(*x));
		}
		// every source's, each keeping its place
		return EntryOrder{rem::order_lists(list_keys()), std::move(showing),
						  v.name};
	}
	return std::nullopt;
}

void Window::save_entry_order(const View& v,
							  const std::vector<std::string>& order) {
	if (smart_info(v.kind)) {
		rem::save_smart_lists(order);
		smart_ = rem::load_smart_lists_layout();
	} else if (v.kind == View::Tag) {
		rem::save_names_setting("tags-order", order);
	} else if (v.kind == View::List) {
		rem::save_names_setting("lists-order", order);
	}
}

bool Window::can_move_entry(const View& v, int delta) {
	auto e = entry_order(v);
	return e && rem::move_in_order(e->order, e->name, delta, e->showing);
}

void Window::move_entry(const View& v, int delta) {
	auto e = entry_order(v);
	if (!e || !rem::move_in_order(e->order, e->name, delta, e->showing)) {
		return;
	}
	try {
		save_entry_order(v, e->order);
	} catch (const std::exception& e) {
		toast(std::format("Couldn't save the order: {}", e.what()));
		return;
	}
	rebuild_sidebar();
	focus_entry(v);
}

// Entries are dragged only within their group: smart lists among smart
// lists, a source's lists among that source's, tags among tags.
bool Window::same_sidebar_group(const View& a, const View& b) {
	if (a == b || !store_) {
		return false;
	}
	if (smart_info(a.kind) || smart_info(b.kind)) {
		return smart_info(a.kind) && smart_info(b.kind);
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
	auto *la = store_->list(a.name), *lb = store_->list(b.name);
	return la && lb && store_->source_of(*la) == store_->source_of(*lb);
}

void Window::drop_entry(const View& v, const View& target, bool after) {
	if (!same_sidebar_group(v, target)) {
		return;
	}
	auto e = entry_order(v);
	auto t = entry_order(target);
	if (!e || !t || !rem::move_next_to(e->order, e->name, t->name, after)) {
		return;
	}
	try {
		save_entry_order(v, e->order);
	} catch (const std::exception& e) {
		toast(std::format("Couldn't save the order: {}", e.what()));
		return;
	}
	rebuild_sidebar();
	focus_entry(v);
}

void Window::focus_entry(const View& v) {
	for (int i = 0;; ++i) {	 // keep focus on the entry that moved
		auto* row =
			gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_list_), i);
		if (!row) {
			break;
		}
		if (auto* rv = row_view(row); rv && *rv == v) {
			gtk_widget_grab_focus(GTK_WIDGET(row));
			break;
		}
	}
}

// The sidebar's context menu, for the group of the row under the pointer.
void Window::sidebar_menu(GtkListBoxRow* row, double x, double y) {
	if (!store_ || !row) {
		return;
	}
	Obj<GMenuModel> menu;
	if (auto* v = row_view(row)) {
		// An entry, which stays as it is (not opened). A list gets the same
		// items as the header's ⋮ menu, but for this list (Show Completed is
		// the window's setting, as in the ⋮ menu); a tag gets Tag Info…; all
		// get Hidden.
		auto view = *v;
		auto name = v->name;
		auto* actions = g_simple_action_group_new();
		add_action(actions, "add-section",
				   [this, name] { idle([this, name] { add_section(name); }); });
		add_action(actions, "list-info",
				   [this, name] { idle([this, name] { edit_list(name); }); });
		add_action(actions, "delete-list",
				   [this, name] { idle([this, name] { delete_list(name); }); });
		add_action(actions, "export-list", [this, name] {
			idle([this, name] { export_lists({name}); });
		});
		add_action(actions, "tag-info",
				   [this, name] { idle([this, name] { edit_tag(name); }); });
		auto* up = add_action(actions, "move-up", [this, view] {
			idle([this, view] { move_entry(view, -1); });
		});
		auto* down = add_action(actions, "move-down", [this, view] {
			idle([this, view] { move_entry(view, 1); });
		});
		g_simple_action_set_enabled(up, can_move_entry(view, -1));
		g_simple_action_set_enabled(down, can_move_entry(view, 1));
		bool hidden = entry_hidden(view);
		add_action(
			actions, "hide",
			[this, view, hidden] {	// Hide, or Show for a hidden one
				idle([this, view, hidden] { set_entry_hidden(view, !hidden); });
			});
		gtk_widget_insert_action_group(sidebar_menu_button_, "sidebar-entry",
									   G_ACTION_GROUP(actions));
		g_object_unref(actions);
		auto* m = g_menu_new();
		if (view.kind == View::List) {
			g_menu_append(menu_section(m), "_Show Completed",
						  "win.show-completed");
			auto* edit = menu_section(m);
			g_menu_append(edit, "Add _Section…", "sidebar-entry.add-section");
			g_menu_append(edit, "List _Info…", "sidebar-entry.list-info");
			g_menu_append(edit, "_Export…", "sidebar-entry.export-list");
		} else if (view.kind == View::Tag) {
			g_menu_append(menu_section(m), "Tag _Info…",
						  "sidebar-entry.tag-info");
		}
		{  // the entry's place in its group
			auto* moves = menu_section(m);
			g_menu_append(moves, "Move _Up", "sidebar-entry.move-up");
			g_menu_append(moves, "Move _Down", "sidebar-entry.move-down");
		}
		g_menu_append(menu_section(m), hidden ? "_Show" : "_Hide",
					  "sidebar-entry.hide");
		if (view.kind == View::List) {
			g_menu_append(menu_section(m), "_Delete List…",
						  "sidebar-entry.delete-list");
		}
		menu = Obj<GMenuModel>::adopt(G_MENU_MODEL(m));
	} else {
		// A group heading: move the group, make it collapsible.
		auto group = row_group(row);
		if (!group) {
			return;
		}
		menu_group_ = *group;
		auto showing = showing_groups();
		auto can = [&](int delta) {
			auto order = order_;
			return rem::move_sidebar_group(order, *group, delta, showing);
		};
		// Every item goes in a section: loose items next to a section make the
		// popover size itself wrongly (clipped, with scrollbars).
		auto* m = g_menu_new();
		auto* moves = menu_section(m);
		auto title = group_title(*group);
		g_menu_append(moves, std::format("Move “{}” _Up", title).c_str(),
					  "win.move-group-up");
		g_menu_append(moves, std::format("Move “{}” _Down", title).c_str(),
					  "win.move-group-down");
		auto enable = [this](const char* name, bool on) {
			g_simple_action_set_enabled(
				G_SIMPLE_ACTION(
					g_action_map_lookup_action(G_ACTION_MAP(window_), name)),
				on);
		};
		enable("move-group-up", can(-1));
		enable("move-group-down", can(1));
		g_menu_append(menu_section(m), "_Collapsible", "win.group-collapsible");
		g_simple_action_set_state(
			collapsible_action_, g_variant_new_boolean(group_foldable(*group)));
		if (group->kind == rem::SidebarGroup::Lists) {	// a source's lists
			auto source = group->source;
			auto* actions = g_simple_action_group_new();
			add_action(actions, "new-list", [this, source] {
				idle([this, source] { new_list(source); });
			});
			add_action(actions, "remove", [this, source] {
				idle([this, source] { remove_source(source); });
			});
			gtk_widget_insert_action_group(sidebar_menu_button_,
										   "sidebar-source",
										   G_ACTION_GROUP(actions));
			g_object_unref(actions);
			add_action(actions, "info", [this, source] {
				idle([this, source] { source_info(source); });
			});
			// A source this app syncs (CalDAV, WebDAV, git): Sync Now, for
			// just this one.
			bool syncs = false;
#ifdef REMINDERS_NETWORK
			for (auto& s : store_->sources()) {
				if (s.config.name == source && rem::syncs(s.config.backend)) {
					syncs = sync_ != nullptr;
				}
			}
			add_action(actions, "sync", [this, source] {
				if (sync_) {
					sync_->sync_now(source);
				}
			});
#endif
			auto* items = menu_section(m);
			g_menu_append(items, "_New List…", "sidebar-source.new-list");
			if (syncs) {
				g_menu_append(items, "_Sync Now", "sidebar-source.sync");
			}
			g_menu_append(items, "Source _Info…", "sidebar-source.info");
		}
		menu = Obj<GMenuModel>::adopt(G_MENU_MODEL(m));
	}
	// The menu belongs to an invisible menu button over the sidebar, not to
	// the list box (rebuilding that removes all its children, popovers too),
	// and not to a plain widget, which never re-sizes a popover whose items
	// arrive after it opens (it came out clipped, with scrollbars).
	auto* button = GTK_MENU_BUTTON(sidebar_menu_button_);
	gtk_menu_button_set_menu_model(button, menu.get());
	auto* popover = GTK_POPOVER(gtk_menu_button_get_popover(button));
	graphene_point_t in_list{static_cast<float>(x), static_cast<float>(y)},
		point{};
	if (!gtk_widget_compute_point(sidebar_list_, sidebar_menu_button_, &in_list,
								  &point)) {
		point = in_list;
	}
	GdkRectangle at{static_cast<int>(point.x), static_cast<int>(point.y), 1, 1};
	gtk_popover_set_pointing_to(popover, &at);
	gtk_popover_set_has_arrow(popover, FALSE);
	gtk_menu_button_popup(button);
}

}  // namespace ui
