#include "internal.hpp"

namespace cli {

int App::cmd_import(const Args& a) {
	if (a.positional.size() != 1) {
		throw UsageError(
			"usage: reminders import FILE [--list LIST] [--source SOURCE] "
			"[--format F] [--duplicates]");
	}
	auto path = folder_arg(a.positional[0]);
	std::optional<rem::Import::Kind> kind;
	if (auto f = a.get("format")) {
		auto format = rem::export_format(*f);
		if (!format) {
			throw UsageError("--format is md, txt, todo.txt, csv or ics");
		}
		static constexpr rem::Import::Kind kKinds[] = {
			rem::Import::Kind::Markdown, rem::Import::Kind::Text,
			rem::Import::Kind::Ics, rem::Import::Kind::Todotxt,
			rem::Import::Kind::Csv};
		kind = kKinds[static_cast<int>(*format)];
	}
	auto imp = rem::read_import_file(path, kind);

	// The list: --list (a name, or source/name), else one named after the
	// calendar (or the file); made in --source (or the default source) if
	// there's none.
	auto name = a.get("list").value_or("");
	if (name.empty()) {
		name = rem::import_list_name(imp, path);
	}
	rem::ListFile* list = nullptr;
	auto source = a.get("source");
	if (source && !store_.store(*source)) {
		throw std::runtime_error(std::format("no source called “{}”", *source));
	}
	auto found = rem::lists_called(store_, name, source.value_or(""));
	if (found.size() == 1) {
		list = found.front();
	} else if (found.size() > 1) {
		list = &list_named(name);  // says which to choose
	}
	if (!list && !rem::list_name_error(name).empty()) {
		throw UsageError(std::format(
			"“{}” can't be a list name: choose one with --list", name));
	}
	auto done =
		rem::import_to(store_, list, source.value_or(store_.default_source()),
					   name, imp, a.has("duplicates"));
	if (g_.json) {
		if (done.key.empty()) {
			out_
				<< std::format(
					   R"({{"list":null,"created":false,"added":0,"already":{},"skipped":{}}})",
					   done.result.already, imp.skipped)
				<< "\n";
			return 0;
		}
		out_
			<< std::format(
				   R"({{"list":{},"created":{},"added":{},"already":{},"skipped":{}}})",
				   json_escape(done.key), done.created, done.result.added,
				   done.result.already, imp.skipped)
			<< "\n";
		return 0;
	}
	out_ << rem::import_summary(store_, done, imp) << "\n";
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
				out_ << std::format(
							R"({{"format":"{}","archive":{},"lists":{}}})",
							rem::export_extension(fmt),
							json_escape(folder.string()), n)
					 << "\n";
			} else {
				out_ << std::format("Exported {} {} to {}\n", n,
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
			out_ << std::format(R"({{"format":"{}","files":[{}]}})",
								rem::export_extension(fmt), list)
				 << "\n";
		} else {
			out_ << std::format("Exported {} {} to {}\n", files.size(),
								files.size() == 1 ? "list" : "lists",
								folder.string());
		}
		return 0;
	}
	auto& list = list_named(join(a.positional));
	auto text = rem::export_list(list, fmt, options);
	if (!out || *out == "-") {
		out_ << text;
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
		out_ << std::format(R"({{"list":{},"format":"{}","file":{}}})",
							json_escape(store_.key_of(list)),
							rem::export_extension(fmt),
							json_escape(path.string()))
			 << "\n";
	} else {
		out_ << std::format("Exported {} to {}\n", store_.label(list),
							path.string());
	}
	return 0;
}

}  // namespace cli
