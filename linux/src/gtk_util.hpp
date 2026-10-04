// Small RAII and lambda helpers over the GTK/GLib C API.
#pragma once

#include <adwaita.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace ui {

// Owning reference to a GObject (or anything with g_object_unref).
template <class T>
class Obj {
	public:
		Obj() = default;
		// Takes over a reference the caller owns (e.g. from a *_new() of a
		// non-widget type, or g_file_new_for_path).
		static Obj adopt(T* p) {
			Obj o;
			o.p_ = p;
			return o;
		}
		// Adds a reference of its own.
		static Obj ref(T* p) {
			Obj o;
			o.p_ = p ? static_cast<T*>(g_object_ref(p)) : nullptr;
			return o;
		}
		Obj(const Obj& o)
			: p_(o.p_ ? static_cast<T*>(g_object_ref(o.p_)) : nullptr) {}
		Obj(Obj&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
		Obj& operator=(Obj o) noexcept {
			std::swap(p_, o.p_);
			return *this;
		}
		~Obj() {
			if (p_) {
				g_object_unref(p_);
			}
		}

		T* get() const { return p_; }
		T* operator->() const { return p_; }
		explicit operator bool() const { return p_ != nullptr; }
		void reset() { *this = Obj(); }

	private:
		T* p_ = nullptr;
};

// g_free'd strings.
struct GFreeDeleter {
		void operator()(void* p) const { g_free(p); }
};
using GStr = std::unique_ptr<char, GFreeDeleter>;

inline std::string take_string(char* s) {
	GStr owned(s);
	return owned ? std::string(owned.get()) : std::string();
}

// Connects a C++ callable to a signal. `Sig` is the handler's C signature
// without the trailing user_data, e.g. connect<void(GtkButton*)>(...).
template <class Sig>
struct Trampoline;

template <class R, class... A>
struct Trampoline<R(A...)> {
		using Fn = std::function<R(A...)>;
		static R call(A... args, gpointer data) {
			return (*static_cast<Fn*>(data))(args...);
		}
		static void destroy(gpointer data, GClosure*) {
			delete static_cast<Fn*>(data);
		}
};

template <class Sig, class F>
gulong connect(gpointer instance, const char* signal, F&& f,
			   bool after = false) {
	using T = Trampoline<Sig>;
	return g_signal_connect_data(instance, signal, G_CALLBACK(&T::call),
								 new typename T::Fn(std::forward<F>(f)),
								 &T::destroy,
								 after ? G_CONNECT_AFTER : GConnectFlags{});
}

// Common shorthand: a no-argument handler for a signal whose only argument
// is the emitter.
template <class F>
gulong on(gpointer instance, const char* signal, F&& f) {
	return connect<void(gpointer)>(instance, signal,
								   [f = std::forward<F>(f)](gpointer) { f(); });
}

// Adds a simple action to a GActionMap (window or application).
inline GSimpleAction* add_action(gpointer map, const char* name,
								 std::function<void()> f) {
	auto* action = g_simple_action_new(name, nullptr);
	connect<void(GSimpleAction*, GVariant*)>(
		action, "activate",
		[f = std::move(f)](GSimpleAction*, GVariant*) { f(); });
	g_action_map_add_action(G_ACTION_MAP(map), G_ACTION(action));
	g_object_unref(action);
	return action;
}

// Adds a stateful boolean action (for check menu items).
inline GSimpleAction* add_toggle(gpointer map, const char* name, bool initial,
								 std::function<void(bool)> f) {
	auto* action = g_simple_action_new_stateful(name, nullptr,
												g_variant_new_boolean(initial));
	connect<void(GSimpleAction*, GVariant*)>(
		action, "activate", [f = std::move(f)](GSimpleAction* a, GVariant*) {
			GVariant* state = g_action_get_state(G_ACTION(a));
			bool next = !g_variant_get_boolean(state);
			g_variant_unref(state);
			g_simple_action_set_state(a, g_variant_new_boolean(next));
			f(next);
		});
	g_action_map_add_action(G_ACTION_MAP(map), G_ACTION(action));
	g_object_unref(action);
	return action;
}

// Runs `f` after `ms` milliseconds; return true from `f` to repeat.
inline guint timeout(guint ms, std::function<bool()> f) {
	auto* fn = new std::function<bool()>(std::move(f));
	return g_timeout_add_full(
		G_PRIORITY_DEFAULT, ms,
		[](gpointer d) -> gboolean {
			return (*static_cast<std::function<bool()>*>(d))() ? TRUE : FALSE;
		},
		fn, [](gpointer d) { delete static_cast<std::function<bool()>*>(d); });
}

inline void idle(std::function<void()> f) {
	auto* fn = new std::function<void()>(std::move(f));
	g_idle_add_full(
		G_PRIORITY_DEFAULT_IDLE,
		[](gpointer d) -> gboolean {
			(*static_cast<std::function<void()>*>(d))();
			return FALSE;
		},
		fn, [](gpointer d) { delete static_cast<std::function<void()>*>(d); });
}

// Ties a C++ object's lifetime to a GObject: deleted when the GObject is.
template <class T>
T* attach(gpointer owner, const char* key, std::unique_ptr<T> obj) {
	T* raw = obj.release();
	g_object_set_data_full(G_OBJECT(owner), key, raw,
						   [](gpointer p) { delete static_cast<T*>(p); });
	return raw;
}

// Runs `f` when `accel` (e.g. "<Control>s") is pressed while focus is
// inside `widget`.
inline void add_shortcut(GtkWidget* widget, const char* accel,
						 std::function<void()> f) {
	auto* controller = gtk_shortcut_controller_new();
	auto* fn = new std::function<void()>(std::move(f));
	auto* action = gtk_callback_action_new(
		[](GtkWidget*, GVariant*, gpointer d) -> gboolean {
			(*static_cast<std::function<void()>*>(d))();
			return TRUE;
		},
		fn, [](gpointer d) { delete static_cast<std::function<void()>*>(d); });
	gtk_shortcut_controller_add_shortcut(
		GTK_SHORTCUT_CONTROLLER(controller),
		gtk_shortcut_new(gtk_shortcut_trigger_parse_string(accel), action));
	gtk_widget_add_controller(widget, controller);
}

// Widget construction shorthands.
inline GtkWidget* label(const std::string& text,
						std::initializer_list<const char*> classes = {}) {
	auto* l = gtk_label_new(text.c_str());
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	for (auto* c : classes) {
		gtk_widget_add_css_class(l, c);
	}
	return l;
}

inline GtkWidget* icon(const char* name,
					   std::initializer_list<const char*> classes = {}) {
	auto* i = gtk_image_new_from_icon_name(name);
	for (auto* c : classes) {
		gtk_widget_add_css_class(i, c);
	}
	return i;
}

inline GtkWidget* hbox(int spacing) {
	return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
}
inline GtkWidget* vbox(int spacing) {
	return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
}

inline void append(GtkWidget* box, std::initializer_list<GtkWidget*> children) {
	for (auto* c : children) {
		gtk_box_append(GTK_BOX(box), c);
	}
}

}  // namespace ui
