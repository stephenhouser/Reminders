// A source's folder: loading and saving list files, the queries behind the
// smart lists, and (through its back end) what its kind of syncing needs,
// such as merging Syncthing conflict copies.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "reminders/backend.hpp"
#include "reminders/model.hpp"

namespace rem {

// The count under a view's title (the GUI's subtitle, the TUI's title line):
//   OpenOnly       "5 Reminders"                 Today, Scheduled, All, Flagged
//   WithComplete   "6 Reminders / 3 Complete"     a list, a tag, All Reminders
//                  (the "/ N Complete" part only when some are done)
//   Completed      "2 Completed"
//   Results        "3 Results"                   a search
enum class CountStyle { OpenOnly, WithComplete, Completed, Results };
std::string count_label(CountStyle style, int total, int done = 0);
// The same, short, for narrow spaces: "8/2" (total/done), or "5".
std::string count_short(CountStyle style, int total, int done = 0);

namespace fs = std::filesystem;

struct ListFile {
    std::string name;  // file name without ".md"
    Document doc;
    std::string disk_text;  // the file's content as of our last read or write

    std::string color() const;  // always one of kColors
    std::string icon() const;   // always one of kIcons
    std::optional<int> order() const;
};

// Every list's file content, by list name.
using Snapshot = std::map<std::string, std::string>;

// A reminder found somewhere in the store. Valid until the next change.
struct Ref {
    ListFile* list = nullptr;
    Reminder* reminder = nullptr;
    Reminder* parent = nullptr;  // set for subtasks
};

class Store {
public:
    // `state_dir` holds per-device data that must not be synced: the
    // candidates the user declined, and the back end's own records (for
    // Syncthing, per list, the merge base and a fingerprint of the last
    // version this device wrote).
    Store(fs::path folder, fs::path state_dir, BackendKind backend = BackendKind::Syncthing);

    BackendKind backend() const { return backend_->kind(); }
    // Back-end set-up when the source is opened (Syncthing: .stignore).
    void prepare() { backend_->prepare(folder_); }

    const fs::path& folder() const { return folder_; }
    fs::path path_of(std::string_view list_name) const;

    // Scans the folder and loads every list, merging any conflict copies.
    void load_all();
    // Re-reads one list after it changed on disk (or a conflict copy for it
    // appeared). Returns false if nothing changed, e.g. it was our own write.
    bool reload(const std::string& name);
    // Maps any file in the folder to the list it would belong to (including
    // conflict copies), or nullopt if it can't be a list file. Whether it
    // is one depends on the marker in its front matter.
    std::optional<std::string> list_name_for(const fs::path& file) const { return backend_->list_name_for(file); }

    // Markdown files with checklists but no marker, which the user may want
    // to use as lists. Excludes ones they declined.
    const std::vector<std::string>& candidates() const { return candidates_; }
    // Adds the marker to a candidate, making it a list.
    void adopt(const std::string& name);
    // Stops suggesting a candidate (remembered per folder on this device).
    void decline(const std::string& name);

    // In sidebar order: by "order", then by name.
    std::vector<ListFile*> lists();
    ListFile* list(std::string_view name);

    // Writes the list to disk if its content changed.
    void save(ListFile& list);

    ListFile& create_list(const std::string& name, std::string_view color, std::string_view icon);
    bool rename_list(ListFile& list, const std::string& new_name);
    // Deletes the file (callers wanting the trash should move it there first).
    void delete_list(const std::string& name);

    std::optional<Ref> find(std::string_view id);

    // For undo: the content of every list, and putting a list back to some
    // content (nullopt deletes the file).
    Snapshot snapshot() const;
    std::optional<std::string> current_text(const std::string& name) const;
    void restore(const std::string& name, const std::optional<std::string>& text);

    // Edits that save the affected list(s) straight away.
    Reminder& add(ListFile& list, Reminder r, const Reminder* after = nullptr,
                  const std::optional<std::string>& section = std::nullopt);
    // Completing a repeating reminder adds the next occurrence above it.
    // Completing a reminder also completes its subtasks.
    void set_done(std::string_view id, bool done, Date today);
    void remove(std::string_view id);
    void move_to_list(std::string_view id, ListFile& dest);
    // Saves after the caller changed a reminder's fields through a Ref.
    void touch(std::string_view id);

    // Smart lists.
    std::vector<Ref> today(Date today);  // open, due today or overdue
    std::vector<Ref> scheduled();        // open with a due date, by date
    std::vector<Ref> all();              // open
    std::vector<Ref> everything();       // open and completed ("All Reminders")
    std::vector<Ref> flagged();          // open and flagged
    std::vector<Ref> completed();        // done, most recent first
    std::vector<Ref> tagged(std::string_view tag);
    std::vector<Ref> search(std::string_view query);
    std::vector<std::string> tags();     // every tag in use, sorted

private:
    fs::path folder_;
    fs::path state_dir_;
    std::unique_ptr<Backend> backend_;
    std::vector<std::unique_ptr<ListFile>> lists_;
    std::vector<std::string> candidates_;

    bool forget(const std::string& name);
    void update_candidate(const std::string& name, const std::optional<std::string>& text);
    std::vector<std::string> declined() const;

    template <class Pred> std::vector<Ref> collect(Pred&& pred);
    std::vector<std::string> taken_ids();
    void write_file(ListFile& list, const std::string& text);
};

}  // namespace rem
