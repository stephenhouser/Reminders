#include "reminders/command_line.hpp"

#include <format>
#include <stdexcept>

namespace rem {

std::vector<CommandWord> split_words(std::string_view line, bool partial) {
	std::vector<CommandWord> words;
	std::size_t i = 0;
	while (i < line.size()) {
		if (line[i] == ' ' || line[i] == '\t') {
			++i;
			continue;
		}
		CommandWord w;
		w.begin = i;
		char quote = 0;
		for (; i < line.size(); ++i) {
			char c = line[i];
			if (c == '\\' && quote != '\'') {
				if (i + 1 < line.size()) {
					w.text += line[++i];
				}
				continue;
			}
			if (quote) {
				if (c == quote) {
					quote = 0;
				} else {
					w.text += c;
				}
			} else if (c == '\'' || c == '"') {
				quote = c;
			} else if (c == ' ' || c == '\t') {
				break;
			} else {
				w.text += c;
			}
		}
		if (quote && !partial) {
			throw std::runtime_error(
				std::format("no closing {} quote", quote == '"' ? "\"" : "'"));
		}
		w.end = i;
		words.push_back(std::move(w));
	}
	return words;
}

std::vector<std::string> split_command_line(std::string_view line) {
	std::vector<std::string> out;
	for (auto& w : split_words(line)) {
		out.push_back(std::move(w.text));
	}
	return out;
}

std::string quote_word(std::string_view word) {
	if (word.empty()) {
		return "\"\"";
	}
	if (word.find_first_of(" \t'\"\\") == std::string_view::npos) {
		return std::string(word);
	}
	std::string out = "\"";
	for (char c : word) {
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		out += c;
	}
	return out + "\"";
}

}  // namespace rem
