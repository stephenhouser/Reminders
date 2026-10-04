#include "reminders/selection.hpp"

#include "test.hpp"

using namespace rem;

TEST(selection_ranges_and_targets) {
	std::vector<std::string> shown{"a", "b", "c", "d", "e"};
	Selection s;
	s.toggle("b");
	CHECK(s.select_range(shown, "d", false));  // from the anchor b
	CHECK((s.in_order(shown) == std::vector<std::string>{"b", "c", "d"}));
	CHECK((s.targets("c", shown) == std::vector<std::string>{"b", "c", "d"}));
	CHECK((s.targets("e", shown) ==
		   std::vector<std::string>{"e"}));	 // not selected: just it
	s.toggle("a");							 // anchor a now
	CHECK(s.select_range(shown, "a", true));
	CHECK_EQ(s.size(), 4u);
	CHECK(!s.select_range(shown, "zz", false));
	// Pruned to what's shown.
	CHECK(s.prune({"a", "c"}));
	CHECK((s.in_order(shown) == std::vector<std::string>{"a", "c"}));
	s.clear();
	// No anchor: from the fallback (the focused one).
	Selection t;
	t.select_range(shown, "c", false, std::string("e"));
	CHECK((t.in_order(shown) == std::vector<std::string>{"c", "d", "e"}));
	t.select_all(shown);
	CHECK_EQ(t.size(), 5u);
	t.select_only("b");
	CHECK_EQ(t.size(), 1u);
	CHECK(t.anchor() == std::optional<std::string>("b"));
}
