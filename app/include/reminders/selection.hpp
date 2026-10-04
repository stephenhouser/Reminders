// The reminders selected in a view (the GNOME app's Ctrl/Shift+click and
// Ctrl+A, the terminal interface's marks), by id. Separate from the cursor
// or focus. Order comes from the view: `shown` is its reminders, top to
// bottom.
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rem {

class Selection {
	public:
		bool empty() const { return ids_.empty(); }
		std::size_t size() const { return ids_.size(); }
		bool contains(const std::string& id) const { return ids_.contains(id); }
		const std::set<std::string>& ids() const { return ids_; }
		// Where a range extends from: the reminder last clicked or toggled.
		const std::optional<std::string>& anchor() const { return anchor_; }
		void set_anchor(std::optional<std::string> id) {
			anchor_ = std::move(id);
		}

		void clear() { ids_.clear(); }
		// Just this one, and the anchor.
		void select_only(const std::string& id);
		// Adds or removes one; it becomes the anchor.
		void toggle(const std::string& id);
		// From the anchor (else `fallback`, else `to`) to `to`, in `shown`
		// order; `add` keeps what was selected. False if `to` isn't shown.
		bool select_range(
			const std::vector<std::string>& shown, const std::string& to,
			bool add,
			const std::optional<std::string>& fallback = std::nullopt);
		void select_all(const std::vector<std::string>& shown);
		// Drops those no longer shown. True if any were.
		bool prune(const std::vector<std::string>& shown);

		// The selected ones in `shown` order.
		std::vector<std::string> in_order(
			const std::vector<std::string>& shown) const;
		// What an action on `id` applies to: the selection when `id` is part
		// of it, else just `id`.
		std::vector<std::string> targets(
			const std::string& id, const std::vector<std::string>& shown) const;

	private:
		std::set<std::string> ids_;
		std::optional<std::string> anchor_;
};

}  // namespace rem
