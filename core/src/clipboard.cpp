#include "reminders/clipboard.hpp"

#include "reminders/format.hpp"

#include <algorithm>
#include <optional>
#include <sstream>

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

namespace {

std::string without_cr(std::string_view text) {  // text from Windows apps
    std::string out;
    for (char c : text)
        if (c != '\r') out += c;
    return out;
}

std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> out;
    for (std::size_t start = 0; start <= text.size();) {
        auto nl = text.find('\n', start);
        out.push_back(text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
        start = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    }
    return out;
}

// The line without a leading bullet or number ("- ", "• ", "1. ", "2) "),
// trimmed; `had` says whether it had one.
std::string_view strip_marker(std::string_view line, bool* had = nullptr) {
    line = trimmed(line);
    if (had) *had = false;
    for (std::string_view bullet : {"- ", "* ", "+ ", "• "})
        if (line.starts_with(bullet)) {
            if (had) *had = true;
            return trimmed(line.substr(bullet.size()));
        }
    std::size_t digits = 0;
    while (digits < line.size() && digits < 4 && line[digits] >= '0' && line[digits] <= '9') ++digits;
    if (digits > 0 && digits + 1 < line.size() && (line[digits] == '.' || line[digits] == ')') && line[digits + 1] == ' ') {
        if (had) *had = true;
        return trimmed(line.substr(digits + 2));
    }
    return line;
}

// A checklist line in notes would be read back as a subtask (the format has
// no escape), so "- [ ] Eggs" is kept as "☐ Eggs" ("☑" when ticked).
std::string checkbox_as_symbol(std::string_view line) {
    auto indent = line.find_first_not_of(" \t");
    if (indent == std::string_view::npos) return std::string(line);
    auto rest = line.substr(indent);
    if (rest.size() >= 6 && (rest[0] == '-' || rest[0] == '*' || rest[0] == '+') && rest.substr(1, 2) == " [" &&
        (rest[3] == ' ' || rest[3] == 'x' || rest[3] == 'X') && rest.substr(4, 2) == "] ")
        return std::string(line.substr(0, indent)) + (rest[3] == ' ' ? "☐ " : "☑ ") + std::string(rest.substr(6));
    return std::string(line);
}

// The first web address (http:// or https://, up to a space) in text: its
// start and length, without trailing punctuation that ends a sentence or
// closes a bracket it doesn't open.
std::optional<std::pair<std::size_t, std::size_t>> find_url(std::string_view s) {
    std::size_t at = std::string_view::npos;
    for (std::string_view scheme : {"https://", "http://"})
        if (auto p = s.find(scheme); p != std::string_view::npos && (p == 0 || s[p - 1] == ' ' || s[p - 1] == '(' ||
                                                                      s[p - 1] == '<' || s[p - 1] == '"'))
            at = std::min(at, p);
    if (at == std::string_view::npos) return std::nullopt;
    auto end = s.find_first_of(" \t\"<>", at);
    if (end == std::string_view::npos) end = s.size();
    auto url = s.substr(at, end - at);
    while (!url.empty()) {
        char c = url.back();
        bool open = (c == ')' && url.find('(') == std::string_view::npos) ||
                    (c == ']' && url.find('[') == std::string_view::npos);
        if (std::string_view(".,;:!?'").find(c) == std::string_view::npos && !open) break;
        url.remove_suffix(1);
    }
    if (url.size() <= 8) return std::nullopt;  // just "https://"
    return std::pair{at, url.size()};
}

// A web address in the title goes in the URL field too, so it shows as a
// link. Without other words the address stays the title; with them it's
// taken out of it.
void take_url(Reminder& r) {
    if (r.url) return;
    auto found = find_url(r.title);
    if (!found) return;
    auto [at, n] = *found;
    r.url = r.title.substr(at, n);
    auto rest = r.title.substr(0, at) + r.title.substr(at + n);
    // Brackets left empty around it, and doubled spaces.
    for (std::string_view pair : {"()", "<>", "[]", "\"\""})
        if (auto p = rest.find(pair); p != std::string::npos) rest.erase(p, 2);
    std::string tidy;
    for (char c : rest) {
        if (c == ' ' && (tidy.empty() || tidy.back() == ' ')) continue;
        if (std::string_view(".,;:!?").find(c) != std::string_view::npos && !tidy.empty() && tidy.back() == ' ')
            tidy.pop_back();  // "this ." → "this."
        tidy += c;
    }
    while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '-' || tidy.back() == ':')) tidy.pop_back();
    if (!tidy.empty()) r.title = tidy;
}

