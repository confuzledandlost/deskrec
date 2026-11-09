#include "recorder.hpp"
#include "source_watcher.hpp"

#include <getopt.h>
#include <pulse/pulseaudio.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>

namespace {

const char* kUsage = R"(deskrec - record desktop audio to WAV, FLAC or MP3

Usage:
  deskrec list                List outputs and apps currently playing audio
  deskrec record [options]    Record (default: everything on the default output)

Record options:
  -o, --output FILE       File to write (default: "<source> - YYYY-MM-DD HH-MM.<format>"
                          in the current directory)
  -F, --format FORMAT     wav (uncompressed), flac (lossless, ~half the size) or
                          mp3 (lossy, smallest). Default: taken from the -o
                          extension, otherwise wav
  -a, --app NAME          Record only apps whose name or binary contains NAME
                          (case-insensitive, repeatable: -a firefox -a spotify)
  -i, --index N           Record only the app stream with index N (see 'list', repeatable)
  -s, --sink NAME         Record a specific output instead of the default one
  -d, --duration SECS     Stop automatically after SECS seconds of audio
  -r, --rate HZ           Sample rate (default: 48000)
  -c, --channels N        Channel count (default: 2)
  -f, --float             Higher precision: 32-bit float WAV or 24-bit FLAC
                          (default: 16-bit)
  -b, --bitrate KBPS      MP3 constant bitrate, 8-320 (default: high-quality VBR,
                          around 190 kbps)
      --split SECS        Start a new file ("<name> - 01", "- 02", ...) after
                          SECS seconds of silence (1-30), e.g. between songs
      --auto-stop SECS    Stop after SECS seconds of silence
      --title TEXT        Title tag (FLAC/MP3; default: the file name)
      --artist TEXT       Artist tag (FLAC/MP3; default: what is being recorded)
  -q, --quiet             No status output
  -h, --help              Show this help

Recording starts when the first audio arrives. Stop it with Ctrl+C (or q).
Press space to pause and resume, or from elsewhere: pkill -USR1 -x deskrec
)";

// ---------------------------------------------------------------- list

std::string fit(const std::string& s, size_t width) {
    if (s.size() <= width) return s;
    return s.substr(0, width - 3) + "...";
}

void print_sources(const SourceWatcher& w) {
    std::map<uint32_t, std::string> sink_names;

    std::printf("Outputs  (record one with: deskrec record -s NAME; * = default)\n");
    for (const auto& o : w.outputs()) {
        sink_names[o.index] = o.description;
        std::printf("  %c %-55s %s\n", o.is_default ? '*' : ' ', o.name.c_str(), o.description.c_str());
    }

    std::printf("\nApps playing audio  (record with: deskrec record -a APP  or  -i INDEX)\n");
    if (w.apps().empty()) {
        std::printf("  (none right now - start playback in the app and run 'deskrec list' again)\n");
        return;
    }
    std::printf("  %-6s %-20s %-16s %-8s %-28s %s\n", "INDEX", "APP", "BINARY", "STATE", "OUTPUT", "MEDIA");
    for (const auto& a : w.apps()) {
        std::printf("  %-6u %-20s %-16s %-8s %-28s %s\n", a.index, fit(a.app, 20).c_str(), fit(a.binary, 16).c_str(),
                    a.corked ? "paused" : "playing", fit(sink_names[a.sink], 28).c_str(), fit(a.media, 40).c_str());
    }
}

int cmd_list() {
    pa_mainloop* ml = pa_mainloop_new();
    int ret = 1;
    {
        SourceWatcher* wp = nullptr;
        SourceWatcher::Hooks hooks;
        hooks.changed = [&] {
            print_sources(*wp);
            pa_mainloop_quit(ml, 0);
        };
        hooks.failed = [&](const std::string& why) {
            std::fprintf(stderr, "Could not connect to the sound server: %s\n", why.c_str());
            pa_mainloop_quit(ml, 1);
        };
        SourceWatcher watcher(pa_mainloop_get_api(ml), hooks, /*live=*/false);
        wp = &watcher;
        if (watcher.start()) pa_mainloop_run(ml, &ret);
    }
    pa_mainloop_free(ml);
    return ret;
}

