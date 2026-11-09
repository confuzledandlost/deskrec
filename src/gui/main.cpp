#include "recorder.hpp"
#include "source_watcher.hpp"

#include <gtk/gtk.h>
#include <pulse/glib-mainloop.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kAppId = "io.github.confuzledandlost.deskrec";
constexpr const char* kDefaultKey = "default";

enum class SourceKind { DefaultOutput, Output, App };

struct SourceEntry {
    SourceKind kind;
    std::string key;     // stable across refreshes, used to keep the selection
    std::string target;  // sink name (Output) or app match pattern (App)
    std::string title;
    std::string subtitle;
    std::string icon;
};

// Apps stay listed after their audio stops, so you can pick one and press
// Record before it starts playing again.
struct SeenApp {
    std::string name;
    std::string icon;
    size_t playing = 0;
    size_t paused = 0;
};

struct App {
    GtkApplication* gapp = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* list = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* record_btn = nullptr;
    GtkWidget* open_btn = nullptr;

    pa_glib_mainloop* pa_loop = nullptr;
    std::unique_ptr<SourceWatcher> watcher;
    std::unique_ptr<Recorder> recorder;

    std::vector<SourceEntry> entries;
    std::map<std::string, SeenApp> seen_apps;
    std::string selected_key = kDefaultKey;
    bool rebuilding = false;

    std::string recording_title;
    std::string last_error;
    std::string last_file;
    uint32_t recording_rate = 0;
    int finished_code = 0;
    uint64_t finished_frames = 0;
    guint status_timer = 0;
    guint finished_idle = 0;
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string output_dir() {
    const char* music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    std::string dir = music ? music : g_get_home_dir();
    g_mkdir_with_parents(dir.c_str(), 0755);
    return dir;
}

void set_status(App& app, const std::string& text) {
    gtk_label_set_text(GTK_LABEL(app.status), text.c_str());
}

// ---------------------------------------------------------------- source list

std::vector<SourceEntry> build_entries(App& app) {
    std::vector<SourceEntry> out;
    const auto& outputs = app.watcher->outputs();

    std::string default_desc = "the default output";
    for (const auto& o : outputs)
        if (o.is_default) default_desc = o.description;
    out.push_back({SourceKind::DefaultOutput, kDefaultKey, "", "All desktop audio",
                   "Everything playing on " + default_desc, "audio-speakers-symbolic"});

    if (outputs.size() > 1) {
        for (const auto& o : outputs) {
            if (o.is_default) continue;
            out.push_back({SourceKind::Output, "out:" + o.name, o.name, o.description,
                           "Only what plays on this output", "audio-card-symbolic"});
        }
    }

    for (auto& [key, seen] : app.seen_apps) seen.playing = seen.paused = 0;
    for (const auto& s : app.watcher->apps()) {
        const std::string name = !s.app.empty() ? s.app : s.binary;
        if (name.empty()) continue;
        SeenApp& seen = app.seen_apps["app:" + lower(name)];
        seen.name = name;
        if (!s.icon.empty()) seen.icon = s.icon;
        ++(s.corked ? seen.paused : seen.playing);
    }

    GtkIconTheme* theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(app.window));
    for (const auto& [key, seen] : app.seen_apps) {
        std::string subtitle;
        if (seen.playing > 1)
            subtitle = "Playing (" + std::to_string(seen.playing) + " streams)";
        else if (seen.playing == 1)
            subtitle = "Playing";
        else if (seen.paused > 0)
            subtitle = "Paused";
        else
            subtitle = "Not playing right now";
        const bool has_icon = !seen.icon.empty() && gtk_icon_theme_has_icon(theme, seen.icon.c_str());
        out.push_back({SourceKind::App, key, seen.name, seen.name, subtitle,
                       has_icon ? seen.icon : "audio-x-generic-symbolic"});
    }
    return out;
}

GtkWidget* make_row(const SourceEntry& e) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    GtkWidget* icon = gtk_image_new_from_icon_name(e.icon.c_str());
    gtk_image_set_icon_size(GTK_IMAGE(icon), GTK_ICON_SIZE_LARGE);
    gtk_box_append(GTK_BOX(box), icon);

    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* title = gtk_label_new(e.title.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    GtkWidget* subtitle = gtk_label_new(e.subtitle.c_str());
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_widget_add_css_class(subtitle, "caption");
    gtk_box_append(GTK_BOX(text), title);
    gtk_box_append(GTK_BOX(text), subtitle);
    gtk_box_append(GTK_BOX(box), text);
    return box;
}

