// The sidebar both apps show: its groups (the smart lists, each source's
// lists, tags) in order, each group's entries, which are hidden or folded,
// and moving them. Holds the layout from settings.ini (sidebar-order,
// smart-lists, the group displays, lists-hidden, …; see settings.hpp) and
// saves each change there; the save functions throw when that fails.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "reminders/library.hpp"
#include "reminders/preferences.hpp"
#include "reminders/view.hpp"

namespace rem {

class Sidebar {
	public:
		explicit Sidebar(Library& library) : library_(library) { reload(); }
		// Reads the layout again (settings.ini changed, or the sources did).
		void reload();

		// The groups with something to show, in order.
		std::vector<SidebarGroup> groups();
		// "Smart Lists", "Tags", or for lists "My Lists" (with several
		// sources: the source's title).
		std::string title(const SidebarGroup& group);
		bool foldable(const SidebarGroup& group);
		bool folded(const SidebarGroup& group);
		// Folds or unfolds it (remembered when settings.ini can be written).
		void toggle_fold(const SidebarGroup& group);
		// Makes it collapsible (unfolded) or not. Saves.
		void set_foldable(const SidebarGroup& group, bool foldable);

		// A group's entries in order: the smart lists, a source's lists (by
		// key), or tags. Hidden ones only while show_hidden().
		std::vector<View> entries(const SidebarGroup& group);
		std::vector<View> smart_views();
		std::vector<ListFile*> lists(const std::string& source);
		std::vector<std::string> tags();
		// Every entry, in sidebar order; `include_folded` adds those of
		// folded groups (Go To finds them).
		std::vector<View> all(bool include_folded = false);
		// Where to land when there's nothing better: Today, unless it's
		// hidden, else the first entry.
		View home();
		// The number beside an entry: open reminders (smart lists by what
		// they show); none for a tag.
		std::optional<int> count(const View& v, Date today);

		// Hidden in settings.ini (whether or not showing).
		bool hidden(const View& v);
		void set_hidden(const View& v, bool hidden);  // saves
		bool show_hidden() const { return hidden_.show; }
		void set_show_hidden(bool show);  // saves
		// The Tags group itself is hidden (tags-display=hidden).
		bool tags_group_hidden() const { return tags_.hidden(); }
		// Whether `v` is gone from the sidebar: hidden and not showing, a
		// smart list not showing, or a tag while the Tags group is hidden.
		bool gone(const View& v);

		// Entries move within their group only: smart lists among smart
		// lists, a source's lists among that source's, tags among tags.
		bool same_group(const View& a, const View& b);
		// Past the next entry showing before it (delta < 0) or after it.
		bool can_move(const View& v, int delta);
		bool move(const View& v, int delta);  // saves; false if it can't
		// Next to another entry of its group (dragged there).
		bool move_next_to(const View& v, const View& target,
						  bool after);	// saves
		bool can_move_group(const SidebarGroup& group, int delta);
		bool move_group(const SidebarGroup& group, int delta);	// saves
		bool move_group_next_to(const SidebarGroup& group,
								const SidebarGroup& target,
								bool after);  // saves

	private:
		struct EntryOrder {
				std::vector<std::string>
					order;	// as saved, hidden ones included
				std::vector<std::string>
					showing;	   // the group's entries showing
				std::string name;  // the entry's name in `order`
		};
		std::optional<EntryOrder> entry_order(const View& v);
		void save_order(const View& v, const std::vector<std::string>& order);
		GroupLayout* layout_of(
			const SidebarGroup& group);	 // nullptr for the smart lists
		std::vector<std::string> list_keys();

		Library& library_;
		std::vector<SidebarGroup> order_;
		SmartListsLayout smart_;
		std::map<std::string, GroupLayout>
			lists_layouts_;	 // by source; loaded as needed
		GroupLayout tags_;
		HiddenEntries hidden_;
};

}  // namespace rem