// ---------------------------------------------------------------- record

class StatusLine {
public:
    explicit StatusLine(bool enabled) : enabled_(enabled) {}

    void log(const std::string& msg) {
        clear();
        std::fprintf(stderr, "%s\n", msg.c_str());
    }

    void show(const RecorderStatus& s) {
        if (!enabled_) return;
        constexpr int kWidth = 24;
        const float db = s.peak > 0 ? 20.0f * std::log10(s.peak) : -100.0f;
        const int bars = std::clamp(int((db + 60.0f) / 60.0f * kWidth + 0.5f), 0, kWidth);
        const std::string meter = std::string(size_t(bars), '#') + std::string(size_t(kWidth - bars), '.');
        const char* plural = s.streams == 1 ? "" : "s";
        const std::string track = s.track > 0 ? "  track " + std::to_string(s.track) : "";
        if (!s.started)
            std::fprintf(stderr, "\r\033[K  waiting for audio...  [%zu stream%s]", s.streams, plural);
        else if (s.paused)
            std::fprintf(stderr, "\r\033[K  PAUSED %s%s  (space to resume)", format_duration(s.seconds).c_str(),
                         track.c_str());
        else if (s.between_tracks)
            std::fprintf(stderr, "\r\033[K  REC %s  waiting for the next track...", format_duration(s.seconds).c_str());
        else
            std::fprintf(stderr, "\r\033[K  REC %s%s  [%s] %5.1f dB  [%zu stream%s]",
                         format_duration(s.seconds).c_str(), track.c_str(), meter.c_str(),
                         double(std::max(db, -99.9f)), s.streams, plural);
        std::fflush(stderr);
        dirty_ = true;
    }

    void clear() {
        if (!dirty_) return;
        std::fputs("\r\033[K", stderr);
        dirty_ = false;
    }

private:
    bool enabled_;
    bool dirty_ = false;
};

struct RecordSession {
    pa_mainloop* ml = nullptr;
    pa_mainloop_api* api = nullptr;
    std::unique_ptr<Recorder> recorder;
    StatusLine status{false};
    pa_time_event* status_ev = nullptr;
    pa_io_event* key_ev = nullptr;
    termios saved_tty{};
    bool tty_changed = false;
    bool quiet = false;
};

void toggle_pause(RecordSession& s) {
    const bool paused = s.recorder->status().paused;
    if (paused)
        s.recorder->resume();
    else
        s.recorder->pause();
    if (!s.quiet) s.status.log(paused ? "Resumed." : "Paused.");
    s.status.show(s.recorder->status());
}

// Single-key controls while recording in the foreground of a terminal: space
// or p toggles pause, q stops. Ctrl+C keeps working (ISIG stays on).
void enable_keys(RecordSession& s) {
    if (!isatty(STDIN_FILENO) || tcgetpgrp(STDIN_FILENO) != getpgrp()) return;
    if (tcgetattr(STDIN_FILENO, &s.saved_tty) != 0) return;
    termios raw = s.saved_tty;
    raw.c_lflag &= tcflag_t(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    s.tty_changed = true;
    s.key_ev = s.api->io_new(
        s.api, STDIN_FILENO, PA_IO_EVENT_INPUT,
        [](pa_mainloop_api* api, pa_io_event* e, int fd, pa_io_event_flags_t, void* ud) {
            auto& self = *static_cast<RecordSession*>(ud);
            char key = 0;
            const ssize_t n = read(fd, &key, 1);
            if (n <= 0) {
                api->io_free(e);
                self.key_ev = nullptr;
                return;
            }
            if (key == ' ' || key == 'p' || key == 'P') toggle_pause(self);
            if (key == 'q' || key == 'Q') self.recorder->stop();
        },
        &s);
}

void restore_keys(RecordSession& s) {
    if (s.key_ev) s.api->io_free(s.key_ev);
    s.key_ev = nullptr;
    if (s.tty_changed) tcsetattr(STDIN_FILENO, TCSANOW, &s.saved_tty);
    s.tty_changed = false;
}

void schedule_status(RecordSession& s) {
    timeval tv;
    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, 250 * 1000);
    if (s.status_ev)
        s.api->time_restart(s.status_ev, &tv);
    else
        s.status_ev = s.api->time_new(
            s.api, &tv,
            [](pa_mainloop_api*, pa_time_event*, const timeval*, void* ud) {
                auto& self = *static_cast<RecordSession*>(ud);
                self.status.show(self.recorder->status());
                schedule_status(self);
            },
            &s);
}

