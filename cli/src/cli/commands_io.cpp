#include "internal.hpp"

namespace cli {

int App::cmd_import(const Args& a) {
	if (a.positional.size() != 1) {
		throw UsageError(
			"usage: reminders import FILE [--list LIST] [--source SOURCE] "
			"[--format F] [--duplicates]");
	}
	auto path = folder_arg(a.positional[0]);
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error(std::format("can't read {}", path.string()));
	}
	std::ostringstream text;
	text << in.rdbuf();
	rem::Import imp;
	if (auto f = a.get("format")) {
		auto format = rem::export_format(*f);
		if (!format) {
			throw UsageError("--format is md, txt, todo.txt, csv or ics");
		}
		static constexpr rem::Import::Kind kKinds[] = {
			rem::Import::Kind::Markdown, rem::Import::Kind::Text,
			rem::Import::Kind::Ics, rem::Import::Kind::Todotxt,
			rem::Import::Kind::Csv};
		imp = rem::read_as(text.str(), kKinds[static_cast<int>(*format)],
						   std::chrono::current_zone());
	} else {
		imp = rem::read_import(text.str(), std::chrono::current_zone(),
							   path.filename().string());
	}
	if (imp.items.empty()) {
		throw std::runtime_error(
			std::format("there are no reminders to import in {}",
						path.filename().string()));
	}

	// The list: --list (a name, or source/name), else one named after the
	// calendar (or the file); made in --source (or the default source) if
	// there's none.
	auto name = a.get("list").value_or("");
	if (name.empty()) {
		name = imp.name.empty() ? path.stem().string() : imp.name;
		std::ranges::replace(name, '/', '-');
	}
	rem::ListFile* list = nullptr;
	auto source = a.get("source");
	if (source && !store_.store(*source)) {
		throw std::runtime_error(std::format("no source called “{}”", *source));
	}
	std::vector<rem::ListFile*> found;
	for (auto* l : source ? store_.lists(*source) : store_.lists()) {
		if (term::lower(l->name) == term::lower(name) ||
			term::lower(store_.key_of(*l)) == term::lower(name)) {
			found.push_back(l);
		}
	}
	if (found.size() == 1) {
		list = found.front();
	} else if (found.size() > 1) {
		list = &list_named(name);  // says which to choose
	}
	bool created = false;
	if (!list) {
		if (name.empty() || name.front() == '.' ||
			name.find_first_of("\\<>:\"|?*") != std::string::npos) {
			throw UsageError(std::format(
				"“{}” can't be a list name: choose one with --list", name));
		}
		auto& l =
			store_.create_list(source.value_or(store_.default_source()), name,
							   imp.color.empty() ? "blue" : imp.color, "list");
		list = &l;
		created = true;
	}
	auto r = rem::import_into(store_, *list, imp, a.has("duplicates"));
	if (created && r.added == 0) {	// nothing new: no empty list either
		store_.delete_list(store_.key_of(*list));
		if (g_.json) {
			std::cout
				<< std::format(
					   R"({{"list":null,"created":false,"added":0,"already":{},"skipped":{}}})",
					   r.already, imp.skipped)
				<< "\n";
			return 0;
		}
		std::cout << std::format(
			"Nothing to import: {} already here\n",
			r.already == 1 ? "the one reminder is"
						   : std::format("all {} reminders are", r.already));
		return 0;
	}
	if (g_.json) {
		std::cout
			<< std::format(
				   R"({{"list":{},"created":{},"added":{},"already":{},"skipped":{}}})",
				   json_escape(store_.key_of(*list)), created, r.added,
				   r.already, imp.skipped)
			<< "\n";
		return 0;
	}
	auto plural = [](int n, std::string_view one, std::string_view many) {
		return std::format("{} {}", n, n == 1 ? one : many);
	};
	std::cout << std::format("Imported {} ({}) into {}{}",
							 plural(r.added, "reminder", "reminders"),
							 rem::kind_name(imp.kind), store_.label(*list),
							 created ? " (a new list)" : "");
	std::vector<std::string> notes;
	if (r.already) {
		notes.push_back(
			plural(r.already, "was already there", "were already there"));
	}
	if (imp.skipped) {
		notes.push_back(plural(imp.skipped, "event or other item skipped",
							   "events or other items skipped"));
	}
	for (std::size_t i = 0; i < notes.size(); ++i) {
		std::cout << (i ? ", " : "; ") << notes[i];
	}
	std::cout << "\n";
	return 0;
}

int App::cmd_export(const Args& a) {
	auto out = a.get("output");
	std::optional<rem::ExportFormat> format;
	if (auto f = a.get("format")) {
		format = rem::export_format(*f);
		if (!format) {
			throw UsageError("--format is md, txt, todo.txt, csv or ics");
		}
	} else if (out && !a.positional.empty()) {
		format = rem::export_format_for(*out);
	}
	auto fmt = format.value_or(rem::ExportFormat::Markdown);
	rem::ExportOptions options;
	options.completed = a.has("all");

	// Every list, into a folder.
	if (a.positional.empty()) {
		if (!out || *out == "-") {
			throw UsageError(
				"usage: reminders export [LIST] [--format F] [-o FILE]; "
				"without LIST, -o names a folder");
		}
		auto folder = folder_arg(*out);
		if (term::lower(folder.extension().string()) == ".zip") {
			std::ofstream file(folder, std::ios::binary | std::ios::trunc);
			file << rem::export_zip(store_, store_.lists(), fmt, options);
			file.close();
			if (!file) {
				throw std::runtime_error(
					std::format("couldn't write {}", folder.string()));
			}
			auto n = store_.lists().size();
			if (g_.json) {
				std::cout << std::format(
								 R"({{"format":"{}","archive":{},"lists":{}}})",
								 rem::export_extension(fmt),
								 json_escape(folder.string()), n)
						  << "\n";
			} else {
				std::cout << std::format("Exported {} {} to {}\n", n,
										 n == 1 ? "list" : "lists",
										 folder.string());
			}
			return 0;
		}
		if (std::filesystem::exists(folder) &&
			!std::filesystem::is_directory(folder)) {
			throw std::runtime_error(
				std::format("{} isn't a folder", folder.string()));
		}
		auto files = rem::export_all(store_, folder, fmt, options);
		if (g_.json) {
			std::string list;
			for (auto& f : files) {
				list += (list.empty() ? "" : ",") + json_escape(f.string());
			}
			std::cout << std::format(R"({{"format":"{}","files":[{}]}})",
									 rem::export_extension(fmt), list)
					  << "\n";
		} else {
			std::cout << std::format("Exported {} {} to {}\n", files.size(),
									 files.size() == 1 ? "list" : "lists",
									 folder.string());
		}
		return 0;
	}
	auto& list = list_named(join(a.positional));
	auto text = rem::export_list(list, fmt, options);
	if (!out || *out == "-") {
		std::cout << text;
		return 0;
	}
	auto path = folder_arg(*out);
	if (std::filesystem::is_directory(path)) {
		path /= std::format("{}.{}", list.name, rem::export_extension(fmt));
	}
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << text;
	file.close();
	if (!file) {
		throw std::runtime_error(
			std::format("couldn't write {}", path.string()));
	}
	if (g_.json) {
		std::cout << std::format(R"({{"list":{},"format":"{}","file":{}}})",
								 json_escape(store_.key_of(list)),
								 rem::export_extension(fmt),
								 json_escape(path.string()))
				  << "\n";
	} else {
		std::cout << std::format("Exported {} to {}\n", store_.label(list),
								 path.string());
	}
	return 0;
}

}  // namespace cli
