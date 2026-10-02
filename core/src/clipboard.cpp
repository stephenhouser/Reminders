#include "reminders/clipboard.hpp"

#include "reminders/format.hpp"

namespace rem {

namespace {

// Forgets where a reminder came from: its ids, and the original lines (which
// end in the ids), so it's written fresh from its fields.
void detach(Reminder& r) {
    r.id.clear();
    r.source_line.reset();
    r.source_fields = {};
    r.source_had_id = false;
    for (auto& s : r.subtasks) detach(s);
}

std::string_view trimmed(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::string to_clipboard_text(const Reminder& r) {
    Document doc;
    Reminder copy = r;
    detach(copy);
    doc.blocks.emplace_back(std::move(copy));
    return serialize(doc, false);
}

std::vector<Reminder> from_clipboard_text(std::string_view text) {
    std::vector<Reminder> out;
    std::string normalized;  // without carriage returns (text from Windows apps)
    for (char c : text)
        if (c != '\r') normalized += c;
    auto doc = parse(normalized);
    for (auto& b : doc.blocks)
        if (auto* r = std::get_if<Reminder>(&b)) {
            detach(*r);
            out.push_back(std::move(*r));
        }
    if (!out.empty()) return out;

    for (std::size_t start = 0; start <= normalized.size();) {
        auto nl = normalized.find('\n', start);
        auto line = trimmed(std::string_view(normalized).substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        start = nl == std::string::npos ? normalized.size() + 1 : nl + 1;
        for (std::string_view bullet : {"- ", "* ", "+ ", "• "})
            if (line.starts_with(bullet)) {
                line = trimmed(line.substr(bullet.size()));
                break;
            }
        if (line.empty()) continue;
        Reminder r;
        r.fields() = parse_fields(line);
        if (r.title.empty()) r.title = std::string(line);  // nothing but fields: keep the text
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace rem
