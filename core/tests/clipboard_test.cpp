#include "reminders/clipboard.hpp"

#include "reminders/format.hpp"
#include "test.hpp"

using namespace rem;

TEST(copied_reminder_is_markdown_without_ids) {
	auto doc = parse(
		"- [ ] Buy milk #errands ⏫ 🚩 📅 2026-10-03 09:00 🔗 "
		"https://example.com ^k3x9qa\n"
		"  2% if they have it\n"
		"  - [x] Check expiry date ^p0d2mf\n");
	auto& r = *doc.reminders().front();
	auto text = to_clipboard_text(r);
	CHECK_EQ(text,
			 "- [ ] Buy milk #errands ⏫ 🚩 📅 2026-10-03 09:00 🔗 "
			 "https://example.com\n"
			 "  2% if they have it\n"
			 "  - [x] Check expiry date\n");
	CHECK_EQ(r.id, "k3x9qa");  // the original keeps its id

	// Pasting it back gives the same reminder, with every field.
	auto pasted = from_clipboard_text(text);
	CHECK_EQ(pasted.size(), 1u);
	auto& p = pasted.front();
	CHECK(p.id.empty());
	CHECK(p.same_content(r));
	CHECK_EQ(p.subtasks.size(), 1u);
	CHECK(p.subtasks.front().done);
	CHECK(p.subtasks.front().id.empty());
}

TEST(pasted_checklist_ignores_other_lines) {
	auto pasted = from_clipboard_text(
		"Shopping:\r\n- [ ] Eggs\r\n- [x] Bread ✅ 2026-09-30\r\n## Later\r\n");
	CHECK_EQ(pasted.size(), 2u);
	CHECK_EQ(pasted[0].title, "Eggs");
	CHECK(pasted[1].done);
}

TEST(pasted_plain_text_is_one_reminder_per_line) {
	auto pasted = from_clipboard_text(
		"  Call Sam #work\n\n- Pick up parcel\n• Water plants 📅 2026-10-05\n");
	CHECK_EQ(pasted.size(), 3u);
	CHECK_EQ(pasted[0].title, "Call Sam");
	CHECK_EQ(pasted[0].tags.size(), 1u);
	CHECK_EQ(pasted[1].title, "Pick up parcel");
	CHECK_EQ(pasted[2].title, "Water plants");
	CHECK(pasted[2].due_date.has_value());
	CHECK(from_clipboard_text(" \n\n").empty());
}

TEST(pasted_text_splits_only_when_it_is_a_list) {
	// Prose: one reminder, the rest its notes.
	auto note = from_clipboard_text(
		"\nDentist on Friday #health\nRemember the forms.\n\nThey close at "
		"five.  \n\n");
	CHECK_EQ(note.size(), 1u);
	CHECK_EQ(note[0].title, "Dentist on Friday");
	CHECK_EQ(note[0].tags.size(), 1u);
	CHECK_EQ(note[0].notes, "Remember the forms.\n\nThey close at five.");
	// Numbered or bulleted lines, a heading allowed: a reminder each.
	auto numbered =
		from_clipboard_text("Packing:\n1. Passport\n2) Charger\n10. Socks\n");
	CHECK_EQ(numbered.size(), 4u);
	CHECK_EQ(numbered[1].title, "Passport");
	CHECK_EQ(numbered[3].title, "Socks");
	CHECK(is_list_text("- milk\n- eggs"));
	CHECK(is_list_text("Shopping:\n- [ ] Eggs\n"));
	CHECK(!is_list_text("milk\neggs\nbread"));
	CHECK(!is_list_text(
		"Notes\n- one aside\nmore text\nand more"));  // a bullet, but mostly
													  // prose
	CHECK(!is_list_text("2026 was a year. It ended.\nThen another"));
	// Asked for either way.
	CHECK_EQ(from_clipboard_text("milk\neggs\nbread", TextSplit::Lines).size(),
			 3u);
	auto one = from_clipboard_text("- milk\n- eggs\n- bread", TextSplit::One);
	CHECK_EQ(one.size(), 1u);
	CHECK_EQ(one[0].title, "milk");
	CHECK_EQ(one[0].notes, "- eggs\n- bread");
	// One line is one reminder whichever way.
	CHECK_EQ(from_clipboard_text("Call Sam", TextSplit::Lines).size(), 1u);
	CHECK_EQ(from_clipboard_text("Call Sam", TextSplit::One)[0].notes, "");
	CHECK_EQ(text_line_count("a\n\n b \n"), 2u);
}

TEST(combined_list_notes_read_back_as_notes) {
	auto one = from_clipboard_text("- Apples\n- Pears\n1. Plums\n* [ ] odd",
								   TextSplit::One);
	Document doc;
	doc.blocks.emplace_back(one[0]);
	auto text = serialize(doc, false);
	auto back = parse(text);
	std::size_t reminders = 0;
	for (auto& b : back.blocks) {
		if (auto* r = std::get_if<Reminder>(&b)) {
			++reminders;
			CHECK_EQ(r->notes, one[0].notes);
			CHECK(r->subtasks.empty());
		}
	}
	CHECK_EQ(reminders, 1u);
	CHECK_EQ(serialize(back, false), text);
	CHECK_EQ(one[0].notes, "- Pears\n1. Plums\n☐ odd");
	CHECK_EQ(
		from_clipboard_text("Trip\n  - [x] Tickets", TextSplit::One)[0].notes,
		"  ☑ Tickets");
}

TEST(pasted_links_fill_the_url_field) {
	auto bare = from_clipboard_text("https://example.com/a?b=1");
	CHECK_EQ(bare[0].title, "https://example.com/a?b=1");
	CHECK_EQ(bare[0].url.value_or(""), "https://example.com/a?b=1");
	auto titled = from_clipboard_text(
		"Example Page https://example.com/a");	// a link dropped from Firefox
	CHECK_EQ(titled[0].title, "Example Page");
	CHECK_EQ(titled[0].url.value_or(""), "https://example.com/a");
	auto sentence =
		from_clipboard_text("Read this (https://example.com/post). #later");
	CHECK_EQ(sentence[0].title, "Read this.");
	CHECK_EQ(sentence[0].url.value_or(""), "https://example.com/post");
	CHECK_EQ(sentence[0].tags.size(), 1u);
	auto wiki = from_clipboard_text("https://en.wikipedia.org/wiki/Foo_(bar)");
	CHECK_EQ(wiki[0].url.value_or(""),
			 "https://en.wikipedia.org/wiki/Foo_(bar)");
	// A list: each line's own link.
	auto list =
		from_clipboard_text("- Docs https://a.example/docs\n- Plain item");
	CHECK_EQ(list.size(), 2u);
	CHECK_EQ(list[0].url.value_or(""), "https://a.example/docs");
	CHECK(!list[1].url);
	// One reminder: an address alone on a line of the notes.
	auto article = from_clipboard_text(
		"Great article\nhttps://example.com/x\nRead by Friday");
	CHECK_EQ(article[0].title, "Great article");
	CHECK_EQ(article[0].url.value_or(""), "https://example.com/x");
	CHECK_EQ(article[0].notes, "Read by Friday");
	// Not a web address.
	CHECK(!from_clipboard_text("Ask about https:// nothing")[0].url);
	CHECK(!from_clipboard_text("ftp://example.com/file")[0].url);
}
