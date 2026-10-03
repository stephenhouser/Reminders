#include "reminders/exporter.hpp"

#include <cctype>
#include <charconv>
#include <format>
#include <fstream>

#include "file_util.hpp"
#include "reminders/caldav.hpp"
#include "reminders/format.hpp"
#include "reminders/ical.hpp"
#include "reminders/vtodo.hpp"

namespace rem {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A reminder's line in plain text: its fields, without the bookkeeping
// (id, created date).
std::string text_line(const Reminder& r) {
    LineFields f = r;
    f.created.reset();
    return format_fields(f, "");
}

// A repeat rule as todo.txt's rec: ("every 2 weeks" → "2w"), or empty.
std::string rec_from_rule(std::string_view rule) {
    auto r = lower(rule);
    if (r == "every weekday") return "1b";
    auto words = detail::split(r, ' ');
    if (words.empty() || words[0] != "every" || words.size() < 2 || words.size() > 3) return "";
    int n = 1;
    if (words.size() == 3) {
        auto& w = words[1];
        auto [p, ec] = std::from_chars(w.data(), w.data() + w.size(), n);
        if (ec != std::errc{} || p != w.data() + w.size() || n < 1) return "";
    }
    auto unit = words.back();
    if (unit.size() > 1 && unit.back() == 's') unit.pop_back();
    char u = unit == "day" ? 'd' : unit == "week" ? 'w' : unit == "month" ? 'm' : unit == "year" ? 'y' : 0;
    return u ? std::format("{}{}", n, u) : "";
}

std::string todotxt_line(const Reminder& r, const Reminder* parent) {
    std::vector<std::string> parts;
    if (r.done) {
        parts.emplace_back("x");
        if (r.completed) {
            parts.push_back(format_date(*r.completed));
            if (r.created) parts.push_back(format_date(*r.created));
        }
    } else {
        if (r.priority != Priority::None)
            parts.push_back(r.priority == Priority::High ? "(A)" : r.priority == Priority::Medium ? "(B)" : "(C)");
        if (r.created) parts.push_back(format_date(*r.created));
    }
    parts.push_back(r.title);
    for (auto& t : r.tags) parts.push_back("+" + t);
    if (r.due_date) parts.push_back("due:" + format_date(*r.due_date));
    if (r.due_date && r.due_time) parts.push_back("time:" + format_time(*r.due_time));
    if (r.repeat)
        if (auto rec = rec_from_rule(*r.repeat); !rec.empty()) parts.push_back("rec:" + rec);
    if (r.done && r.priority != Priority::None)
        parts.push_back(r.priority == Priority::High ? "pri:A" : r.priority == Priority::Medium ? "pri:B" : "pri:C");
    if (r.flagged) parts.emplace_back("flag:yes");
    if (r.url) parts.push_back("url:" + *r.url);
    if (!r.id.empty()) parts.push_back("id:" + r.id);
    if (parent && !parent->id.empty()) parts.push_back("p:" + parent->id);
    std::string out;
    for (auto& p : parts) out += (out.empty() ? "" : " ") + p;
    return out;
}

std::string csv_field(std::string_view s) {
    if (s.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(s);
    std::string out = "\"";
    for (char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

std::string priority_name(Priority p) {
    switch (p) {
    case Priority::High: return "high";
    case Priority::Medium: return "medium";
    case Priority::Low: return "low";
    case Priority::None: break;
    }
    return "";
}

}  // namespace

std::optional<ExportFormat> export_format(std::string_view name) {
    if (name.starts_with('.')) name.remove_prefix(1);
    auto n = lower(name);
    if (n == "md" || n == "markdown") return ExportFormat::Markdown;
    if (n == "txt" || n == "text") return ExportFormat::Text;
    if (n == "ics" || n == "ical" || n == "icalendar") return ExportFormat::Ics;
    if (n == "todo.txt" || n == "todotxt" || n == "todo") return ExportFormat::Todotxt;
    if (n == "csv") return ExportFormat::Csv;
    return std::nullopt;
}

std::optional<ExportFormat> export_format_for(const std::string& file_name) {
    auto name = lower(fs::path(file_name).filename().string());
    if (name == "todo.txt" || name == "done.txt" || name.ends_with(".todo.txt")) return ExportFormat::Todotxt;
    auto dot = file_name.rfind('.');
    if (dot == std::string::npos || file_name.find('/', dot) != std::string::npos) return std::nullopt;
    return export_format(std::string_view(file_name).substr(dot + 1));
}

std::string_view export_extension(ExportFormat format) {
    switch (format) {
    case ExportFormat::Text: return "txt";
    case ExportFormat::Ics: return "ics";
    case ExportFormat::Todotxt: return "todo.txt";
    case ExportFormat::Csv: return "csv";
    case ExportFormat::Markdown: break;
    }
    return "md";
}

std::string export_markdown(const Document& doc) { return serialize(doc); }

std::string export_text(const Document& doc, bool completed) {
    Document copy = doc;
    std::string out;
    for (auto& section : copy.sections()) {
        std::string lines;
        for (auto* r : section.reminders) {
            if (r->done && !completed) continue;
            lines += text_line(*r) + "\n";
            for (auto& s : r->subtasks)
                if (!s.done || completed) lines += "  " + text_line(s) + "\n";
        }
        if (lines.empty()) continue;
        if (section.name) out += (out.empty() ? "" : "\n") + std::string("# ") + *section.name + "\n";
        out += lines;
    }
    return out;
}

std::string export_ics(const Document& doc, std::string_view name, const ExportOptions& options) {
    using namespace std::chrono;
    auto now = options.now != sys_seconds{} ? options.now : floor<seconds>(system_clock::now());
    auto* zone = options.zone ? options.zone : current_zone();

    IcalComponent cal{"VCALENDAR", {}, {}};
    cal.set("VERSION", "2.0");
    cal.set("PRODID", "-//stephenhouser//Reminders//EN");
    cal.set("X-WR-CALNAME", ical_escape(name));
    if (auto hex = hex_from_color(doc.meta("color").value_or("")); !hex.empty()) cal.set("X-APPLE-CALENDAR-COLOR", hex);

    long long order = 0;
    auto add = [&](const Reminder& r, const Reminder* parent, const std::optional<std::string>& section) {
        Todo want;
        want.uid = r.id;
        want.reminder = r;
        want.reminder.subtasks.clear();
        want.section = section;
        want.sort_order = order += 1024;
        if (parent) want.parent_uid = parent->id;
        auto one = new_todo_calendar(want, now, zone);
        cal.children.push_back(std::move(one.children.front()));
    };
    Document copy = doc;
    std::vector<std::string> taken;
    copy.ensure_ids(taken);  // subtasks name their parent by its UID
    for (auto& section : copy.sections())
        for (auto* r : section.reminders) {
            add(*r, nullptr, section.name);
            for (auto& s : r->subtasks) add(s, r, std::nullopt);
        }
    return serialize_ical(cal);
}

std::string export_todotxt(const Document& doc) {
    Document copy = doc;
    std::vector<std::string> taken;
    copy.ensure_ids(taken);  // subtasks name their parent by its id
    std::string out;
    for (auto& section : copy.sections())
        for (auto* r : section.reminders) {
            out += todotxt_line(*r, nullptr) + "\n";
            for (auto& s : r->subtasks) out += todotxt_line(s, r) + "\n";
        }
    return out;
}

std::string export_csv(const Document& doc, std::string_view list_name) {
    std::string out = "List,Section,Title,Done,Due Date,Due Time,Priority,Flagged,Tags,Repeat,URL,Notes,Completed,Created,ID,"
                      "Parent ID\r\n";
    auto row = [&](const Reminder& r, const std::optional<std::string>& section, const Reminder* parent) {
        std::string tags;
        for (auto& t : r.tags) tags += (tags.empty() ? "" : " ") + t;
        std::vector<std::string> cells{
            std::string(list_name),
            section.value_or(""),
            r.title,
            r.done ? "yes" : "",
            r.due_date ? format_date(*r.due_date) : "",
            r.due_date && r.due_time ? format_time(*r.due_time) : "",
            priority_name(r.priority),
            r.flagged ? "yes" : "",
            tags,
            r.repeat.value_or(""),
            r.url.value_or(""),
            r.notes,
            r.completed ? format_date(*r.completed) : "",
            r.created ? format_date(*r.created) : "",
            r.id,
            parent ? parent->id : "",
        };
        for (std::size_t i = 0; i < cells.size(); ++i) out += (i ? "," : "") + csv_field(cells[i]);
        out += "\r\n";
    };
    Document copy = doc;
    std::vector<std::string> taken;
    copy.ensure_ids(taken);
    for (auto& section : copy.sections())
        for (auto* r : section.reminders) {
            row(*r, section.name, nullptr);
            for (auto& s : r->subtasks) row(s, section.name, r);
        }
    return out;
}

std::string export_list(const ListFile& list, ExportFormat format, const ExportOptions& options) {
    switch (format) {
    case ExportFormat::Text: return export_text(list.doc, options.completed);
    case ExportFormat::Ics: return export_ics(list.doc, list.name, options);
    case ExportFormat::Todotxt: return export_todotxt(list.doc);
    case ExportFormat::Csv: return export_csv(list.doc, list.name);
    case ExportFormat::Markdown: break;
    }
    return export_markdown(list.doc);
}

std::vector<fs::path> export_all(Library& library, const fs::path& folder, ExportFormat format,
                                 const ExportOptions& options) {
    return export_lists(library, library.lists(), folder, format, options);
}

std::vector<fs::path> export_lists(Library& library, const std::vector<ListFile*>& lists, const fs::path& folder,
                                   ExportFormat format, const ExportOptions& options) {
    fs::create_directories(folder);
    std::vector<fs::path> out;
    for (auto* list : lists) {
        // "work/Todo" where two sources have a Todo: "work-Todo".
        auto name = library.label(*list);
        std::ranges::replace(name, '/', '-');
        auto path = folder / std::format("{}.{}", name, export_extension(format));
        detail::write_atomic(path, export_list(*list, format, options));
        out.push_back(path);
    }
    return out;
}

}  // namespace rem