void rebuild_list(App& app) {
    app.entries = build_entries(app);

    app.rebuilding = true;
    while (GtkWidget* child = gtk_widget_get_first_child(app.list)) gtk_list_box_remove(GTK_LIST_BOX(app.list), child);

    int selected = 0;
    for (size_t i = 0; i < app.entries.size(); ++i) {
        gtk_list_box_append(GTK_LIST_BOX(app.list), make_row(app.entries[i]));
        if (app.entries[i].key == app.selected_key) selected = int(i);
    }
    gtk_list_box_select_row(GTK_LIST_BOX(app.list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(app.list), selected));
    app.selected_key = app.entries[size_t(selected)].key;
    app.rebuilding = false;
}

void on_row_selected(GtkListBox*, GtkListBoxRow* row, gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    if (app.rebuilding || !row) return;
    const int idx = gtk_list_box_row_get_index(row);
    if (idx >= 0 && size_t(idx) < app.entries.size()) app.selected_key = app.entries[size_t(idx)].key;
}

// ---------------------------------------------------------------- recording

void set_recording_ui(App& app, bool recording) {
    gtk_button_set_label(GTK_BUTTON(app.record_btn), recording ? "Stop" : "Record");
    if (recording) {
        gtk_widget_remove_css_class(app.record_btn, "suggested-action");
        gtk_widget_add_css_class(app.record_btn, "destructive-action");
    } else {
        gtk_widget_remove_css_class(app.record_btn, "destructive-action");
        gtk_widget_add_css_class(app.record_btn, "suggested-action");
    }
    gtk_widget_set_sensitive(app.list, !recording);
}

gboolean on_status_tick(gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    if (!app.recorder) {
        app.status_timer = 0;
        return G_SOURCE_REMOVE;
    }
    const RecorderStatus s = app.recorder->status();
    if (s.started)
        set_status(app, "Recording " + app.recording_title + "   " + format_duration(s.seconds));
    else
        set_status(app, "Waiting for " + app.recording_title + " to play audio...");
    return G_SOURCE_CONTINUE;
}

gboolean on_finished_idle(gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    app.finished_idle = 0;
    app.recorder.reset();
    if (app.status_timer) {
        g_source_remove(app.status_timer);
        app.status_timer = 0;
    }
    set_recording_ui(app, false);

    if (app.finished_code != 0 && !app.last_error.empty()) {
        set_status(app, "Recording failed: " + app.last_error);
    } else if (app.finished_frames == 0) {
        set_status(app, "Nothing was recorded (no audio played).");
    } else {
        char* base = g_path_get_basename(app.last_file.c_str());
        set_status(app, "Saved " + format_duration(double(app.finished_frames) / app.recording_rate) + " to " + base);
        g_free(base);
        gtk_widget_set_visible(app.open_btn, TRUE);
    }
    return G_SOURCE_REMOVE;
}

void start_recording(App& app) {
    const SourceEntry* entry = nullptr;
    for (const auto& e : app.entries)
        if (e.key == app.selected_key) entry = &e;
    if (!entry) return;

    RecordOptions opts;
    if (entry->kind == SourceKind::Output) opts.sink = entry->target;
    if (entry->kind == SourceKind::App) opts.apps.push_back(entry->target);
    char* path = g_build_filename(output_dir().c_str(), default_filename(opts.format).c_str(), nullptr);
    opts.output = path;
    g_free(path);

    app.last_error.clear();
    app.last_file = opts.output;
    app.recording_title = entry->title;
    app.recording_rate = opts.rate;

    Recorder::Hooks hooks;
    hooks.error = [&app](const std::string& msg) {
        app.last_error = msg;
        set_status(app, msg);
    };
    hooks.finished = [&app](int code, uint64_t frames) {
        app.finished_code = code;
        app.finished_frames = frames;
        // The recorder is still on the call stack here; tear it down from idle.
        app.finished_idle = g_idle_add(on_finished_idle, &app);
    };

    app.recorder = std::make_unique<Recorder>(pa_glib_mainloop_get_api(app.pa_loop), opts, hooks);
    if (!app.recorder->start()) {
        app.recorder.reset();
        set_status(app, "Couldn't start recording: " + app.last_error);
        return;
    }

    gtk_widget_set_visible(app.open_btn, FALSE);
    set_recording_ui(app, true);
    on_status_tick(&app);
    app.status_timer = g_timeout_add(250, on_status_tick, &app);
}

