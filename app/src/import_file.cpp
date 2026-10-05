#include "reminders/import_file.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rem {

namespace {

std::string lower(std::string_view s) {
	std::string out(s);
	for (auto& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

std::string plural(int n, std::string_view one, std::string_view many) {
	return std::format("{} {}", n, n == 1 ? one : many);
}

}  // namespace

Import read_import_file(const std::filesystem::path& path,
						std::optional<Import::Kind> kind) {
	std::error_code ec;
	if (std::filesystem::is_directory(path, ec)) {
		throw std::runtime_error(
			std::format("{} is a folder", path.filename().string()));
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error(std::format("can't read {}", path.string()));
	}
	std::ostringstream text;
	text << in.rdbuf();
	auto imp = kind ? read_as(text.str(), *kind, std::chrono::current_zone())
					: read_import(text.str(), std::chrono::current_zone(),
								  path.filename().string());
	if (imp.items.empty()) {
		throw std::runtime_error(
			std::format("there are no reminders to import in {}",
						path.filename().string()));
	}
	return imp;
}

std::string import_list_name(const Import& import,
							 const std::filesystem::path& path) {
	auto name = import.name.empty() ? path.stem().string() : import.name;
	std::ranges::replace(name, '/', '-');
	return name;
}

std::vector<ListFile*> lists_called(Library& library, std::string_view name,
									const std::string& source) {
	std::vector<ListFile*> found;
	auto want = lower(name);
	for (auto* l : source.empty() ? library.lists() : library.lists(source)) {
		if (lower(l->name) == want || lower(library.key_of(*l)) == want) {
			found.push_back(l);
		}
	}
	return found;
}

std::string list_name_error(std::string_view name) {
	if (name.empty()) {
		return "Enter a name";
	}
	if (name.front() == '.') {
		return "Names can't start with a dot";
	}
	if (name.find_first_of("/\\<>:\"|?*") != std::string_view::npos) {
		return "Names can't contain / \\ < > : \" | ? *";
	}
	if (name.find(".sync-conflict-") != std::string_view::npos) {
		return "That name is reserved";
	}
	return {};
}

ImportDone import_to(Library& library, ListFile* list,
					 const std::string& source, const std::string& name,
					 const Import& import, bool duplicates) {
	ImportDone done;
	if (!list) {
		list = &library.create_list(
			source, name, import.color.empty() ? "blue" : import.color, "list");
		done.created = true;
	}
	done.key = library.key_of(*list);
	done.result = import_into(library, *list, import, duplicates);
	if (done.created && done.result.added == 0) {  // no empty list either
		library.delete_list(done.key);
		done.key.clear();
		done.created = false;
	}
	return done;
}

std::string import_summary(Library& library, const ImportDone& done,
						   const Import& import) {
	auto* list = done.key.empty() ? nullptr : library.list(done.key);
	if (!list) {  // a new list, and nothing was new
		return std::format(
			"Nothing to import: {} already here",
			done.result.already == 1
				? "the one reminder is"
				: std::format("all {} reminders are", done.result.already));
	}
	auto text = std::format("Imported {} ({}) into {}{}",
							plural(done.result.added, "reminder", "reminders"),
							kind_name(import.kind), library.label(*list),
							done.created ? " (a new list)" : "");
	std::vector<std::string> notes;
	if (done.result.already) {
		notes.push_back(plural(done.result.already, "was already there",
							   "were already there"));
	}
	if (import.skipped) {
		notes.push_back(plural(import.skipped, "event or other item skipped",
							   "events or other items skipped"));
	}
	for (std::size_t i = 0; i < notes.size(); ++i) {
		text += (i ? ", " : "; ") + notes[i];
	}
	return text;
}

}  // namespace rem
