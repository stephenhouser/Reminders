// In-memory model of a list file. See docs/FORMAT.md.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rem {

using Date = std::chrono::year_month_day;

struct TimeOfDay {
    int hour = 0;
    int minute = 0;
    bool operator==(const TimeOfDay&) const = default;
};

enum class Priority { None = 0, Low = 1, Medium = 2, High = 3 };

inline constexpr std::string_view kColors[] = {
    "red", "orange", "yellow", "green", "cyan", "blue",
    "indigo", "purple", "pink", "brown", "gray"};

inline constexpr std::string_view kIcons[] = {
    "list", "bookmark", "cart", "gift", "home", "work", "school",
    "calendar", "flag", "star", "heart", "music", "game", "book", "food",
    "travel", "nature", "person", "people", "money", "pill", "computer",
    "camera"};

// The values stored on a reminder's own line (everything except id, notes
// and subtasks).
struct LineFields {
    std::string title;
    bool done = false;
    std::vector<std::string> tags;
    Priority priority = Priority::None;
    bool flagged = false;
    std::optional<std::string> repeat;
    std::optional<Date> due_date;
    std::optional<TimeOfDay> due_time;
    std::optional<Date> completed;
    std::optional<Date> created;
    std::optional<std::string> url;

    bool operator==(const LineFields&) const = default;
};

struct Reminder : LineFields {
    std::string id;     // empty if the line on disk has none yet
    std::string notes;  // '\n'-separated
    std::vector<Reminder> subtasks;

    // The line as read from disk and the fields it parsed to. If the fields
    // are unchanged, the original line is written back untouched, so
    // hand-written lines keep their formatting.
    std::optional<std::string> source_line;
    LineFields source_fields;
    bool source_had_id = false;

    LineFields& fields() { return *this; }
    const LineFields& fields() const { return *this; }

    // Everything except the id, used to compare versions when merging.
    bool same_content(const Reminder& other) const;
};

struct RawLine {
    std::string text;
    // "## Name" → "Name"; otherwise empty.
    std::optional<std::string> section() const;
};

using Block = std::variant<Reminder, RawLine>;

struct Section {
    std::optional<std::string> name;  // nullopt = before the first heading
    std::vector<Reminder*> reminders;
};

struct Document {
    bool has_front = false;
    // Front matter in file order. A value that spans several lines (nested
    // YAML) starts with '\n' and keeps its continuation lines verbatim.
    std::vector<std::pair<std::string, std::string>> front;
    std::vector<Block> blocks;

    std::optional<std::string> meta(std::string_view key) const;
    void set_meta(std::string_view key, std::optional<std::string> value);

    // The front-matter key that makes a Markdown file a list ("reminders: 1",
    // where 1 is the format version).
    static constexpr std::string_view kMarker = "reminders";
    static constexpr std::string_view kFormatVersion = "1";
    bool is_list() const { return meta(kMarker).has_value(); }
    // Adds the marker as the first front-matter key, if it's missing.
    void mark_as_list();

    std::vector<Reminder*> reminders();
    std::vector<Section> sections();

    // Depth-first over every reminder and subtask.
    template <class F> void walk(F&& f) {
        for (auto& b : blocks)
            if (auto* r = std::get_if<Reminder>(&b)) {
                f(*r, static_cast<Reminder*>(nullptr));
                for (auto& s : r->subtasks) f(s, r);
            }
    }

    Reminder* find(std::string_view id, Reminder** parent = nullptr);
    std::optional<std::string> section_of(const Reminder& r) const;

    // Gives every reminder without an id a fresh one not in `taken`; adds
    // all ids to `taken`.
    void ensure_ids(std::vector<std::string>& taken);

    // Insert a top-level reminder after `anchor` (any top-level reminder or
    // subtask; a subtask anchor inserts a sibling subtask), or, if anchor is
    // null, at the end of `section` (created at the end of the file if
    // missing). Returns the inserted reminder.
    Reminder& insert(Reminder r, const Reminder* anchor,
                     const std::optional<std::string>& section = std::nullopt);
    // Removes and returns the reminder with this id.
    std::optional<Reminder> remove(std::string_view id);
    // Moves reminder `id` to sit directly before `before_id` (null = end of
    // its section/parent). Both must share the same parent.
    bool move_before(std::string_view id, std::optional<std::string_view> before_id);

    enum class Place { Before, After };
    // Drag and drop: moves reminder `id` next to `target`, taking target's
    // level. Next to a top-level reminder it becomes top-level (in target's
    // section); next to a subtask it becomes a subtask of the same parent.
    // "After" a reminder with subtasks means after the whole block. Returns
    // false, changing nothing, if the move would nest subtasks or put a
    // reminder next to itself or inside itself.
    bool move_next_to(std::string_view id, std::string_view target, Place place);
    // Keyboard reordering: moves `id` one step up or down among the sibling
    // reminders for which `visible` is true (hidden ones are skipped over).
    // Past the edge of a section it moves into the neighbouring section;
    // subtasks stay within their parent. Returns false at the very edge.
    bool move_step(std::string_view id, bool up, const std::function<bool(const Reminder&)>& visible);
    // Makes top-level reminder `id` the last subtask of the nearest reminder
    // above it in the same section for which `visible` is true. False if it
    // already is a subtask, has subtasks of its own, or nothing is above it.
    bool indent(std::string_view id, const std::function<bool(const Reminder&)>& visible);
    // Makes subtask `id` a top-level reminder directly after its parent.
    bool outdent(std::string_view id);
    // Section headings. Both return false if `name` doesn't exist (or, for a
    // rename, `new_name` is empty or already taken).
    bool rename_section(const std::string& name, const std::string& new_name);
    // Removes the heading. With `keep_reminders` its reminders join the
    // section above; otherwise everything under the heading goes too.
    bool delete_section(const std::string& name, bool keep_reminders);
    // Moves `id` to the end of a section as a top-level reminder.
    bool move_to_end(std::string_view id, const std::optional<std::string>& section);

private:
    std::size_t section_end(const std::optional<std::string>& section);
};

std::string new_id();

}  // namespace rem