void on_record_clicked(GtkButton*, gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    if (app.recorder)
        app.recorder->stop();
    else
        start_recording(app);
}

void on_open_clicked(GtkButton*, gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    GFile* file = g_file_new_for_path(app.last_file.c_str());
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkFileLauncher* launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_open_containing_folder(launcher, GTK_WINDOW(app.window), nullptr, nullptr, nullptr);
    g_object_unref(launcher);
#else
    GFile* dir = g_file_get_parent(file);
    char* uri = g_file_get_uri(dir);
    gtk_show_uri(GTK_WINDOW(app.window), uri, GDK_CURRENT_TIME);
    g_free(uri);
    g_object_unref(dir);
#endif
    g_object_unref(file);
}

gboolean on_close_request(GtkWindow*, gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    // Save whatever has been recorded instead of losing it. The window is going
    // away, so skip the UI updates the finished hook would schedule.
    if (app.recorder) app.recorder->stop();
    if (app.finished_idle) g_source_remove(app.finished_idle);
    if (app.status_timer) g_source_remove(app.status_timer);
    app.finished_idle = app.status_timer = 0;
    app.recorder.reset();
    return FALSE;
}

// ---------------------------------------------------------------- window

void on_activate(GtkApplication* gapp, gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    if (app.window) {
        gtk_window_present(GTK_WINDOW(app.window));
        return;
    }

    app.window = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(app.window), "deskrec");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 420, 480);
    g_signal_connect(app.window, "close-request", G_CALLBACK(on_close_request), &app);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(app.window), box);

    GtkWidget* heading = gtk_label_new("Record from");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(box), heading);

    app.list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.list), GTK_SELECTION_SINGLE);
    g_signal_connect(app.list, "row-selected", G_CALLBACK(on_row_selected), &app);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app.list);
    gtk_widget_set_vexpand(scroller, TRUE);
    GtkWidget* frame = gtk_frame_new(nullptr);
    gtk_frame_set_child(GTK_FRAME(frame), scroller);
    gtk_box_append(GTK_BOX(box), frame);

    app.status = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(app.status), 0);
    gtk_label_set_wrap(GTK_LABEL(app.status), TRUE);
    gtk_box_append(GTK_BOX(box), app.status);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.open_btn = gtk_button_new_with_label("Show file");
    gtk_widget_set_visible(app.open_btn, FALSE);
    g_signal_connect(app.open_btn, "clicked", G_CALLBACK(on_open_clicked), &app);
    app.record_btn = gtk_button_new_with_label("Record");
    gtk_widget_add_css_class(app.record_btn, "suggested-action");
    gtk_widget_set_hexpand(app.record_btn, TRUE);
    g_signal_connect(app.record_btn, "clicked", G_CALLBACK(on_record_clicked), &app);
    gtk_box_append(GTK_BOX(buttons), app.record_btn);
    gtk_box_append(GTK_BOX(buttons), app.open_btn);
    gtk_box_append(GTK_BOX(box), buttons);

    set_status(app, "Recordings are saved to " + output_dir());

    SourceWatcher::Hooks hooks;
    hooks.changed = [&app] { rebuild_list(app); };
    hooks.failed = [&app](const std::string& why) {
        set_status(app, "Can't connect to the sound server: " + why);
        gtk_widget_set_sensitive(app.record_btn, FALSE);
    };
    app.watcher = std::make_unique<SourceWatcher>(pa_glib_mainloop_get_api(app.pa_loop), hooks, /*live=*/true);
    app.watcher->start();

    gtk_window_present(GTK_WINDOW(app.window));
}

}  // namespace

int main(int argc, char** argv) {
    App app;
    app.pa_loop = pa_glib_mainloop_new(nullptr);
#if GLIB_CHECK_VERSION(2, 74, 0)
    app.gapp = gtk_application_new(kAppId, G_APPLICATION_DEFAULT_FLAGS);
#else
    app.gapp = gtk_application_new(kAppId, G_APPLICATION_FLAGS_NONE);
#endif
    g_signal_connect(app.gapp, "activate", G_CALLBACK(on_activate), &app);
    const int ret = g_application_run(G_APPLICATION(app.gapp), argc, argv);

    app.recorder.reset();
    app.watcher.reset();
    g_object_unref(app.gapp);
    pa_glib_mainloop_free(app.pa_loop);
    return ret;
}
