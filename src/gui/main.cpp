#include "recorder.hpp"
#include "source_watcher.hpp"

#include <gtk/gtk.h>
#include <pulse/glib-mainloop.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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
    std::string file_label;  // used in the recording's filename
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
    GtkWidget* pause_btn = nullptr;
    GtkWidget* open_btn = nullptr;
    GtkWidget* meter = nullptr;
    double meter_level = 0;
    GtkWidget* format_dropdown = nullptr;
    GtkWidget* folder_btn = nullptr;

    // Settings, persisted in ~/.config/deskrec/settings.ini.
    Format format = Format::Wav;
    std::string folder;
    std::vector<Format> formats;  // dropdown index -> format

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

std::string default_folder() {
    const char* music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    return music ? music : g_get_home_dir();
}

void set_status(App& app, const std::string& text) {
    gtk_label_set_text(GTK_LABEL(app.status), text.c_str());
}

// Event sounds (notification bings, volume pops) and speech-dispatcher's
// always-silent placeholder stream aren't useful recording targets on their own;
// "All desktop audio" still includes them.
bool is_system_stream(const AppStreamInfo& s) {
    return s.role == "event" || s.binary == "gnome-shell" || s.binary == "sd_dummy";
}

// ---------------------------------------------------------------- settings

std::string settings_path() {
    char* path = g_build_filename(g_get_user_config_dir(), "deskrec", "settings.ini", nullptr);
    std::string out = path;
    g_free(path);
    return out;
}

void load_settings(App& app) {
    app.format = Format::Wav;
    app.folder = default_folder();

    GKeyFile* kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, settings_path().c_str(), G_KEY_FILE_NONE, nullptr)) {
        if (char* f = g_key_file_get_string(kf, "recording", "format", nullptr)) {
            Format parsed;
            if (parse_format(f, parsed) && format_available(parsed)) app.format = parsed;
            g_free(f);
        }
        if (char* dir = g_key_file_get_string(kf, "recording", "folder", nullptr)) {
            if (g_file_test(dir, G_FILE_TEST_IS_DIR)) app.folder = dir;
            g_free(dir);
        }
    }
    g_key_file_free(kf);
}

void save_settings(const App& app) {
    GKeyFile* kf = g_key_file_new();
    g_key_file_set_string(kf, "recording", "format", format_name(app.format));
    g_key_file_set_string(kf, "recording", "folder", app.folder.c_str());

    const std::string path = settings_path();
    char* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_key_file_save_to_file(kf, path.c_str(), nullptr);
    g_key_file_free(kf);
}

void update_folder_button(App& app) {
    char* base = g_path_get_basename(app.folder.c_str());
    gtk_button_set_label(GTK_BUTTON(app.folder_btn), base);
    gtk_widget_set_tooltip_text(app.folder_btn, app.folder.c_str());
    g_free(base);
}

void on_format_changed(GObject* dropdown, GParamSpec*, gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    const guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (idx >= app.formats.size()) return;
    app.format = app.formats[idx];
    save_settings(app);
}

void set_folder(App& app, GFile* folder) {
    if (char* path = g_file_get_path(folder)) {
        app.folder = path;
        g_free(path);
        update_folder_button(app);
        save_settings(app);
    }
}

#if GTK_CHECK_VERSION(4, 10, 0)
void on_folder_chosen(GObject* source, GAsyncResult* result, gpointer ud) {
    GFile* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, nullptr);
    if (!folder) return;  // cancelled
    set_folder(*static_cast<App*>(ud), folder);
    g_object_unref(folder);
}
#else
void on_folder_response(GtkNativeDialog* dialog, int response, gpointer ud) {
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile* folder = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        if (folder) {
            set_folder(*static_cast<App*>(ud), folder);
            g_object_unref(folder);
        }
    }
    g_object_unref(dialog);
}
#endif

void on_folder_clicked(GtkButton*, gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    GFile* current = g_file_new_for_path(app.folder.c_str());
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save recordings to");
    gtk_file_dialog_set_initial_folder(dialog, current);
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app.window), nullptr, on_folder_chosen, &app);
    g_object_unref(dialog);
#else
    GtkFileChooserNative* dialog = gtk_file_chooser_native_new(
        "Save recordings to", GTK_WINDOW(app.window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Select", "_Cancel");
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), current, nullptr);
    g_signal_connect(dialog, "response", G_CALLBACK(on_folder_response), &app);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
#endif
    g_object_unref(current);
}

// ---------------------------------------------------------------- source list

std::vector<SourceEntry> build_entries(App& app) {
    std::vector<SourceEntry> out;
    const auto& outputs = app.watcher->outputs();

    std::string default_desc = "the default output";
    for (const auto& o : outputs)
        if (o.is_default) default_desc = o.description;
    out.push_back({SourceKind::DefaultOutput, kDefaultKey, "", "All desktop audio",
                   "Everything playing on " + default_desc, "audio-speakers-symbolic", "Desktop audio"});

    if (outputs.size() > 1) {
        for (const auto& o : outputs) {
            if (o.is_default) continue;
            out.push_back({SourceKind::Output, "out:" + o.name, o.name, o.description,
                           "Only what plays on this output", "audio-card-symbolic", o.description});
        }
    }

    for (auto& [key, seen] : app.seen_apps) seen.playing = seen.paused = 0;
    for (const auto& s : app.watcher->apps()) {
        const std::string name = !s.app.empty() ? s.app : s.binary;
        if (name.empty() || is_system_stream(s)) continue;
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
                       has_icon ? seen.icon : "audio-x-generic-symbolic", seen.name});
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
    gtk_widget_set_sensitive(app.format_dropdown, !recording);
    gtk_widget_set_sensitive(app.folder_btn, !recording);

    gtk_button_set_label(GTK_BUTTON(app.pause_btn), "Pause");
    gtk_widget_set_visible(app.pause_btn, recording);
    app.meter_level = 0;
    gtk_level_bar_set_value(GTK_LEVEL_BAR(app.meter), 0);
    gtk_widget_set_visible(app.meter, recording);
}