int run_record(const RecordOptions& opts, bool quiet) {
    RecordSession s;
    s.ml = pa_mainloop_new();
    s.api = pa_mainloop_get_api(s.ml);
    s.status = StatusLine(!quiet && isatty(STDERR_FILENO));
    s.quiet = quiet;
    pa_signal_init(s.api);

    Recorder::Hooks hooks;
    hooks.note = [&](const std::string& msg) {
        if (!quiet) s.status.log(msg);
    };
    hooks.error = [&](const std::string& msg) { s.status.log(msg); };
    hooks.finished = [&](int code, uint64_t frames) {
        s.status.clear();
        if (!quiet) {
            const auto& files = s.recorder->files();
            const std::string length = format_duration(double(frames) / opts.rate);
            if (frames == 0)
                s.status.log("No audio was captured; nothing saved.");
            else if (files.size() == 1)
                s.status.log("Saved " + length + " of audio to " + files[0]);
            else
                s.status.log("Saved " + length + " of audio in " + std::to_string(files.size()) + " tracks.");
        }
        pa_mainloop_quit(s.ml, code);
    };
    s.recorder = std::make_unique<Recorder>(s.api, opts, hooks);

    auto on_stop_signal = [](pa_mainloop_api*, pa_signal_event*, int, void* ud) {
        static_cast<RecordSession*>(ud)->recorder->stop();
    };
    auto on_pause_signal = [](pa_mainloop_api*, pa_signal_event*, int, void* ud) {
        toggle_pause(*static_cast<RecordSession*>(ud));
    };
    pa_signal_new(SIGINT, on_stop_signal, &s);
    pa_signal_new(SIGTERM, on_stop_signal, &s);
    pa_signal_new(SIGUSR1, on_pause_signal, &s);

    int ret = 1;
    if (s.recorder->start()) {
        enable_keys(s);
        schedule_status(s);
        pa_mainloop_run(s.ml, &ret);
    }

    restore_keys(s);
    if (s.status_ev) s.api->time_free(s.status_ev);
    s.recorder.reset();
    pa_signal_done();
    pa_mainloop_free(s.ml);
    return ret;
}

// ---------------------------------------------------------------- arguments

bool format_from_extension(const std::string& path, Format& out) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
    return parse_format(path.substr(dot + 1), out);
}

void print_usage(std::FILE* f) {
    std::fputs(kUsage, f);
    std::fprintf(f, "Formats in this build: %s\n", available_formats().c_str());
}

bool parse_uint(const char* s, unsigned long max, unsigned long& out) {
    errno = 0;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (errno || end == s || *end || v > max) return false;
    out = v;
    return true;
}

int bad_arg(const char* opt, const char* value) {
    std::fprintf(stderr, "deskrec: invalid value for %s: '%s'\n", opt, value);
    return 2;
}

bool parse_seconds(const char* s, double max, double& out) {
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s || *end || !(v > 0) || v > max) return false;
    out = v;
    return true;
}

enum LongOnly { kOptSplit = 1000, kOptAutoStop, kOptTitle, kOptArtist };

