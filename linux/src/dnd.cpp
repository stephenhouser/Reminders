#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#include "dialogs.hpp"
#include "reminders/clipboard.hpp"
#include "reminders/exporter.hpp"
#include "reminders/format.hpp"
#include "reminders/importer.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "reminders/syncthing.hpp"
#include "support.hpp"
#include "window_internal.hpp"

namespace ui::detail {

GType reminder_drag_type() {
	static GType type = g_boxed_type_register_static(
		"RemindersReminderIds",
		[](gpointer p) -> gpointer { return new Ids(*static_cast<Ids*>(p)); },
		[](gpointer p) { delete static_cast<Ids*>(p); });
	return type;
}

const Ids* dragged_ids(const GValue* value) {
	if (!value || !G_VALUE_HOLDS(value, reminder_drag_type())) {
		return nullptr;
	}
	return static_cast<const Ids*>(g_value_get_boxed(value));
}

// The widget a controller is attached to; null once that widget is gone
// (a drop rebuilds the view, destroying the dragged row before "drag-end").
GtkWidget* owner(gpointer controller) {
	return gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
}

// `image` with a count badge in its top left corner, for dragging several.
Obj<GdkPaintable> with_badge(GtkWidget* widget, GdkPaintable* image,
							 std::size_t count) {
	int w = gdk_paintable_get_intrinsic_width(image),
		h = gdk_paintable_get_intrinsic_height(image);
	auto* snap = gtk_snapshot_new();
	gdk_paintable_snapshot(image, snap, w, h);
	auto layout = Obj<PangoLayout>::adopt(
		gtk_widget_create_pango_layout(widget, std::to_string(count).c_str()));
	int lw = 0, lh = 0;
	pango_layout_get_pixel_size(layout.get(), &lw, &lh);
	float bh = static_cast<float>(lh + 4),
		  bw = std::max(static_cast<float>(lw + 12), bh);
	graphene_rect_t r = GRAPHENE_RECT_INIT(4, 4, bw, bh);
	GskRoundedRect round;
	gsk_rounded_rect_init_from_rect(&round, &r, bh / 2);
	gtk_snapshot_push_rounded_clip(snap, &round);
	auto* accent = adw_style_manager_get_accent_color_rgba(
		adw_style_manager_get_default());
	gtk_snapshot_append_color(snap, accent, &r);
	gdk_rgba_free(accent);
	gtk_snapshot_pop(snap);
	gtk_snapshot_save(snap);
	graphene_point_t at =
		GRAPHENE_POINT_INIT(4 + (bw - static_cast<float>(lw)) / 2, 6);
	gtk_snapshot_translate(snap, &at);
	GdkRGBA white{1, 1, 1, 1};
	gtk_snapshot_append_layout(snap, layout.get(), &white);
	gtk_snapshot_restore(snap);
	graphene_size_t size =
		GRAPHENE_SIZE_INIT(static_cast<float>(w), static_cast<float>(h));
	return Obj<GdkPaintable>::adopt(
		gtk_snapshot_free_to_paintable(snap, &size));
}

// Rows carry their reminders' ids when dragged: `ids` gives them when the
// drag starts (the row's own, or the selection it's part of), and `mark`
// fades those rows while they're dragged (false: no longer). Other apps get
// `text` of them (the Markdown Copy makes) and copy it; within the app they
// move.
void make_draggable(GtkWidget* row, std::function<Ids()> ids,
					std::function<void(const Ids&, bool)> mark,
					std::function<std::string(const Ids&)> text) {
	auto* source = gtk_drag_source_new();
	gtk_drag_source_set_actions(
		source, GdkDragAction(GDK_ACTION_MOVE | GDK_ACTION_COPY));
	auto dragging = std::make_shared<Ids>();
	connect<GdkContentProvider*(GtkDragSource*, double, double)>(
		source, "prepare",
		[ids, dragging, text](GtkDragSource*, double, double) {
			*dragging = ids();
			GValue value = G_VALUE_INIT;
			g_value_init(&value, reminder_drag_type());
			g_value_set_boxed(&value, dragging.get());
			auto* own = gdk_content_provider_new_for_value(&value);
			g_value_unset(&value);
			auto markdown = text(*dragging);
			auto* plain =
				gdk_content_provider_new_typed(G_TYPE_STRING, markdown.c_str());
			GdkContentProvider* both[] = {own, plain};
			return gdk_content_provider_new_union(both, 2);
		});
	connect<void(GtkDragSource*, GdkDrag*)>(
		source, "drag-begin", [dragging, mark](GtkDragSource* s, GdkDrag*) {
			auto* row = owner(s);
			if (!row) {
				return;
			}
			// A still image of the row (the rows themselves fade while
			// dragged).
			auto live = Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(row));
			auto still = Obj<GdkPaintable>::adopt(
				gdk_paintable_get_current_image(live.get()));
			if (dragging->size() > 1) {
				still = with_badge(row, still.get(), dragging->size());
			}
			gtk_drag_source_set_icon(s, still.get(), 24, 20);
			gtk_widget_add_css_class(row, "dragging");
			mark(*dragging, true);
		});
	connect<void(GtkDragSource*, GdkDrag*, gboolean)>(
		source, "drag-end",
		[dragging, mark](GtkDragSource* s, GdkDrag*, gboolean) {
			if (auto* row = owner(s)) {
				gtk_widget_remove_css_class(row, "dragging");
			}
			mark(*dragging, false);
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
}

// Accepts dropped reminders on `row`, showing where they'd land.
// `self` is the row's own reminder id (it can't be dropped on itself).
void make_drop_target(GtkWidget* row, DropStyle style, std::string self,
					  std::function<void(Ids, rem::Document::Place)> on_drop) {
	auto* target = gtk_drop_target_new(reminder_drag_type(), GDK_ACTION_MOVE);
	gtk_drop_target_set_preload(target, TRUE);
	// Before the row's children: a dragged reminder also carries text, which
	// a text field in the row (New Reminder, a title being edited) would
	// otherwise take.
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(target),
											   GTK_PHASE_CAPTURE);
	auto place_at = [style](GtkWidget* w, double y) {
		if (style == DropStyle::Halves && y > gtk_widget_get_height(w) / 2.0) {
			return rem::Document::Place::After;
		}
		return rem::Document::Place::Before;
	};
	auto clear = [](GtkWidget* w) {
		if (!w) {
			return;
		}
		for (auto* c : {"drop-above", "drop-below", "drop-into"}) {
			gtk_widget_remove_css_class(w, c);
		}
	};
	connect<GdkDragAction(GtkDropTarget*, double, double)>(
		target, "motion",
		[style, self, place_at, clear](GtkDropTarget* t, double, double y) {
			auto* w = owner(t);
			if (!w) {
				return GdkDragAction(0);
			}
			clear(w);
			auto* ids = dragged_ids(gtk_drop_target_get_value(t));
			if (ids && std::ranges::find(*ids, self) != ids->end()) {
				return GdkDragAction(0);
			}
			if (style == DropStyle::Into) {
				gtk_widget_add_css_class(w, "drop-into");
			} else if (place_at(w, y) == rem::Document::Place::Before) {
				gtk_widget_add_css_class(w, "drop-above");
			} else {
				gtk_widget_add_css_class(w, "drop-below");
			}
			return GDK_ACTION_MOVE;
		});
	connect<void(GtkDropTarget*)>(
		target, "leave", [clear](GtkDropTarget* t) { clear(owner(t)); });
	connect<gboolean(GtkDropTarget*, const GValue*, double, double)>(
		target, "drop",
		[self, place_at, clear, on_drop](GtkDropTarget* t, const GValue* value,
										 double, double y) -> gboolean {
			auto* w = owner(t);
			clear(w);
			auto* ids = dragged_ids(value);
			if (!w || !ids || ids->empty() ||
				std::ranges::find(*ids, self) != ids->end()) {
				return FALSE;
			}
			auto place = place_at(w, y);
			// Rebuilding the view destroys this row; do it after the drop
			// finishes.
			idle([on_drop, ids = *ids, place] { on_drop(ids, place); });
			return TRUE;
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}

// The local files in a drop, if it holds files.
std::vector<std::filesystem::path> dropped_files(const GValue* value) {
	std::vector<std::filesystem::path> out;
	if (!value || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
		return out;
	}
	auto* list = static_cast<GdkFileList*>(g_value_get_boxed(value));
	auto* files = gdk_file_list_get_files(list);
	for (auto* f = files; f; f = f->next) {
		if (auto path = take_string(g_file_get_path(G_FILE(f->data)));
			!path.empty()) {
			out.emplace_back(path);
		}
	}
	g_slist_free(files);
	return out;
}

// Accepts files dropped on `widget`; with `highlight`, the widget shows it
// while they're over it (a sidebar list).
void make_file_drop_target(
	GtkWidget* widget, bool highlight,
	std::function<void(std::vector<std::filesystem::path>)> on_drop) {
	auto* target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	connect<gboolean(GtkDropTarget*, GdkDrop*)>(
		target, "accept", [](GtkDropTarget*, GdkDrop* drop) -> gboolean {
			return is_file_drop(drop);	// a browser's link is text
		});
	if (highlight) {
		connect<GdkDragAction(GtkDropTarget*, double, double)>(
			target, "motion", [](GtkDropTarget* t, double, double) {
				if (auto* w = owner(t)) {
					gtk_widget_add_css_class(w, "drop-into");
				}
				return GDK_ACTION_COPY;
			});
		connect<void(GtkDropTarget*)>(target, "leave", [](GtkDropTarget* t) {
			if (auto* w = owner(t)) {
				gtk_widget_remove_css_class(w, "drop-into");
			}
		});
	}
	connect<gboolean(GtkDropTarget*, const GValue*, double, double)>(
		target, "drop",
		[on_drop](GtkDropTarget* t, const GValue* value, double,
				  double) -> gboolean {
			if (auto* w = owner(t)) {
				gtk_widget_remove_css_class(w, "drop-into");
			}
			auto files = dropped_files(value);
			if (files.empty()) {
				return FALSE;
			}
			// The dialog opens after the drop has finished.
			idle([on_drop, files = std::move(files)] { on_drop(files); });
			return TRUE;
		});
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(target));
}

// Sidebar entries carry their View when dragged, to be put in another place
// in their group.
GType entry_drag_type() {
	static GType type = g_boxed_type_register_static(
		"RemSidebarEntry",
		[](gpointer p) -> gpointer { return new View(*static_cast<View*>(p)); },
		[](gpointer p) { delete static_cast<View*>(p); });
	return type;
}

const View* dragged_entry(const GValue* value) {
	if (!value || !G_VALUE_HOLDS(value, entry_drag_type())) {
		return nullptr;
	}
	return static_cast<const View*>(g_value_get_boxed(value));
}

void make_entry_draggable(GtkWidget* row, const View& view) {
	auto* source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
	connect<GdkContentProvider*(GtkDragSource*, double, double)>(
		source, "prepare", [view](GtkDragSource*, double, double) {
			GValue value = G_VALUE_INIT;
			g_value_init(&value, entry_drag_type());
			g_value_set_boxed(&value, &view);
			auto* provider = gdk_content_provider_new_for_value(&value);
			g_value_unset(&value);
			return provider;
		});
	connect<void(GtkDragSource*, GdkDrag*)>(
		source, "drag-begin", [](GtkDragSource* s, GdkDrag*) {
			auto* row = owner(s);
			if (!row) {
				return;
			}
			auto live = Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(row));
			auto still = Obj<GdkPaintable>::adopt(
				gdk_paintable_get_current_image(live.get()));
			gtk_drag_source_set_icon(s, still.get(), 24, 16);
			gtk_widget_add_css_class(row, "dragging");
		});
	connect<void(GtkDragSource*, GdkDrag*, gboolean)>(
		source, "drag-end", [](GtkDragSource* s, GdkDrag*, gboolean) {
			if (auto* row = owner(s)) {
				gtk_widget_remove_css_class(row, "dragging");
			}
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
}

// Accepts entries dropped on `row` when `accepts`
// says they can go next to it, showing a line above or below.
void make_entry_drop_target(GtkWidget* row,
							std::function<bool(const View&)> accepts,
							std::function<void(View, bool)> on_drop) {
	auto* target = gtk_drop_target_new(entry_drag_type(), GDK_ACTION_MOVE);
	gtk_drop_target_set_preload(target, TRUE);
	auto below = [](GtkWidget* w, double y) {
		return y > gtk_widget_get_height(w) / 2.0;
	};
	auto clear = [](GtkWidget* w) {
		if (!w) {
			return;
		}
		for (auto* c : {"drop-above", "drop-below"}) {
			gtk_widget_remove_css_class(w, c);
		}
	};
	connect<GdkDragAction(GtkDropTarget*, double, double)>(
		target, "motion",
		[accepts, below, clear](GtkDropTarget* t, double, double y) {
			auto* w = owner(t);
			if (!w) {
				return GdkDragAction(0);
			}
			clear(w);
			auto* v = dragged_entry(gtk_drop_target_get_value(t));
			if (!v || !accepts(*v)) {
				return GdkDragAction(0);
			}
			gtk_widget_add_css_class(w,
									 below(w, y) ? "drop-below" : "drop-above");
			return GDK_ACTION_MOVE;
		});
	connect<void(GtkDropTarget*)>(
		target, "leave", [clear](GtkDropTarget* t) { clear(owner(t)); });
	connect<gboolean(GtkDropTarget*, const GValue*, double, double)>(
		target, "drop",
		[accepts, below, clear, on_drop](GtkDropTarget* t, const GValue* value,
										 double, double y) -> gboolean {
			auto* w = owner(t);
			clear(w);
			auto* v = dragged_entry(value);
			if (!w || !v || !accepts(*v)) {
				return FALSE;
			}
			// Rebuilding the sidebar destroys this row; do it after the drop
			// finishes.
			idle([on_drop, v = *v, after = below(w, y)] { on_drop(v, after); });
			return TRUE;
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}

// Groups are dragged by their heading and dropped on any row of another
// group: above it when over the group's top half, below it when over the
// bottom half. The rows carry their group (set_row_group).
GType group_drag_type() {
	static GType type = g_boxed_type_register_static(
		"RemSidebarGroup",
		[](gpointer p) -> gpointer {
			return new rem::SidebarGroup(*static_cast<rem::SidebarGroup*>(p));
		},
		[](gpointer p) { delete static_cast<rem::SidebarGroup*>(p); });
	return type;
}

const rem::SidebarGroup* dragged_group(const GValue* value) {
	if (!value || !G_VALUE_HOLDS(value, group_drag_type())) {
		return nullptr;
	}
	return static_cast<const rem::SidebarGroup*>(g_value_get_boxed(value));
}

// The rows of `group` in the list box holding `row`, top to bottom.
std::vector<GtkWidget*> group_rows(GtkWidget* row,
								   const rem::SidebarGroup& group) {
	std::vector<GtkWidget*> out;
	auto* list = row ? gtk_widget_get_parent(row) : nullptr;
	if (!list || !GTK_IS_LIST_BOX(list)) {
		return out;
	}
	for (int i = 0;; ++i) {
		auto* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i);
		if (!r) {
			break;
		}
		if (row_group(r) == group) {
			out.push_back(GTK_WIDGET(r));
		}
	}
	return out;
}

void make_group_draggable(GtkWidget* heading, const rem::SidebarGroup& group) {
	auto* source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
	connect<GdkContentProvider*(GtkDragSource*, double, double)>(
		source, "prepare", [group](GtkDragSource*, double, double) {
			GValue value = G_VALUE_INIT;
			g_value_init(&value, group_drag_type());
			g_value_set_boxed(&value, &group);
			auto* provider = gdk_content_provider_new_for_value(&value);
			g_value_unset(&value);
			return provider;
		});
	connect<void(GtkDragSource*, GdkDrag*)>(
		source, "drag-begin", [group](GtkDragSource* s, GdkDrag*) {
			auto* row = owner(s);
			if (!row) {
				return;
			}
			auto live = Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(row));
			auto still = Obj<GdkPaintable>::adopt(
				gdk_paintable_get_current_image(live.get()));
			gtk_drag_source_set_icon(s, still.get(), 24, 12);
			for (auto* r : group_rows(row, group)) {
				gtk_widget_add_css_class(r, "dragging");
			}
		});
	connect<void(GtkDragSource*, GdkDrag*, gboolean)>(
		source, "drag-end", [group](GtkDragSource* s, GdkDrag*, gboolean) {
			for (auto* r : group_rows(owner(s), group)) {
				gtk_widget_remove_css_class(r, "dragging");
			}
		});
	gtk_widget_add_controller(heading, GTK_EVENT_CONTROLLER(source));
}

// Accepts groups dropped on `row`, a row of `group`, showing a line above
// the group's first row or below its last.
void make_group_drop_target(
	GtkWidget* row, const rem::SidebarGroup& group,
	std::function<void(rem::SidebarGroup, bool)> on_drop) {
	auto* target = gtk_drop_target_new(group_drag_type(), GDK_ACTION_MOVE);
	gtk_drop_target_set_preload(target, TRUE);
	// Below when the pointer is over the bottom half of the group's rows.
	auto below = [group](GtkWidget* w, double y) {
		auto rows = group_rows(w, group);
		auto at = std::ranges::find(rows, w);
		if (rows.empty() || at == rows.end()) {
			return false;
		}
		double h = std::max(1, gtk_widget_get_height(w));
		return (static_cast<double>(at - rows.begin()) + y / h) /
				   static_cast<double>(rows.size()) >=
			   0.5;
	};
	auto clear = [group](GtkWidget* w) {
		for (auto* r : group_rows(w, group)) {
			for (auto* c : {"drop-above", "drop-below"}) {
				gtk_widget_remove_css_class(r, c);
			}
		}
	};
	connect<GdkDragAction(GtkDropTarget*, double, double)>(
		target, "motion",
		[group, below, clear](GtkDropTarget* t, double, double y) {
			auto* w = owner(t);
			if (!w) {
				return GdkDragAction(0);
			}
			clear(w);
			auto* g = dragged_group(gtk_drop_target_get_value(t));
			if (!g || *g == group) {
				return GdkDragAction(0);
			}
			auto rows = group_rows(w, group);
			if (rows.empty()) {
				return GdkDragAction(0);
			}
			if (below(w, y)) {
				gtk_widget_add_css_class(rows.back(), "drop-below");
			} else {
				gtk_widget_add_css_class(rows.front(), "drop-above");
			}
			return GDK_ACTION_MOVE;
		});
	connect<void(GtkDropTarget*)>(
		target, "leave", [clear](GtkDropTarget* t) { clear(owner(t)); });
	connect<gboolean(GtkDropTarget*, const GValue*, double, double)>(
		target, "drop",
		[group, below, clear, on_drop](GtkDropTarget* t, const GValue* value,
									   double, double y) -> gboolean {
			auto* w = owner(t);
			if (!w) {
				return FALSE;
			}
			clear(w);
			auto* g = dragged_group(value);
			if (!g || *g == group) {
				return FALSE;
			}
			// Rebuilding the sidebar destroys this row; do it after the drop
			// finishes.
			idle([on_drop, g = *g, after = below(w, y)] { on_drop(g, after); });
			return TRUE;
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}

// Whether a drop offers `type` (directly, as drags within the app do, or
// as a format another app's data can be read as).
bool drop_offers(GdkDrop* drop, GType type) {
	auto* formats = gdk_content_formats_union_deserialize_gtypes(
		gdk_content_formats_ref(gdk_drop_get_formats(drop)));
	bool yes = gdk_content_formats_contain_gtype(formats, type);
	gdk_content_formats_unref(formats);
	return yes;
}

// REMINDERS_DEBUG_DND=1: report what other apps' drops offer and what was
// read from them, on stderr.
bool dnd_debug() {
	static bool on = g_getenv("REMINDERS_DEBUG_DND") != nullptr;
	return on;
}

// The formats text is read from when another app drops it, best first.
// Browsers offer several, some in UTF-16.
constexpr const char* kTextMimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING",	 "text/plain", "STRING", "TEXT",
	"text/x-moz-url",			"text/uri-list", "text/html"};

std::string from_utf16(std::string_view data) {
	if (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0xFF &&
		static_cast<unsigned char>(data[1]) == 0xFE) {
		data.remove_prefix(
			2);	 // the byte-order mark (little-endian, as browsers send)
	}
	std::u16string units(data.size() / 2, u'\0');
	std::memcpy(units.data(), data.data(), units.size() * 2);
	auto* utf8 = g_utf16_to_utf8(
		reinterpret_cast<const gunichar2*>(units.data()),
		static_cast<glong>(units.size()), nullptr, nullptr, nullptr);
	return take_string(utf8);
}

// Text in UTF-8 from a drop's bytes: UTF-16 when it looks like it (a
// byte-order mark, or a zero byte in every other place), Latin-1 when it
// isn't valid UTF-8.
std::string decode_text(std::string data) {
	auto looks_utf16 =
		data.size() >= 2 && ((static_cast<unsigned char>(data[0]) == 0xFF &&
							  static_cast<unsigned char>(data[1]) == 0xFE) ||
							 (data[1] == '\0' && data[0] != '\0'));
	if (looks_utf16) {
		data = from_utf16(data);
	}
	while (!data.empty() && data.back() == '\0') {
		data.pop_back();
	}
	if (!g_utf8_validate(data.data(), static_cast<gssize>(data.size()),
						 nullptr)) {
		data = take_string(g_convert(data.data(),
									 static_cast<gssize>(data.size()), "UTF-8",
									 "ISO-8859-1", nullptr, nullptr, nullptr));
	}
	return data;
}

// HTML as plain text: line breaks for block ends, tags dropped, the common
// entities decoded.
std::string html_text(const std::string& html) {
	std::string out;
	for (std::size_t i = 0; i < html.size();) {
		if (html[i] == '<') {
			auto end = html.find('>', i);
			if (end == std::string::npos) {
				break;
			}
			auto tag = html.substr(i + 1, end - i - 1);
			for (auto& c : tag) {
				c = static_cast<char>(g_ascii_tolower(c));
			}
			for (auto* block :
				 {"br", "/p", "/li", "/div", "/h1", "/h2", "/h3", "/tr"}) {
				if (tag.starts_with(block)) {
					out += '\n';
				}
			}
			i = end + 1;
		} else if (html[i] == '&') {
			auto end = html.find(';', i);
			auto name = end == std::string::npos
						  ? std::string()
						  : html.substr(i + 1, end - i - 1);
			const char* as = name == "amp"					 ? "&"
						   : name == "lt"					 ? "<"
						   : name == "gt"					 ? ">"
						   : name == "quot"					 ? "\""
						   : name == "#39" || name == "apos" ? "'"
						   : name == "nbsp"					 ? " "
															 : nullptr;
			if (as) {
				out += as;
				i = end + 1;
			} else {
				out += html[i++];
			}
		} else {
			out += html[i++];
		}
	}
	return out;
}

// The text in a drop's data of type `mime`, or "" if there's none.
std::string text_from(std::string_view mime, std::string data) {
	auto text = decode_text(std::move(data));
	if (mime == "text/x-moz-url") {	 // the address, then the page's title
		auto nl = text.find('\n');
		auto url = trim(text.substr(0, nl)),
			 title = nl == std::string::npos ? std::string()
											 : trim(text.substr(nl + 1));
		return title.empty() ? url : title + " " + url;
	}
	if (mime == "text/uri-list") {	// one address a line; # starts a comment
		std::string out;
		std::istringstream in(text);
		for (std::string line; std::getline(in, line);) {
			if (auto t = trim(line); !t.empty() && t[0] != '#') {
				out += t + "\n";
			}
		}
		return out;
	}
	if (mime == "text/html") {
		return html_text(text);
	}
	return text;
}

// Reads the text of another app's drop, trying its formats in turn (some
// apps offer a format they then can't deliver), then calls `done` with it,
// or with nullopt if none gave any.
struct TextDropRead {
		Obj<GdkDrop> drop;
		std::vector<std::string> mimes;
		std::size_t next = 0;
		Obj<GOutputStream> sink;
		std::function<void(std::optional<std::string>)> done;
};

void read_next_text(TextDropRead* job) {
	if (job->next >= job->mimes.size()) {
		gdk_drop_finish(job->drop.get(), GdkDragAction(0));
		auto done = std::move(job->done);
		delete job;
		done(std::nullopt);
		return;
	}
	const char* types[] = {job->mimes[job->next].c_str(), nullptr};
	gdk_drop_read_async(
		job->drop.get(), types, G_PRIORITY_DEFAULT, nullptr,
		[](GObject* source, GAsyncResult* result, gpointer data) {
			auto* job = static_cast<TextDropRead*>(data);
			GError* error = nullptr;
			auto* in =
				gdk_drop_read_finish(GDK_DROP(source), result, nullptr, &error);
			if (!in) {
				if (dnd_debug()) {
					g_printerr("drop: %s unreadable: %s\n",
							   job->mimes[job->next].c_str(), error->message);
				}
				g_clear_error(&error);
				++job->next;
				return read_next_text(job);
			}
			job->sink = Obj<GOutputStream>::adopt(
				g_memory_output_stream_new_resizable());
			g_output_stream_splice_async(
				job->sink.get(), in,
				GOutputStreamSpliceFlags(G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
										 G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
				G_PRIORITY_DEFAULT, nullptr,
				[](GObject* sink, GAsyncResult* result, gpointer data) {
					auto* job = static_cast<TextDropRead*>(data);
					auto& mime = job->mimes[job->next];
					std::string text;
					if (g_output_stream_splice_finish(G_OUTPUT_STREAM(sink),
													  result, nullptr) >= 0) {
						auto* bytes = g_memory_output_stream_steal_as_bytes(
							G_MEMORY_OUTPUT_STREAM(sink));
						gsize n = 0;
						auto* p = static_cast<const char*>(
							g_bytes_get_data(bytes, &n));
						text = text_from(mime, std::string(p ? p : "", n));
						g_bytes_unref(bytes);
						if (dnd_debug()) {
							g_printerr("drop: read %zu bytes of %s: “%s”\n", n,
									   mime.c_str(), text.c_str());
						}
					}
					if (trim(text).empty()) {
						++job->next;
						return read_next_text(job);
					}
					gdk_drop_finish(job->drop.get(), GDK_ACTION_COPY);
					auto done = std::move(job->done);
					delete job;
					done(std::move(text));
				},
				job);
			g_object_unref(in);
		},
		job);
}

// A file manager's drop (imported by make_file_drop_target), as against a
// browser's, which offers links as a URI list too.
bool is_file_drop(GdkDrop* drop) {
	auto* formats = gdk_drop_get_formats(drop);
	return drop_offers(drop, GDK_TYPE_FILE_LIST) &&
		   !gdk_content_formats_contain_mime_type(formats, "text/x-moz-url") &&
		   !gdk_content_formats_contain_mime_type(formats, "text/html");
}

// Accepts text dragged from another app (not reminders dragged within this
// one, which carry text too, nor files, which are imported) on `widget`,
// showing where it would land: Halves (above / below a reminder) or Into
// (a sidebar list); Above shows nothing (the window itself). `on_drop`
// gets the text, or nullopt if it couldn't be read.
void make_text_drop_target(
	GtkWidget* widget, DropStyle style,
	std::function<void(std::optional<std::string>, rem::Document::Place)>
		on_drop) {
	auto* formats = gdk_content_formats_new(
		const_cast<const char**>(kTextMimes), G_N_ELEMENTS(kTextMimes));
	auto* target = gtk_drop_target_async_new(
		formats, GDK_ACTION_COPY);	// takes the formats
	connect<gboolean(GtkDropTargetAsync*, GdkDrop*)>(
		target, "accept", [](GtkDropTargetAsync*, GdkDrop* drop) -> gboolean {
			if (gdk_drop_get_drag(drop)) {
				return FALSE;  // from this app: reminders, moved by their own
							   // targets
			}
			auto* offered = gdk_drop_get_formats(drop);
			if (dnd_debug()) {
				char* f = gdk_content_formats_to_string(offered);
				g_printerr("drop offers: %s\n", f);
				g_free(f);
			}
			if (is_file_drop(drop)) {
				return FALSE;
			}
			for (auto* m : kTextMimes) {
				if (gdk_content_formats_contain_mime_type(offered, m)) {
					return TRUE;
				}
			}
			return FALSE;
		});
	auto below = [style](GtkWidget* w, double y) {
		return style == DropStyle::Halves && y > gtk_widget_get_height(w) / 2.0;
	};
	auto clear = [](GtkWidget* w) {
		if (!w) {
			return;
		}
		for (auto* c : {"drop-above", "drop-below", "drop-into"}) {
			gtk_widget_remove_css_class(w, c);
		}
	};
	auto motion = [style, below, clear](GtkDropTargetAsync* t, GdkDrop*, double,
										double y) {
		auto* w = owner(t);
		if (!w) {
			return GdkDragAction(0);
		}
		clear(w);
		if (style == DropStyle::Into) {
			gtk_widget_add_css_class(w, "drop-into");
		} else if (style == DropStyle::Halves) {
			gtk_widget_add_css_class(w,
									 below(w, y) ? "drop-below" : "drop-above");
		}
		return GDK_ACTION_COPY;
	};
	connect<GdkDragAction(GtkDropTargetAsync*, GdkDrop*, double, double)>(
		target, "drag-enter", motion);
	connect<GdkDragAction(GtkDropTargetAsync*, GdkDrop*, double, double)>(
		target, "drag-motion", motion);
	connect<void(GtkDropTargetAsync*, GdkDrop*)>(
		target, "drag-leave",
		[clear](GtkDropTargetAsync* t, GdkDrop*) { clear(owner(t)); });
	connect<gboolean(GtkDropTargetAsync*, GdkDrop*, double, double)>(
		target, "drop",
		[below, clear, on_drop](GtkDropTargetAsync* t, GdkDrop* drop, double,
								double y) -> gboolean {
			auto* w = owner(t);
			clear(w);
			auto place = w && below(w, y) ? rem::Document::Place::After
										  : rem::Document::Place::Before;
			auto* job = new TextDropRead{
				Obj<GdkDrop>::ref(drop),
				{},
				0,
				{},
				[on_drop, place](std::optional<std::string> text) {
					// Adding rebuilds the view; not from inside the read.
					idle([on_drop, text, place] { on_drop(text, place); });
				}};
			auto* offered = gdk_drop_get_formats(drop);
			for (auto* m : kTextMimes) {
				if (gdk_content_formats_contain_mime_type(offered, m)) {
					job->mimes.emplace_back(m);
				}
			}
			read_next_text(job);
			return TRUE;
		});
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(target));
}

}  // namespace ui::detail
