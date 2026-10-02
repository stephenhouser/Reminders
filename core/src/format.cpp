#include "reminders/format.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <vector>

namespace rem {

namespace {

constexpr std::string_view kDue = "\U0001F4C5";        // 📅
constexpr std::string_view kDone = "✅";           // ✅
constexpr std::string_view kCreated = "➕";        // ➕
constexpr std::string_view kRepeat = "\U0001F501";     // 🔁
constexpr std::string_view kUrl = "\U0001F517";        // 🔗
constexpr std::string_view kFlag = "\U0001F6A9";       // 🚩
constexpr std::string_view kHigh = "⏫";           // ⏫
constexpr std::string_view kHighest = "\U0001F53A";    // 🔺
constexpr std::string_view kMedium = "\U0001F53C";     // 🔼
constexpr std::string_view kLow = "\U0001F53D";        // 🔽
constexpr std::string_view kLowest = "⏬";         // ⏬
constexpr std::string_view kVariationSelector = "️";

constexpr std::string_view kFieldEmoji[] = {kDue, kDone, kCreated, kRepeat, kUrl, kFlag,
                                            kHigh, kHighest, kMedium, kLow, kLowest};

bool is_space(char c) { return c == ' ' || c == '\t'; }

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) ++i;
        auto start = i;
        while (i < s.size() && !is_space(s[i])) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
    return s;
}

bool is_tag(std::string_view t) {
    if (t.size() < 2 || t[0] != '#') return false;
    auto first = static_cast<unsigned char>(t[1]);
    if (!(std::isalpha(first) || first == '_' || first >= 0x80)) return false;
    return std::ranges::all_of(t.substr(1), [](char ch) {
        auto c = static_cast<unsigned char>(ch);
        return std::isalnum(c) || c == '_' || c == '/' || c == '-' || c >= 0x80;
    });
}

bool is_id_token(std::string_view t) {
    return t.size() >= 2 && t[0] == '^' &&
           std::ranges::all_of(t.substr(1), [](char ch) {
               auto c = static_cast<unsigned char>(ch);
               return std::isalnum(c) || c == '-';
           });
}

bool is_field_emoji(std::string_view t) {
    return std::ranges::find(kFieldEmoji, t) != std::end(kFieldEmoji);
}

int parse_int(std::string_view s) {
    int v = -1;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return (ec == std::errc{} && p == s.data() + s.size()) ? v : -1;
}

// Indentation width: a space counts 1, a tab 2 (the spec's "2 spaces or a tab").
int indent_width(std::string_view line) {
    int w = 0;
    for (char c : line) {
        if (c == ' ') w += 1;
        else if (c == '\t') w += 2;
        else break;
    }
    return w;
}

std::string_view strip_indent(std::string_view line, int width) {
    int w = 0;
    std::size_t i = 0;
    while (i < line.size() && w < width && is_space(line[i])) {
        w += line[i] == '\t' ? 2 : 1;
        ++i;
    }
    return line.substr(i);
}

bool is_blank(std::string_view line) {
    return line.find_first_not_of(" \t") == std::string_view::npos;
}

struct TaskLine {
    int indent;
    bool done;
    std::string_view rest;
};

// "- [ ] rest", "* [x] rest", "+ [X]" (title may be empty).
std::optional<TaskLine> match_task(std::string_view line) {
    auto indent = indent_width(line);
    auto body = line.substr(line.find_first_not_of(" \t") == std::string_view::npos
                                ? line.size()
                                : line.find_first_not_of(" \t"));
    if (body.size() < 5) return std::nullopt;
    if (body[0] != '-' && body[0] != '*' && body[0] != '+') return std::nullopt;
    if (body[1] != ' ' || body[2] != '[' || body[4] != ']') return std::nullopt;
    char mark = body[3];
    if (mark != ' ' && mark != 'x' && mark != 'X') return std::nullopt;
    auto rest = body.substr(5);
    if (!rest.empty()) {
        if (rest[0] != ' ') return std::nullopt;
        rest.remove_prefix(1);
    }
    return TaskLine{indent, mark != ' ', rest};
}

Reminder make_reminder(std::string_view line, const TaskLine& t) {
    Reminder r;
    r.fields() = parse_fields(t.rest, &r.id);
    r.done = t.done;
    r.source_line = std::string(line);
    r.source_fields = r.fields();
    r.source_had_id = !r.id.empty();
    return r;
}

// Blank note lines before the first real one are dropped.
void append_note(Reminder& r, std::string_view text) {
    if (r.notes.empty()) {
        r.notes = text;
    } else {
        r.notes += '\n';
        r.notes += text;
    }
}

}  // namespace

std::optional<Date> parse_date(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
    int y = parse_int(s.substr(0, 4)), m = parse_int(s.substr(5, 2)), d = parse_int(s.substr(8, 2));
    if (y < 0 || m < 0 || d < 0) return std::nullopt;
    Date date{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(m)},
              std::chrono::day{static_cast<unsigned>(d)}};
    if (!date.ok()) return std::nullopt;
    return date;
}

