#include "reminders/view.hpp"

#include "test.hpp"

using namespace rem;

TEST(views_round_trip_through_settings) {
	for (auto v : {View{View::Today, ""}, View{View::AllReminders, ""},
				   View{View::List, "home/Todo"}, View{View::Tag, "work"}}) {
		CHECK(view_from_string(view_to_string(v)) == v);
	}
	CHECK_EQ(view_to_string({View::List, "home/Todo"}), "list:home/Todo");
	CHECK_EQ(view_to_string({View::Search, "milk"}),
			 "today");	// searches aren't kept
	CHECK(view_from_string("nonsense") == (View{View::Today, ""}));
	CHECK_EQ(smart_view_name(View::AllReminders), "all-reminders");
	CHECK(smart_view_name(View::Tag).empty());
}
