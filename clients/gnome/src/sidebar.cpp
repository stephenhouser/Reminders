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
#include "support.hpp"
#include "window_internal.hpp"

namespace ui {

void Window::rebuild_sidebar() {
	updating_sidebar_ = true;
	auto* list = GTK_LIST_BOX(sidebar_list_);
	gtk_list_box_remove_all(list);
	auto day = today();

	// Entries are dragged to another place in their group.
	auto make_reorderable = [this](GtkWidget* row, const View& v) {
		make_entry_draggable(row, v);
		make_entry_drop_target(
			row,
			[this, v](const View& dragged) {
				if (!same_sidebar_group(dragged, v)) {
					return false;
				}
				// A hidden smart list has no place in the order to drop next
				// to.
				return rem::smart_view_name(v.kind).empty() || !entry_hidden(v);
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
					auto* row = sidebar_row(s->icon, s->color, s->title,
											static_cast<int>(count));
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
									l->name, open_count(*l));
					set_row_view(row, View{View::List, key});
					if (entry_hidden(View{View::List, key})) {
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
					auto style = rem::load_tag_style(profile_, t);
					auto* row = sidebar_row(list_icon_name(style.icon),
											style.color, "#" + t, std::nullopt);
					set_row_view(row, View{View::Tag, t});
					if (entry_hidden(View{View::Tag, t})) {
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
	return sidebar_ ? sidebar_->smart_views() : std::vector<View>{};
}

std::vector<rem::ListFile*> Window::sidebar_lists(const std::string& source) {
	return sidebar_ ? sidebar_->lists(source) : std::vector<rem::ListFile*>{};
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

std::string Window::group_title(const rem::SidebarGroup& group) {
	return sidebar_ ? sidebar_->title(group) : rem::group_title(group);
}

std::vector<std::string> Window::sidebar_tags() {
	return sidebar_ ? sidebar_->tags() : std::vector<std::string>{};
}

bool Window::entry_hidden(const View& v) {
	return sidebar_ && sidebar_->hidden(v);
}

// Hides or unhides a sidebar entry (saved in settings.ini). Leaving the view
// that was just hidden goes to Today or the first entry showing.
void Window::set_entry_hidden(const View& v, bool hidden) {
	if (!sidebar_) {
		return;
	}
	try {
		sidebar_->set_hidden(v, hidden);
	} catch (const std::exception& e) {
		toast(std::format("Couldn't save the setting: {}", e.what()));
	}
	if (hidden && !sidebar_->show_hidden() && view_ == v) {
		select(home_view());
	}
	rebuild_sidebar();
}

// Tag Info…: the tag's colour and icon, kept in settings.ini.
void Window::edit_tag(const std::string& tag) {
	auto style = rem::load_tag_style(profile_, tag);
	show_tag_dialog(
		window_, tag, ListEdit{"#" + tag, style.color, style.icon, {}},
		[this, tag](ListEdit e) {
			try {
				rem::save_tag_style(profile_, tag, {e.color, e.icon});
			} catch (const std::exception& err) {
				toast(std::format("Couldn't save the setting: {}", err.what()));
			}
			rebuild_sidebar();
		});
}

// Sidebar entries in display order. `include_folded` adds the entries of
// collapsed groups (Go To finds them; numbers skip them).
std::vector<View> Window::sidebar_views(bool include_folded) {
	return sidebar_ ? sidebar_->all(include_folded) : std::vector<View>{};
}

std::vector<rem::SidebarGroup> Window::showing_groups() {
	return sidebar_ ? sidebar_->groups() : std::vector<rem::SidebarGroup>{};
}

bool Window::group_foldable(const rem::SidebarGroup& group) {
	return sidebar_ && sidebar_->foldable(group);
}

bool Window::group_folded(const rem::SidebarGroup& group) {
	return sidebar_ && sidebar_->folded(group);
}

void Window::toggle_fold(const rem::SidebarGroup& group) {
	if (sidebar_) {
		sidebar_->toggle_fold(group);
	}
	rebuild_sidebar();
}

// Moves a group past its neighbour and saves the order. Focus stays on the
// row that had it (or the group's first row).
void Window::move_group(const rem::SidebarGroup& group, int delta) {
	if (!sidebar_) {
		return;
	}
	try {
		if (!sidebar_->move_group(group, delta)) {
			return;
		}
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
	if (!sidebar_) {
		return;
	}
	try {
		if (!sidebar_->move_group_next_to(group, target, after)) {
			return;
		}
	} catch (const std::exception& e) {
		toast(std::format("Couldn't save the sidebar order: {}", e.what()));
	}
	rebuild_sidebar();
}

bool Window::can_move_entry(const View& v, int delta) {
	return sidebar_ && sidebar_->can_move(v, delta);
}

void Window::move_entry(const View& v, int delta) {
	if (!sidebar_) {
		return;
	}
	try {
		if (!sidebar_->move(v, delta)) {
			return;
		}
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
	return sidebar_ && sidebar_->same_group(a, b);
}

void Window::drop_entry(const View& v, const View& target, bool after) {
	if (!sidebar_) {
		return;
	}
	try {
		if (!sidebar_->move_next_to(v, target, after)) {
			return;
		}
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
			menu_append_accel(moves, "Move _Up", "sidebar-entry.move-up",
							  "<Control>Up");
			menu_append_accel(moves, "Move _Down", "sidebar-entry.move-down",
							  "<Control>Down");
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
		auto can = [&](int delta) {
			return sidebar_ && sidebar_->can_move_group(*group, delta);
		};
		// Every item goes in a section: loose items next to a section make the
		// popover size itself wrongly (clipped, with scrollbars).
		auto* m = g_menu_new();
		auto* moves = menu_section(m);
		auto title = group_title(*group);
		menu_append_accel(moves, std::format("Move “{}” _Up", title).c_str(),
						  "win.move-group-up", "<Control><Shift>Up");
		menu_append_accel(moves, std::format("Move “{}” _Down", title).c_str(),
						  "win.move-group-down", "<Control><Shift>Down");
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
			for (auto& s : store_->sources()) {
				if (s.config.name == source && rem::syncs(s.config)) {
					syncs = sync_ != nullptr;
				}
			}
			add_action(actions, "sync", [this, source] {
				if (sync_) {
					sync_->sync_now(source);
				}
			});
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
