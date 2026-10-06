#include "reminders/command_line.hpp"

#include <stdexcept>

#include "test.hpp"

using namespace rem;

namespace {

using Words = std::vector<std::string>;

TEST(command_line_splits_like_a_shell) {
	CHECK((split_command_line("") == Words{}));
	CHECK((split_command_line("  quit  ") == Words{"quit"}));
	CHECK((split_command_line("add Buy milk --list Home") ==
		   Words{"add", "Buy", "milk", "--list", "Home"}));
	CHECK(
		(split_command_line("import '~/My Files/a.ics' --list \"Big Plans\"") ==
		 Words{"import", "~/My Files/a.ics", "--list", "Big Plans"}));
	CHECK((split_command_line(R"(add It\'s\ here)") ==
		   Words{"add", "It's here"}));
	CHECK((split_command_line(R"(a "say \"hi\"" 'no\escape')") ==
		   Words{"a", "say \"hi\"", "no\\escape"}));
	CHECK((split_command_line("a''b \"\"") == Words{"ab", ""}));
	CHECK((split_command_line("a\tb") == Words{"a", "b"}));
	bool threw = false;
	try {
		split_command_line("add 'unclosed");
	} catch (const std::runtime_error&) {
		threw = true;
	}
	CHECK(threw);
}

TEST(command_line_words_know_where_they_are) {
	auto w = split_words("import 'My Fi", true);
	CHECK_EQ(w.size(), 2u);
	CHECK_EQ(w[1].text, "My Fi");
	CHECK_EQ(w[1].begin, 7u);
	CHECK_EQ(w[1].end, 13u);
	CHECK_EQ(split_words("go ").back().end, 2u);
}

TEST(command_line_quotes_words_that_need_it) {
	CHECK_EQ(quote_word("Home"), "Home");
	CHECK_EQ(quote_word("Big Plans"), "\"Big Plans\"");
	CHECK_EQ(quote_word(R"(a"b\c)"), R"("a\"b\\c")");
	CHECK_EQ(quote_word(""), "\"\"");
	for (auto s : {"Big Plans", "it's", R"(a"b\c)", "x"}) {
		CHECK((split_command_line(quote_word(s)) == Words{s}));
	}
}

}  // namespace