Reminder from_line(std::string_view line) {
    Reminder r;
    r.fields() = parse_fields(line);
    if (r.title.empty()) r.title = std::string(line);  // nothing but fields: keep the text
    take_url(r);
    return r;
}

// Checklist lines in text, as reminders with their fields; none if it has none.
std::vector<Reminder> checklist_of(const std::string& text) {
    std::vector<Reminder> out;
    auto doc = parse(text);
    for (auto& b : doc.blocks)
        if (auto* r = std::get_if<Reminder>(&b)) {
            detach(*r);
            out.push_back(std::move(*r));
        }
    return out;
}

}  // namespace

std::size_t text_line_count(std::string_view text) {
    std::size_t n = 0;
    for (auto line : lines_of(text))
        if (!trimmed(line).empty()) ++n;
    return n;
}

bool is_list_text(std::string_view text) {
    auto clean = without_cr(text);
    if (!checklist_of(clean).empty()) return true;
    std::size_t lines = 0, marked = 0;
    for (auto line : lines_of(clean)) {
        if (trimmed(line).empty()) continue;
        ++lines;
        bool had = false;
        strip_marker(line, &had);
        marked += had;
    }
    return marked > 0 && marked * 2 >= lines;
}

std::vector<Reminder> from_clipboard_text(std::string_view text, TextSplit split) {
    auto clean = without_cr(text);
    if (split == TextSplit::Auto) split = is_list_text(clean) ? TextSplit::Lines : TextSplit::One;
    std::vector<Reminder> out;
    if (split == TextSplit::Lines) {
        out = checklist_of(clean);
        if (!out.empty()) return out;
        for (auto line : lines_of(clean))
            if (auto item = strip_marker(line); !item.empty()) out.push_back(from_line(item));
        return out;
    }
    // One: the first line is the title, the rest the notes.
    auto lines = lines_of(clean);
    std::size_t i = 0;
    while (i < lines.size() && trimmed(lines[i]).empty()) ++i;
    if (i == lines.size()) return out;
    auto r = from_line(strip_marker(lines[i]));
    std::size_t first = i + 1, last = lines.size();
    while (first < last && trimmed(lines[first]).empty()) ++first;
    while (last > first && trimmed(lines[last - 1]).empty()) --last;
    std::string notes;
    for (auto j = first; j < last; ++j) {
        auto line = lines[j];
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        notes += checkbox_as_symbol(line) + (j + 1 < last ? "\n" : "");
    }
    // No address in the title: one on a line of its own in the notes is the
    // URL ("Article title" then its address).
    if (!r.url) {
        std::string kept;
        std::istringstream in(notes);
        bool first_line = true;
        for (std::string line; std::getline(in, line);) {
            auto t = std::string(trimmed(line));
            if (!r.url && find_url(t) && find_url(t)->first == 0 && find_url(t)->second == t.size()) {
                r.url = t;
                continue;
            }
            kept += (first_line ? "" : "\n") + line;
            first_line = false;
        }
        while (!kept.empty() && kept.back() == '\n') kept.pop_back();
        while (!kept.empty() && kept.front() == '\n') kept.erase(0, 1);
        notes = kept;
    }
    if (!notes.empty()) r.notes = r.notes.empty() ? notes : r.notes + "\n" + notes;
    out.push_back(std::move(r));
    return out;
}

}  // namespace rem