std::string format_date(Date d) {
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(d.year()),
                       static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
}

std::optional<TimeOfDay> parse_time(std::string_view s) {
    auto colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 2 || s.size() != colon + 3)
        return std::nullopt;
    int h = parse_int(s.substr(0, colon)), m = parse_int(s.substr(colon + 1));
    if (h < 0 || h > 23 || m < 0 || m > 59) return std::nullopt;
    return TimeOfDay{h, m};
}

std::string format_time(TimeOfDay t) { return std::format("{:02}:{:02}", t.hour, t.minute); }

LineFields parse_fields(std::string_view text, std::string* id) {
    // Drop variation selectors and make sure every field emoji is its own
    // token, so "milk📅2026-10-03" still parses.
    auto s = replace_all(std::string(text), kVariationSelector, "");
    for (auto e : kFieldEmoji) s = replace_all(std::move(s), e, std::format(" {} ", e));
    auto tokens = split_ws(s);

    LineFields f;
    std::vector<std::string_view> title;
    std::size_t end = tokens.size();
    if (end > 0 && is_id_token(tokens[end - 1])) {
        if (id) *id = std::string(tokens[end - 1].substr(1));
        --end;
    }

    auto next = [&](std::size_t i) -> std::optional<std::string_view> {
        return i + 1 < end ? std::optional{tokens[i + 1]} : std::nullopt;
    };

    for (std::size_t i = 0; i < end; ++i) {
        auto t = tokens[i];
        if (is_tag(t)) {
            f.tags.emplace_back(t.substr(1));
        } else if (t == kHigh || t == kHighest) {
            f.priority = Priority::High;
        } else if (t == kMedium) {
            f.priority = Priority::Medium;
        } else if (t == kLow || t == kLowest) {
            f.priority = Priority::Low;
        } else if (t == kFlag) {
            f.flagged = true;
        } else if (t == kDue && next(i) && parse_date(*next(i))) {
            f.due_date = parse_date(*next(i));
            ++i;
            if (auto n = next(i); n && parse_time(*n)) {
                f.due_time = parse_time(*n);
                ++i;
            }
        } else if ((t == kDone || t == kCreated) && next(i) && parse_date(*next(i))) {
            (t == kDone ? f.completed : f.created) = parse_date(*next(i));
            ++i;
        } else if (t == kUrl && next(i)) {
            f.url = std::string(*next(i));
            ++i;
        } else if (t == kRepeat && next(i) && !is_field_emoji(*next(i)) && !is_tag(*next(i))) {
            std::string rule;
            while (auto n = next(i)) {
                if (is_field_emoji(*n) || is_tag(*n)) break;
                if (!rule.empty()) rule += ' ';
                rule += *n;
                ++i;
            }
            f.repeat = std::move(rule);
        } else {
            title.push_back(t);
        }
    }
    for (auto t : title) {
        if (!f.title.empty()) f.title += ' ';
        f.title += t;
    }
    return f;
}

std::string format_fields(const LineFields& f, std::string_view id) {
    std::vector<std::string> parts;
    if (!f.title.empty()) parts.push_back(f.title);
    for (auto& t : f.tags) parts.push_back("#" + t);
    switch (f.priority) {
        case Priority::High: parts.emplace_back(kHigh); break;
        case Priority::Medium: parts.emplace_back(kMedium); break;
        case Priority::Low: parts.emplace_back(kLow); break;
        case Priority::None: break;
    }
    if (f.flagged) parts.emplace_back(kFlag);
    if (f.repeat) parts.push_back(std::format("{} {}", kRepeat, *f.repeat));
    if (f.due_date) {
        auto due = std::format("{} {}", kDue, format_date(*f.due_date));
        if (f.due_time) due += " " + format_time(*f.due_time);
        parts.push_back(std::move(due));
    }
    if (f.completed) parts.push_back(std::format("{} {}", kDone, format_date(*f.completed)));
    if (f.created) parts.push_back(std::format("{} {}", kCreated, format_date(*f.created)));
    if (f.url) parts.push_back(std::format("{} {}", kUrl, *f.url));
    if (!id.empty()) parts.push_back(std::format("^{}", id));

    std::string out;
    for (auto& p : parts) {
        if (!out.empty()) out += ' ';
        out += p;
    }
    return out;
}

