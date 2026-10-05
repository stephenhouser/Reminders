#include <ncurses.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <cwchar>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../editfile.hpp"
#include "../text.hpp"
#include "internal.hpp"
#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/history.hpp"
#include "reminders/settings.hpp"
#include "reminders/sync_runner.hpp"

namespace tui {

std::wstring widen(std::string_view s) {
	std::wstring out;
	std::mbstate_t st{};
	const char* p = s.data();
	const char* end = s.data() + s.size();
	while (p < end) {
		wchar_t wc;
		auto n = std::mbrtowc(&wc, p, static_cast<std::size_t>(end - p), &st);
		if (n == static_cast<std::size_t>(-1) ||
			n == static_cast<std::size_t>(-2)) {
			wc = L'?';
			n = 1;
			st = {};
		} else if (n == 0) {
			n = 1;
		}
		if (wc != 0xFE0F) {
			out += wc;	// variation selectors confuse terminals' widths
		}
		p += n;
	}
	return out;
}

std::string narrow(const std::wstring& w) {
	std::string out;
	std::mbstate_t st{};
	char buf[MB_LEN_MAX];
	for (wchar_t wc : w) {
		auto n = std::wcrtomb(buf, wc, &st);
		if (n != static_cast<std::size_t>(-1)) {
			out.append(buf, n);
		}
	}
	return out;
}

int cell_width(wchar_t c) {
	int w = wcwidth(c);
	return w < 0 ? 1 : w;
}

int text_width(const std::wstring& w) {
	int n = 0;
	for (auto c : w) {
		n += cell_width(c);
	}
	return n;
}

// Draws `s` at (y, x) in at most `width` cells, ending in "…" if cut. Returns
// the cells used.
int put(int y, int x, std::string_view s, int width) {
	if (width <= 0) {
		return 0;
	}
	auto w = widen(s);
	std::wstring out;
	int used = 0;
	for (auto c : w) {
		int cw = cell_width(c);
		if (used + cw > width) {
			while (!out.empty() && used + 1 > width) {
				used -= cell_width(out.back());
				out.pop_back();
			}
			out += L'…';
			++used;
			break;
		}
		out += c;
		used += cw;
	}
	mvaddnwstr(y, x, out.c_str(), static_cast<int>(out.size()));
	return used;
}

// Nearest xterm-256 colour to an RGB value (from the 6×6×6 cube).
short xterm256(term::Rgb c) {
	auto level = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
	return static_cast<short>(16 + 36 * level(c.r) + 6 * level(c.g) +
							  level(c.b));
}

std::map<std::string, short> g_color_pairs;

void setup_colors() {
	if (!has_colors()) {
		return;
	}
	start_color();
	use_default_colors();
	init_pair(kDim, COLORS >= 256 ? 244 : COLOR_WHITE, -1);
	init_pair(kRed, COLOR_RED, -1);
	init_pair(kSelected, -1, COLORS >= 256 ? 237 : COLOR_BLUE);
	init_pair(kHeading, -1, -1);
	init_pair(kStatus, COLORS >= 256 ? 250 : COLOR_WHITE,
			  COLORS >= 256 ? 236 : COLOR_BLACK);
	init_pair(kMarked, COLORS >= 256 ? 75 : COLOR_CYAN,
			  -1);	// the * beside a marked reminder
	short next = kFirstListColor;
	for (auto c : rem::kColors) {
		auto rgb = term::color_rgb(c);
		init_pair(next, COLORS >= 256 ? xterm256(rgb) : COLOR_CYAN, -1);
		g_color_pairs[std::string(c)] = next++;
	}
}

attr_t list_color(const std::string& color) {
	if (!has_colors()) {
		return A_BOLD;
	}
	auto it = g_color_pairs.find(color);
	return it == g_color_pairs.end() ? 0 : COLOR_PAIR(it->second);
}

attr_t dim() { return has_colors() ? COLOR_PAIR(kDim) : A_DIM; }

}  // namespace tui
