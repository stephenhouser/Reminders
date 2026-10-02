// Undo and redo, as before/after versions of the list files an action changed.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "reminders/store.hpp"

namespace rem {

class History {
public:
    // Records the difference between two snapshots (snapshot()) as
    // one undoable step. Returns its id, or 0 if nothing changed.
    std::uint64_t record(std::string label, const Snapshot& before, const Snapshot& after);

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    // Id of the step that undo() would revert, or 0.
    std::uint64_t next_undo() const { return undo_.empty() ? 0 : undo_.back().id; }
    std::string undo_label() const { return undo_.empty() ? "" : undo_.back().label; }
    std::string redo_label() const { return redo_.empty() ? "" : redo_.back().label; }

    struct Result {
        bool applied = false;  // false: nothing to undo/redo
        // Lists that changed elsewhere in a way the step can't be applied to
        // (e.g. deleted on another device); left as they are.
        std::vector<std::string> skipped;
    };
    Result undo(ListTexts& lists);
    Result redo(ListTexts& lists);
    void clear();

private:
    struct Change {
        std::string list;
        std::optional<std::string> before, after;  // nullopt: no such list
    };
    struct Step {
        std::uint64_t id;
        std::string label;
        std::vector<Change> changes;
    };
    static constexpr std::size_t kLimit = 100;
    std::vector<Step> undo_, redo_;
    std::uint64_t next_id_ = 1;

    static Result apply(ListTexts& store, const Step& step, bool backwards);
};

}  // namespace rem