gboolean on_status_tick(gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    if (!app.recorder) {
        app.status_timer = 0;
        return G_SOURCE_REMOVE;
    }
    const RecorderStatus s = app.recorder->status();
    if (s.paused)
        set_status(app, "Paused   " + format_duration(s.seconds));
    else if (s.started)
        set_status(app, "Recording " + app.recording_title + "   " + format_duration(s.seconds));
    else
        set_status(app, "Waiting for " + app.recording_title + " to play audio...");

    // Map -60..0 dBFS onto the bar; let it fall back gradually like a VU meter.
    const double db = s.peak > 0 ? 20.0 * std::log10(double(s.peak)) : -100.0;
    const double level = s.paused ? 0.0 : std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
    app.meter_level = std::max(level, app.meter_level * 0.8);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(app.meter), app.meter_level);
    return G_SOURCE_CONTINUE;
}

void on_pause_clicked(GtkButton*, gpointer ud) {
    auto& app = *static_cast<App*>(ud);
    if (!app.recorder) return;
    const bool paused = app.recorder->status().paused;
    if (paused)
        app.recorder->resume();
    else
        app.recorder->pause();
    gtk_button_set_label(GTK_BUTTON(app.pause_btn), paused ? "Pause" : "Resume");
    on_status_tick(&app);
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
    opts.format = app.format;
    if (entry->kind == SourceKind::Output) opts.sink = entry->target;
    if (entry->kind == SourceKind::App) opts.apps.push_back(entry->target);
    g_mkdir_with_parents(app.folder.c_str(), 0755);
    opts.output = recording_path(app.folder, entry->file_label, opts.format);

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
    app.status_timer = g_timeout_add(100, on_status_tick, &app);
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

    load_settings(app);

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

    GtkWidget* settings = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* format_label = gtk_label_new("Format");
    gtk_widget_add_css_class(format_label, "dim-label");
    gtk_box_append(GTK_BOX(settings), format_label);

    std::vector<std::string> names;
    for (Format f : {Format::Wav, Format::Flac, Format::Mp3}) {
        if (!format_available(f)) continue;
        app.formats.push_back(f);
        names.push_back(f == Format::Wav    ? "WAV (uncompressed)"
                        : f == Format::Flac ? "FLAC (lossless, smaller)"
                                            : "MP3 (smallest)");
    }
    std::vector<const char*> c_names;
    for (const auto& n : names) c_names.push_back(n.c_str());
    c_names.push_back(nullptr);
    app.format_dropdown = gtk_drop_down_new_from_strings(c_names.data());
    const auto current = std::find(app.formats.begin(), app.formats.end(), app.format);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app.format_dropdown), guint(current - app.formats.begin()));
    g_signal_connect(app.format_dropdown, "notify::selected", G_CALLBACK(on_format_changed), &app);
    gtk_box_append(GTK_BOX(settings), app.format_dropdown);

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(settings), spacer);

    GtkWidget* folder_label = gtk_label_new("Save to");
    gtk_widget_add_css_class(folder_label, "dim-label");
    gtk_box_append(GTK_BOX(settings), folder_label);
    app.folder_btn = gtk_button_new();
    update_folder_button(app);
    g_signal_connect(app.folder_btn, "clicked", G_CALLBACK(on_folder_clicked), &app);
    gtk_box_append(GTK_BOX(settings), app.folder_btn);
    gtk_box_append(GTK_BOX(box), settings);

    app.status = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(app.status), 0);
    gtk_label_set_wrap(GTK_LABEL(app.status), TRUE);
    gtk_box_append(GTK_BOX(box), app.status);

    app.meter = gtk_level_bar_new();
    // Drop GTK's default low/high/full colour bands; a plain bar reads better as a meter.
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(app.meter), GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(app.meter), GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(app.meter), GTK_LEVEL_BAR_OFFSET_FULL);
    gtk_widget_set_visible(app.meter, FALSE);
    gtk_box_append(GTK_BOX(box), app.meter);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.open_btn = gtk_button_new_with_label("Show file");
    gtk_widget_set_visible(app.open_btn, FALSE);
    g_signal_connect(app.open_btn, "clicked", G_CALLBACK(on_open_clicked), &app);
    app.record_btn = gtk_button_new_with_label("Record");
    gtk_widget_add_css_class(app.record_btn, "suggested-action");
    gtk_widget_set_hexpand(app.record_btn, TRUE);
    g_signal_connect(app.record_btn, "clicked", G_CALLBACK(on_record_clicked), &app);
    app.pause_btn = gtk_button_new_with_label("Pause");
    gtk_widget_set_visible(app.pause_btn, FALSE);
    g_signal_connect(app.pause_btn, "clicked", G_CALLBACK(on_pause_clicked), &app);
    gtk_box_append(GTK_BOX(buttons), app.record_btn);
    gtk_box_append(GTK_BOX(buttons), app.pause_btn);
    gtk_box_append(GTK_BOX(buttons), app.open_btn);
    gtk_box_append(GTK_BOX(box), buttons);

    set_status(app, "Pick what to record and press Record.");

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