Document parse(std::string_view text) {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start < text.size();) {
        auto nl = text.find('\n', start);
        auto line = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        lines.push_back(line);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }

    Document doc;
    std::size_t i = 0;

    // Front matter: "---" ... "---" at the very top.
    if (!lines.empty() && lines[0] == "---") {
        auto close = std::find_if(lines.begin() + 1, lines.end(),
                                  [](auto l) { return l == "---" || l == "..."; });
        if (close != lines.end()) {
            doc.has_front = true;
            for (auto it = lines.begin() + 1; it != close; ++it) {
                auto l = *it;
                auto colon = l.find(':');
                bool is_key = colon != std::string_view::npos && colon > 0 && !is_space(l[0]) &&
                              l[0] != '#' && l[0] != '-' &&
                              std::ranges::all_of(l.substr(0, colon), [](char c) {
                                  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                                         c == '-';
                              });
                if (is_key) {
                    auto value = l.substr(colon + 1);
                    if (value.starts_with(' ')) value.remove_prefix(1);
                    doc.front.emplace_back(std::string(l.substr(0, colon)), std::string(value));
                } else if ((l.empty() || is_space(l[0]) || l[0] == '-') && !doc.front.empty() &&
                           !doc.front.back().first.empty()) {
                    doc.front.back().second += "\n" + std::string(l);
                } else {
                    doc.front.emplace_back("", std::string(l));  // comment or stray line
                }
            }
            i = static_cast<std::size_t>(close - lines.begin()) + 1;
        }
    }

    // Index of the reminder whose block we're in (notes/subtasks attach to it).
    std::optional<std::size_t> current;
    auto cur = [&]() -> Reminder& { return std::get<Reminder>(doc.blocks[*current]); };

    for (; i < lines.size(); ++i) {
        auto line = lines[i];
        if (auto task = match_task(line)) {
            auto r = make_reminder(line, *task);
            if (task->indent > 0 && current) {
                cur().subtasks.push_back(std::move(r));
            } else {
                doc.blocks.emplace_back(std::move(r));
                current = doc.blocks.size() - 1;
            }
            continue;
        }
        if (current && !is_blank(line) && is_space(line[0])) {
            auto& top = cur();
            if (!top.subtasks.empty() && indent_width(line) >= 4)
                append_note(top.subtasks.back(), strip_indent(line, 4));
            else
                append_note(top, strip_indent(line, 2));
            continue;
        }
        if (current && is_blank(line)) {
            // Blank lines stay inside the reminder when the block carries on
            // (indented) afterwards.
            auto j = i;
            while (j < lines.size() && is_blank(lines[j])) ++j;
            if (j < lines.size() && is_space(lines[j][0])) {
                if (!match_task(lines[j])) {
                    auto& top = cur();
                    auto& target = (!top.subtasks.empty() && indent_width(lines[j]) >= 4)
                                       ? top.subtasks.back()
                                       : top;
                    for (auto k = i; k < j; ++k) append_note(target, "");
                }
                i = j - 1;
                continue;
            }
        }
        doc.blocks.emplace_back(RawLine{std::string(line)});
        current.reset();
    }
    return doc;
}

namespace {

void write_reminder(std::string& out, const Reminder& r, std::string_view indent, bool new_ids) {
    // Reuse the original line only if its fields are unchanged and it still
    // sits at the same nesting level (a moved subtask needs new indentation).
    bool reuse = r.source_line && r.fields() == r.source_fields &&
                 (indent_width(*r.source_line) == 0) == indent.empty();
    if (reuse && (r.source_had_id || r.id.empty() || !new_ids)) {
        out += *r.source_line;
    } else if (reuse) {
        out += *r.source_line + " ^" + r.id;  // just adding the missing id
    } else {
        out += indent;
        out += r.done ? "- [x] " : "- [ ] ";
        out += format_fields(r.fields(), r.id);
        if (out.ends_with(' ')) out.pop_back();  // empty reminder: "- [ ]"
    }
    out += '\n';

    // Notes, without trailing blank lines.
    std::string_view notes = r.notes;
    while (notes.ends_with('\n')) notes.remove_suffix(1);
    if (!notes.empty()) {
        for (std::size_t start = 0;;) {
            auto nl = notes.find('\n', start);
            auto line = notes.substr(start, nl == std::string_view::npos ? std::string_view::npos
                                                                         : nl - start);
            if (!line.empty()) {
                out += indent;
                out += "  ";
                out += line;
            }
            out += '\n';
            if (nl == std::string_view::npos) break;
            start = nl + 1;
        }
    }

    auto sub_indent = std::string(indent) + "  ";
    for (auto& s : r.subtasks) write_reminder(out, s, sub_indent, new_ids);
}

}  // namespace

std::string serialize(const Document& doc, bool new_ids) {
    std::string out;
    if (doc.has_front) {
        out += "---\n";
        for (auto& [k, v] : doc.front) {
            if (k.empty()) {
                out += v;
            } else {
                out += k;
                out += ':';
                if (!v.empty() && v[0] != '\n') out += ' ';
                out += v;
            }
            out += '\n';
        }
        out += "---\n";
    }
    for (auto& b : doc.blocks) {
        if (auto* raw = std::get_if<RawLine>(&b)) {
            out += raw->text;
            out += '\n';
        } else {
            write_reminder(out, std::get<Reminder>(b), "", new_ids);
        }
    }
    return out;
}

}  // namespace rem