int cmd_record(int argc, char** argv) {
    static const option long_opts[] = {
        {"split", required_argument, nullptr, kOptSplit}, {"auto-stop", required_argument, nullptr, kOptAutoStop},
        {"title", required_argument, nullptr, kOptTitle}, {"artist", required_argument, nullptr, kOptArtist},
        {"output", required_argument, nullptr, 'o'},  {"app", required_argument, nullptr, 'a'},
        {"index", required_argument, nullptr, 'i'},   {"sink", required_argument, nullptr, 's'},
        {"duration", required_argument, nullptr, 'd'}, {"rate", required_argument, nullptr, 'r'},
        {"channels", required_argument, nullptr, 'c'}, {"float", no_argument, nullptr, 'f'},
        {"format", required_argument, nullptr, 'F'},  {"bitrate", required_argument, nullptr, 'b'},
        {"quiet", no_argument, nullptr, 'q'},         {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    RecordOptions opts;
    bool quiet = false;
    bool format_given = false;
    unsigned long n = 0;
    int c;
    while ((c = getopt_long(argc, argv, "o:a:i:s:d:r:c:fF:b:qh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'o': opts.output = optarg; break;
        case 'a': opts.apps.emplace_back(optarg); break;
        case 's': opts.sink = optarg; break;
        case 'f': opts.high_precision = true; break;
        case 'q': quiet = true; break;
        case 'F':
            if (!parse_format(optarg, opts.format)) return bad_arg("--format", optarg);
            format_given = true;
            break;
        case 'b':
            if (!parse_uint(optarg, 320, n) || n < 8) return bad_arg("--bitrate", optarg);
            opts.mp3_bitrate = int(n);
            break;
        case 'i':
            if (!parse_uint(optarg, 0xFFFFFFFEul, n)) return bad_arg("--index", optarg);
            opts.indices.push_back(uint32_t(n));
            break;
        case 'r':
            if (!parse_uint(optarg, 384000, n) || n < 8000) return bad_arg("--rate", optarg);
            opts.rate = uint32_t(n);
            break;
        case 'c':
            if (!parse_uint(optarg, 32, n) || n < 1) return bad_arg("--channels", optarg);
            opts.channels = uint8_t(n);
            break;
        case 'd': {
            char* end = nullptr;
            opts.duration = std::strtod(optarg, &end);
            if (end == optarg || *end || !(opts.duration > 0)) return bad_arg("--duration", optarg);
            break;
        }
        case kOptSplit:
            if (!parse_seconds(optarg, 30, opts.split_silence)) return bad_arg("--split", optarg);
            break;
        case kOptAutoStop:
            if (!parse_seconds(optarg, 86400, opts.stop_silence)) return bad_arg("--auto-stop", optarg);
            break;
        case kOptTitle: opts.tags.title = optarg; break;
        case kOptArtist: opts.tags.artist = optarg; break;
        case 'h': print_usage(stdout); return 0;
        default: print_usage(stderr); return 2;
        }
    }
    if (optind < argc) {
        std::fprintf(stderr, "deskrec: unexpected argument '%s'\n", argv[optind]);
        return 2;
    }
    if (!opts.sink.empty() && opts.app_mode()) {
        std::fprintf(stderr, "deskrec: --sink can't be combined with --app/--index\n");
        return 2;
    }
    if (!format_given && !opts.output.empty()) format_from_extension(opts.output, opts.format);
    if (!format_available(opts.format)) {
        std::fprintf(stderr, "deskrec: this build can't write %s files (available: %s)\n",
                     format_name(opts.format), available_formats().c_str());
        return 2;
    }
    if (opts.mp3_bitrate && opts.format != Format::Mp3) {
        std::fprintf(stderr, "deskrec: --bitrate only applies to mp3\n");
        return 2;
    }
    if (opts.high_precision && opts.format == Format::Mp3) {
        std::fprintf(stderr, "deskrec: --float doesn't apply to mp3 (use --bitrate for quality)\n");
        return 2;
    }
    std::string label = "Desktop audio";
    if (opts.apps.size() == 1 && opts.indices.empty()) {
        label = opts.apps[0];
        label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
    } else if (opts.app_mode()) {
        label = "Apps";
    } else if (!opts.sink.empty()) {
        label = opts.sink;
    }
    if (opts.output.empty()) opts.output = recording_path("", label, opts.format);
    if (opts.tags.artist.empty()) opts.tags.artist = label;

    return run_record(opts, quiet);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "list") return cmd_list();
    if (cmd == "record") return cmd_record(argc - 1, argv + 1);
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        print_usage(stdout);
        return 0;
    }
    std::fprintf(stderr, "deskrec: unknown command '%s'\n\n", cmd.c_str());
    print_usage(stderr);
    return 2;
}
