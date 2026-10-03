#include "reminders/exporter.hpp"

#include <cctype>

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

}  // namespace

std::optional<ExportFormat> export_format(std::string_view name) {
    if (name.starts_with('.')) name.remove_prefix(1);
    auto n = lower(name);
    if (n == "md" || n == "markdown") return ExportFormat::Markdown;
    if (n == "txt" || n == "text") return ExportFormat::Text;
    if (n == "ics" || n == "ical" || n == "icalendar") return ExportFormat::Ics;
    return std::nullopt;
}

std::optional<ExportFormat> export_format_for(const std::string& file_name) {
    auto dot = file_name.rfind('.');
    if (dot == std::string::npos || file_name.find('/', dot) != std::string::npos) return std::nullopt;
    return export_format(std::string_view(file_name).substr(dot + 1));
}

std::string_view export_extension(ExportFormat format) {
    switch (format) {
    case ExportFormat::Text: return "txt";
    case ExportFormat::Ics: return "ics";
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

std::string export_list(const ListFile& list, ExportFormat format, const ExportOptions& options) {
    switch (format) {
    case ExportFormat::Text: return export_text(list.doc, options.completed);
    case ExportFormat::Ics: return export_ics(list.doc, list.name, options);
    case ExportFormat::Markdown: break;
    }
    return export_markdown(list.doc);
}

}  // namespace rem
