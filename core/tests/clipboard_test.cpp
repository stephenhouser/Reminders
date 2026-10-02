#include "reminders/clipboard.hpp"
#include "reminders/format.hpp"
#include "test.hpp"

using namespace rem;

TEST(copied_reminder_is_markdown_without_ids) {
    auto doc = parse(
        "- [ ] Buy milk #errands ⏫ 🚩 📅 2026-10-03 09:00 🔗 https://example.com ^k3x9qa\n"
        "  2% if they have it\n"
        "  - [x] Check expiry date ^p0d2mf\n");
    auto& r = *doc.reminders().front();
    auto text = to_clipboard_text(r);
    CHECK_EQ(text,
             "- [ ] Buy milk #errands ⏫ 🚩 📅 2026-10-03 09:00 🔗 https://example.com\n"
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
    auto pasted = from_clipboard_text("Shopping:\r\n- [ ] Eggs\r\n- [x] Bread ✅ 2026-09-30\r\n## Later\r\n");
    CHECK_EQ(pasted.size(), 2u);
    CHECK_EQ(pasted[0].title, "Eggs");
    CHECK(pasted[1].done);
}

TEST(pasted_plain_text_is_one_reminder_per_line) {
    auto pasted = from_clipboard_text("  Call Sam #work\n\n- Pick up parcel\n• Water plants 📅 2026-10-05\n");
    CHECK_EQ(pasted.size(), 3u);
    CHECK_EQ(pasted[0].title, "Call Sam");
    CHECK_EQ(pasted[0].tags.size(), 1u);
    CHECK_EQ(pasted[1].title, "Pick up parcel");
    CHECK_EQ(pasted[2].title, "Water plants");
    CHECK(pasted[2].due_date.has_value());
    CHECK(from_clipboard_text(" \n\n").empty());
}
