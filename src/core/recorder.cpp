#include "recorder.hpp"

#include "audio_writer.hpp"
#include "mixer.hpp"
#include "pulse_common.hpp"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <utility>
#include <unistd.h>

namespace {

constexpr pa_usec_t kTickUsec = 100 * 1000;
constexpr pa_usec_t kFragmentUsec = 20 * 1000;
constexpr pa_usec_t kSyncUsec = 1000 * 1000;
constexpr pa_usec_t kRetryUsec = 1000 * 1000;
// Audio is held this long before being written so every stream's data for a
// given moment has arrived and can be mixed together.
constexpr double kMixDelaySec = 3.0;
// A stream stays sample-contiguous unless its position is further than this
// from the wall clock (e.g. the app paused and resumed). The sound server can
// deliver data in bursts several hundred ms late, so this must be well above
// that and well below kMixDelaySec.
constexpr double kResyncSec = 1.5;

// Silence detection (split / auto-stop): anything below -60 dBFS counts as silent.
constexpr float kSilenceLevel = 0.001f;
// Silence kept at the end of a file when trailing silence is trimmed.
constexpr double kTailSec = 0.5;
// Upper bound on silence held back in memory while deciding whether to trim it.
constexpr double kMaxHeldSec = 30.0;

constexpr uint32_t kSystemKey = PA_INVALID_INDEX;

std::string unique_path(const std::string& base, const std::string& ext) {
    std::string path = base + ext;
    for (int n = 2; access(path.c_str(), F_OK) == 0; ++n) path = base + " (" + std::to_string(n) + ")" + ext;
    return path;
}

// Splits "dir/name.ext" into "dir/name" and ".ext" (ext may be empty).
std::pair<std::string, std::string> split_extension(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || (slash != std::string::npos && dot <= slash + 1))
        return {path, ""};
    return {path.substr(0, dot), path.substr(dot)};
}

std::string file_stem(const std::string& path) {
    std::string base = split_extension(path).first;
    const auto slash = base.find_last_of('/');
    return slash == std::string::npos ? base : base.substr(slash + 1);
}

std::string today() {
    char buf[16];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t));
    return buf;
}

}  // namespace

class Recorder::Impl {
public:
    Impl(pa_mainloop_api* api, RecordOptions opts, Hooks hooks)
        : opts_(std::move(opts)), hooks_(std::move(hooks)), api_(api), mixer_(opts_.channels) {}
    ~Impl();

    bool start();
    void stop(int code);
    void pause();
    void resume();
    RecorderStatus status();
    const RecordOptions& options() const { return opts_; }
    const std::vector<std::string>& files() const { return files_; }

private:
    struct Capture {
        Impl* owner = nullptr;
        uint32_t key = kSystemKey;  // sink input index, or kSystemKey for a whole output
        uint32_t sink = PA_INVALID_INDEX;
        std::string label;
        pa_stream* stream = nullptr;
        bool dead = false;
        bool announced = false;
        bool anchored = false;
        int64_t cursor = 0;
    };

    struct SinkLookup {
        Impl* owner;
        uint32_t key;
        uint32_t sink;
    };

    static void context_state_cb(pa_context* c, void* ud);
    static void subscribe_cb(pa_context* c, pa_subscription_event_type_t t, uint32_t idx, void* ud);
    static void server_info_cb(pa_context* c, const pa_server_info* info, void* ud);
    static void system_sink_cb(pa_context* c, const pa_sink_info* info, int eol, void* ud);
    static void sink_input_cb(pa_context* c, const pa_sink_input_info* info, int eol, void* ud);
    static void app_sink_cb(pa_context* c, const pa_sink_info* info, int eol, void* ud);
    static void stream_state_cb(pa_stream* s, void* ud);
    static void stream_read_cb(pa_stream* s, size_t nbytes, void* ud);
    static void tick_cb(pa_mainloop_api* api, pa_time_event* e, const struct timeval* tv, void* ud);

    void on_ready();
    void refresh_system_sink();
    void rescan_sink_inputs();
    void consider_sink_input(const pa_sink_input_info* info);
    bool matches(const pa_sink_input_info* info) const;
    void open_stream(Capture& c, const char* monitor_source);
    void close_capture(uint32_t key, bool announce);
    void on_data(Capture& c, const float* data, size_t frames, size_t queued_after);
    int64_t now_frames() const;
    void flush_to(int64_t upto);
    bool open_writer(const std::string& path, int track);
    void close_writer();
    void write_out(const float* samples, size_t frames);
    void emit(const float* samples, size_t frames);
    void on_sound(const float* samples, size_t frames);
    void on_silence(const float* samples, size_t frames);
    void write_tail();
    void end_track();
    void tick();
    void note(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    RecordOptions opts_;
    Hooks hooks_;
    pa_mainloop_api* api_;
    pa_context* ctx_ = nullptr;
    pa_time_event* tick_ev_ = nullptr;
    pa_sample_spec spec_{};
    pa_channel_map map_{};

    std::map<uint32_t, std::unique_ptr<Capture>> captures_;
    Mixer mixer_;
    std::unique_ptr<AudioWriter> writer_;  // null between tracks
    std::string current_path_;
    std::vector<std::string> files_;
    uint64_t total_frames_ = 0;  // from files already closed

    // Silence handling; only active when split_silence or stop_silence is set.
    bool silence_mode_ = false;
    int64_t split_frames_ = 0;
    int64_t stop_frames_ = 0;
    std::vector<float> held_;  // recent silence, written only if sound follows
    int64_t silent_run_ = 0;
    bool heard_ = false;
    bool between_tracks_ = false;
    bool silence_stop_ = false;
    int track_ = 0;

    pa_usec_t start_usec_ = 0;  // 0 until the first audio arrives; shifted forward by time spent paused
    pa_usec_t pause_usec_ = 0;
    bool paused_ = false;
    int64_t limit_frames_ = -1;
    float peak_ = 0;
    pa_usec_t last_sync_ = 0;
    pa_usec_t last_retry_ = 0;
    bool need_rescan_ = false;
    bool lookup_pending_ = false;
    bool waiting_shown_ = false;
    bool started_ = false;
    bool stopping_ = false;
    bool write_failed_ = false;
};

// ---------------------------------------------------------------- helpers

std::string format_duration(double seconds) {
    const long total = long(seconds);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
    return buf;
}

std::string recording_path(const std::string& dir, const std::string& label, Format format) {
    std::string name;
    for (unsigned char c : label) {
        if (c == '/' || c < 0x20) name += '_';
        else name += char(c);
    }
    if (name.empty() || name == "." || name == "..") name = "Recording";

    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M", std::localtime(&t));

    const std::string base = (dir.empty() ? "" : dir + "/") + name + " - " + stamp;
    return unique_path(base, std::string(".") + format_name(format));
}

std::string track_path(const std::string& path, int track) {
    const auto [base, ext] = split_extension(path);
    char num[16];
    std::snprintf(num, sizeof num, " - %02d", track);
    return unique_path(base + num, ext);
}

void Recorder::Impl::note(const char* fmt, ...) {
    if (!hooks_.note) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hooks_.note(buf);
}

void Recorder::Impl::error(const char* fmt, ...) {
    if (!hooks_.error) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hooks_.error(buf);
}

// ---------------------------------------------------------------- setup

bool Recorder::Impl::start() {
    if (started_) return true;

    spec_.format = PA_SAMPLE_FLOAT32NE;
    spec_.rate = opts_.rate;
    spec_.channels = opts_.channels;
    if (!pa_sample_spec_valid(&spec_)) {
        error("Invalid sample rate or channel count.");
        return false;
    }
    pa_channel_map_init_extend(&map_, spec_.channels, PA_CHANNEL_MAP_DEFAULT);
    if (opts_.duration > 0) limit_frames_ = std::llround(opts_.duration * opts_.rate);

    if (opts_.split_silence > 0) split_frames_ = std::max<int64_t>(1, std::llround(opts_.split_silence * opts_.rate));
    if (opts_.stop_silence > 0) stop_frames_ = std::max<int64_t>(1, std::llround(opts_.stop_silence * opts_.rate));
    silence_mode_ = split_frames_ > 0 || stop_frames_ > 0;
    if (split_frames_) track_ = 1;
    if (!open_writer(split_frames_ ? track_path(opts_.output, 1) : opts_.output, track_)) return false;

    pa_proplist* props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_ID, "deskrec");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ICON_NAME, "audio-input-microphone");
    ctx_ = pa_context_new_with_proplist(api_, "deskrec", props);
    pa_proplist_free(props);
    pa_context_set_state_callback(ctx_, context_state_cb, this);

    if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        error("Could not connect to the sound server: %s", pa_strerror(pa_context_errno(ctx_)));
        close_writer();
        return false;
    }
    started_ = true;
    return true;
}

Recorder::Impl::~Impl() {
    if (started_ && !stopping_) {
        hooks_ = {};
        stop(0);
    }
    if (tick_ev_) api_->time_free(tick_ev_);
    if (ctx_) {
        pa_context_set_state_callback(ctx_, nullptr, nullptr);
        pa_context_set_subscribe_callback(ctx_, nullptr, nullptr);
        pa_context_disconnect(ctx_);
        pa_context_unref(ctx_);
    }
}

void Recorder::Impl::context_state_cb(pa_context* c, void* ud) {
    auto* self = static_cast<Impl*>(ud);
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        self->on_ready();
        break;
    case PA_CONTEXT_FAILED:
        self->error("Lost connection to the sound server: %s", pa_strerror(pa_context_errno(c)));
        self->stop(1);
        break;
    default:
        break;
    }
}

void Recorder::Impl::on_ready() {
    pa_context_set_subscribe_callback(ctx_, subscribe_cb, this);
    const auto mask = opts_.app_mode() ? PA_SUBSCRIPTION_MASK_SINK_INPUT : PA_SUBSCRIPTION_MASK_SERVER;
    pa_operation_unref(pa_context_subscribe(ctx_, mask, nullptr, nullptr));

    if (opts_.app_mode())
        rescan_sink_inputs();
    else
        refresh_system_sink();

    tick_ev_ = pa_context_rttime_new(ctx_, pa_rtclock_now() + kTickUsec, tick_cb, this);
}

// ---------------------------------------------------------------- whole-output mode

void Recorder::Impl::refresh_system_sink() {
    if (lookup_pending_) return;
    lookup_pending_ = true;
    if (!opts_.sink.empty())
        pa_operation_unref(pa_context_get_sink_info_by_name(ctx_, opts_.sink.c_str(), system_sink_cb, this));
    else
        pa_operation_unref(pa_context_get_server_info(ctx_, server_info_cb, this));
}

void Recorder::Impl::server_info_cb(pa_context* c, const pa_server_info* info, void* ud) {
    auto* self = static_cast<Impl*>(ud);
    if (!info || !info->default_sink_name) {
        self->lookup_pending_ = false;
        return;
    }
    pa_operation_unref(pa_context_get_sink_info_by_name(c, info->default_sink_name, system_sink_cb, self));
}

void Recorder::Impl::system_sink_cb(pa_context*, const pa_sink_info* info, int eol, void* ud) {
    auto* self = static_cast<Impl*>(ud);
    if (eol != 0) {
        self->lookup_pending_ = false;
        if (eol < 0 && !self->opts_.sink.empty() && self->captures_.empty() && !self->start_usec_) {
            self->error("No output named '%s'.", self->opts_.sink.c_str());
            self->stop(1);
        }
        return;
    }
    if (self->stopping_) return;

    auto it = self->captures_.find(kSystemKey);
    if (it != self->captures_.end() && !it->second->dead && it->second->sink == info->index) return;
    if (it != self->captures_.end()) self->close_capture(kSystemKey, false);

    auto cap = std::make_unique<Capture>();
    cap->owner = self;
    cap->key = kSystemKey;
    cap->sink = info->index;
    cap->label = info->description ? info->description : info->name;
    Capture& ref = *cap;
    self->captures_[kSystemKey] = std::move(cap);
    self->note("Recording output: %s", ref.label.c_str());
    self->open_stream(ref, info->monitor_source_name);
}

// ---------------------------------------------------------------- per-app mode

void Recorder::Impl::rescan_sink_inputs() {
    need_rescan_ = false;
    pa_operation_unref(pa_context_get_sink_input_info_list(ctx_, sink_input_cb, this));
}

bool Recorder::Impl::matches(const pa_sink_input_info* info) const {
    if (std::find(opts_.indices.begin(), opts_.indices.end(), info->index) != opts_.indices.end())
        return true;
    const std::string app = get_prop(info->proplist, PA_PROP_APPLICATION_NAME);
    const std::string binary = get_prop(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    for (const auto& pattern : opts_.apps)
        if (contains_ci(app, pattern) || contains_ci(binary, pattern)) return true;
    return false;
}

void Recorder::Impl::sink_input_cb(pa_context*, const pa_sink_input_info* info, int eol, void* ud) {
    auto* self = static_cast<Impl*>(ud);
    if (eol != 0) {
        if (eol > 0 && self->captures_.empty() && !self->waiting_shown_) {
            self->waiting_shown_ = true;
            self->note("Waiting for a matching app to play audio...");
        }
        return;
    }
    if (!self->stopping_) self->consider_sink_input(info);
}

void Recorder::Impl::consider_sink_input(const pa_sink_input_info* info) {
    auto it = captures_.find(info->index);
    if (!matches(info)) {
        if (it != captures_.end()) close_capture(info->index, true);
        return;
    }
    if (it != captures_.end() && !it->second->dead && it->second->sink == info->sink) return;
    const bool announced = it != captures_.end() && it->second->announced;
    if (it != captures_.end()) close_capture(info->index, false);

    std::string label = get_prop(info->proplist, PA_PROP_APPLICATION_NAME);
    if (label.empty()) label = get_prop(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    if (label.empty()) label = info->name ? info->name : "unknown";
    label += " (#" + std::to_string(info->index) + ")";

    auto cap = std::make_unique<Capture>();
    cap->owner = this;
    cap->key = info->index;
    cap->sink = info->sink;
    cap->label = label;
    cap->announced = announced;
    captures_[info->index] = std::move(cap);

    auto* lookup = new SinkLookup{this, info->index, info->sink};
    pa_operation_unref(pa_context_get_sink_info_by_index(ctx_, info->sink, app_sink_cb, lookup));
}

void Recorder::Impl::app_sink_cb(pa_context*, const pa_sink_info* info, int eol, void* ud) {
    auto* lookup = static_cast<SinkLookup*>(ud);
    Impl* self = lookup->owner;
    auto it = self->captures_.find(lookup->key);
    const bool wanted = it != self->captures_.end() && !it->second->stream && !it->second->dead &&
                        it->second->sink == lookup->sink;

    if (eol != 0) {
        if (eol < 0 && wanted) it->second->dead = true;
        delete lookup;
        return;
    }
    if (wanted && !self->stopping_) {
        if (!it->second->announced) self->note("Capturing: %s", it->second->label.c_str());
        it->second->announced = true;
        self->open_stream(*it->second, info->monitor_source_name);
    }
}

void Recorder::Impl::subscribe_cb(pa_context* c, pa_subscription_event_type_t t, uint32_t idx, void* ud) {
    auto* self = static_cast<Impl*>(ud);
    if (self->stopping_) return;
    const auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    const auto type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        if (type == PA_SUBSCRIPTION_EVENT_REMOVE)
            self->close_capture(idx, true);
        else
            pa_operation_unref(pa_context_get_sink_input_info(c, idx, sink_input_cb, self));
    } else if (facility == PA_SUBSCRIPTION_EVENT_SERVER && self->opts_.sink.empty()) {
        self->refresh_system_sink();
    }
}

// ---------------------------------------------------------------- streams

void Recorder::Impl::open_stream(Capture& c, const char* monitor_source) {
    const std::string name = "deskrec: " + c.label;
    pa_stream* s = pa_stream_new(ctx_, name.c_str(), &spec_, &map_);
    if (!s) {
        error("Could not create capture stream: %s", pa_strerror(pa_context_errno(ctx_)));
        c.dead = true;
        return;
    }
    c.stream = s;
    pa_stream_set_state_callback(s, stream_state_cb, &c);
    pa_stream_set_read_callback(s, stream_read_cb, &c);
    if (c.key != kSystemKey) pa_stream_set_monitor_stream(s, c.key);

    pa_buffer_attr attr;
    attr.maxlength = uint32_t(-1);
    attr.tlength = uint32_t(-1);
    attr.prebuf = uint32_t(-1);
    attr.minreq = uint32_t(-1);
    attr.fragsize = uint32_t(pa_usec_to_bytes(kFragmentUsec, &spec_));

    // DONT_MOVE: if the output disappears, the server would otherwise move us
    // onto the default input, i.e. the microphone.
    const auto flags = pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
    if (pa_stream_connect_record(s, monitor_source, &attr, flags) < 0) {
        error("Could not start capture: %s", pa_strerror(pa_context_errno(ctx_)));
        c.dead = true;
    }
}

void Recorder::Impl::close_capture(uint32_t key, bool announce) {
    auto it = captures_.find(key);
    if (it == captures_.end()) return;
    Capture& c = *it->second;
    if (announce && c.stream) note("Stream ended: %s", c.label.c_str());
    if (c.stream) {
        pa_stream_set_state_callback(c.stream, nullptr, nullptr);
        pa_stream_set_read_callback(c.stream, nullptr, nullptr);
        const auto state = pa_stream_get_state(c.stream);
        if (state == PA_STREAM_READY || state == PA_STREAM_CREATING) pa_stream_disconnect(c.stream);
        pa_stream_unref(c.stream);
    }
    captures_.erase(it);
}

void Recorder::Impl::stream_state_cb(pa_stream* s, void* ud) {
    auto* c = static_cast<Capture*>(ud);
    const auto state = pa_stream_get_state(s);
    if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
        // Cleanup is deferred to tick(): unref'ing a stream inside its own callback is unsafe.
        c->dead = true;
    }
}

void Recorder::Impl::stream_read_cb(pa_stream* s, size_t, void* ud) {
    auto* c = static_cast<Capture*>(ud);
    const size_t frame_bytes = pa_frame_size(&c->owner->spec_);
    // Data can arrive in bursts; what's still queued behind a chunk tells us how
    // long ago that chunk was actually played.
    size_t pending = pa_stream_readable_size(s);
    while (pending > 0 && pending != size_t(-1)) {
        const void* data = nullptr;
        size_t nbytes = 0;
        if (pa_stream_peek(s, &data, &nbytes) < 0 || nbytes == 0) break;
        pending = pending > nbytes ? pending - nbytes : 0;
        c->owner->on_data(*c, static_cast<const float*>(data), nbytes / frame_bytes, pending / frame_bytes);
        pa_stream_drop(s);
    }
}

int64_t Recorder::Impl::now_frames() const {
    if (!start_usec_) return 0;
    const pa_usec_t now = paused_ ? pause_usec_ : pa_rtclock_now();
    return int64_t((now - start_usec_) * spec_.rate / PA_USEC_PER_SEC);
}

void Recorder::Impl::pause() {
    if (!started_ || stopping_ || paused_) return;
    paused_ = true;
    pause_usec_ = pa_rtclock_now();
}

void Recorder::Impl::resume() {
    if (!paused_) return;
    paused_ = false;
    if (start_usec_) start_usec_ += pa_rtclock_now() - pause_usec_;
}

void Recorder::Impl::on_data(Capture& c, const float* data, size_t frames, size_t queued_after) {
    if (stopping_ || paused_ || frames == 0) return;
    if (!data) {
        // A hole in the stream: keep the timeline moving, it will be silent.
        if (c.anchored) c.cursor += int64_t(frames);
        return;
    }
    if (!start_usec_) {
        start_usec_ = pa_rtclock_now() - pa_bytes_to_usec((frames + queued_after) * pa_frame_size(&spec_), &spec_);
    }

    const int64_t expected = std::max<int64_t>(0, now_frames() - int64_t(frames + queued_after));
    const int64_t resync = int64_t(kResyncSec * spec_.rate);
    if (!c.anchored || std::llabs(c.cursor - expected) > resync) {
        c.cursor = expected;
        c.anchored = true;
    }
    mixer_.add(c.cursor, data, frames);

    for (size_t i = 0; i < frames * spec_.channels; ++i) peak_ = std::max(peak_, std::fabs(data[i]));
}

// ---------------------------------------------------------------- main loop

void Recorder::Impl::flush_to(int64_t upto) {
    if (limit_frames_ >= 0) upto = std::min(upto, limit_frames_);
    mixer_.flush(upto, [this](const float* samples, size_t frames) { emit(samples, frames); });
}

// ---------------------------------------------------------------- output files

bool Recorder::Impl::open_writer(const std::string& path, int track) {
    EncoderOptions enc;
    enc.format = opts_.format;
    enc.rate = opts_.rate;
    enc.channels = opts_.channels;
    enc.high_precision = opts_.high_precision;
    enc.mp3_bitrate = opts_.mp3_bitrate;
    enc.tags = opts_.tags;
    if (enc.tags.title.empty())
        enc.tags.title = file_stem(path);
    else if (track > 0)
        enc.tags.title += " (" + std::to_string(track) + ")";
    if (enc.tags.date.empty()) enc.tags.date = today();
    if (enc.tags.comment.empty()) enc.tags.comment = "Recorded with deskrec";
    enc.tags.track = track;

    std::string why;
    auto writer = make_writer(enc, &why);
    if (!writer) {
        error("%s", why.c_str());
        return false;
    }
    if (!writer->open(path)) {
        error("Cannot create %s: %s", path.c_str(), writer->error().c_str());
        std::remove(path.c_str());
        return false;
    }
    writer_ = std::move(writer);
    current_path_ = path;
    files_.push_back(path);
    return true;
}

void Recorder::Impl::close_writer() {
    if (!writer_) return;
    const uint64_t frames = writer_->frames_written();
    writer_->close();
    writer_.reset();
    if (frames == 0) {
        std::remove(current_path_.c_str());
        files_.pop_back();
    }
    total_frames_ += frames;
}

void Recorder::Impl::write_out(const float* samples, size_t frames) {
    if (write_failed_ || !writer_ || frames == 0) return;
    if (!writer_->write(samples, frames)) {
        write_failed_ = true;
        error("Writing %s failed: %s", current_path_.c_str(), writer_->error().c_str());
    }
}

void Recorder::Impl::emit(const float* samples, size_t frames) {
    if (!silence_mode_) {
        write_out(samples, frames);
        return;
    }
    const size_t ch = spec_.channels;
    auto loud = [&](size_t i) {
        for (size_t c = 0; c < ch; ++c)
            if (std::fabs(samples[i * ch + c]) > kSilenceLevel) return true;
        return false;
    };
    for (size_t i = 0; i < frames;) {
        const bool l = loud(i);
        size_t j = i + 1;
        while (j < frames && loud(j) == l) ++j;
        if (l)
            on_sound(samples + i * ch, j - i);
        else
            on_silence(samples + i * ch, j - i);
        i = j;
    }
}

void Recorder::Impl::on_sound(const float* samples, size_t frames) {
    if (between_tracks_) {
        between_tracks_ = false;
        if (!open_writer(track_path(opts_.output, track_ + 1), track_ + 1)) {
            write_failed_ = true;
            return;
        }
        ++track_;
        note("Track %d: %s", track_, current_path_.c_str());
    } else if (heard_ && !held_.empty()) {
        // Just a quiet moment inside the recording: keep it.
        write_out(held_.data(), held_.size() / spec_.channels);
    }
    held_.clear();
    silent_run_ = 0;
    heard_ = true;
    write_out(samples, frames);
}

void Recorder::Impl::on_silence(const float* samples, size_t frames) {
    silent_run_ += int64_t(frames);
    // Leading silence and the gap between tracks are dropped.
    if (heard_ && !between_tracks_) {
        const size_t ch = spec_.channels;
        held_.insert(held_.end(), samples, samples + frames * ch);
        const size_t cap = size_t(kMaxHeldSec * spec_.rate) * ch;
        if (held_.size() > cap) {
            // Too long to keep in memory: give up on trimming the older half.
            const size_t out = (held_.size() - cap / 2) / ch;
            write_out(held_.data(), out);
            held_.erase(held_.begin(), held_.begin() + std::ptrdiff_t(out * ch));
        }
        if (split_frames_ && silent_run_ >= split_frames_) end_track();
    }
    if (stop_frames_ && heard_ && silent_run_ >= stop_frames_) silence_stop_ = true;
}

void Recorder::Impl::write_tail() {
    const size_t keep = std::min(held_.size() / spec_.channels, size_t(kTailSec * spec_.rate));
    write_out(held_.data(), keep);
    held_.clear();
}

void Recorder::Impl::end_track() {
    write_tail();
    const std::string path = current_path_;
    close_writer();
    between_tracks_ = true;
    note("Saved track %d: %s", track_, path.c_str());
}

void Recorder::Impl::tick_cb(pa_mainloop_api*, pa_time_event*, const struct timeval*, void* ud) {
    static_cast<Impl*>(ud)->tick();
}

void Recorder::Impl::tick() {
    if (stopping_) return;
    const pa_usec_t now = pa_rtclock_now();

    for (auto it = captures_.begin(); it != captures_.end();) {
        const uint32_t key = it->first;
        const bool dead = it->second->dead;
        ++it;
        if (dead) {
            close_capture(key, opts_.app_mode());
            need_rescan_ = true;
        }
    }

    if (now - last_retry_ >= kRetryUsec) {
        if (opts_.app_mode() && need_rescan_) {
            last_retry_ = now;
            rescan_sink_inputs();
        } else if (!opts_.app_mode() && captures_.empty()) {
            last_retry_ = now;
            refresh_system_sink();
        }
    }

    if (start_usec_) {
        flush_to(now_frames() - int64_t(kMixDelaySec * spec_.rate));
        if (silence_stop_ && !write_failed_) note("Stopping after %g seconds of silence.", opts_.stop_silence);
        if (write_failed_ || silence_stop_ || (limit_frames_ >= 0 && mixer_.flushed() >= limit_frames_)) {
            stop(write_failed_ ? 1 : 0);
            return;
        }
    }

    if (now - last_sync_ >= kSyncUsec) {
        last_sync_ = now;
        if (writer_) writer_->sync();
    }

    pa_context_rttime_restart(ctx_, tick_ev_, now + kTickUsec);
}

RecorderStatus Recorder::Impl::status() {
    RecorderStatus s;
    s.started = start_usec_ != 0;
    s.paused = paused_;
    s.seconds = double(now_frames()) / spec_.rate;
    if (opts_.duration > 0) s.seconds = std::min(s.seconds, opts_.duration);
    s.peak = peak_;
    peak_ = 0;
    s.track = split_frames_ ? track_ : 0;
    s.between_tracks = between_tracks_;
    for (const auto& [key, cap] : captures_)
        if (cap->stream && !cap->dead) ++s.streams;
    return s;
}

void Recorder::Impl::stop(int code) {
    if (!started_ || stopping_) return;
    stopping_ = true;
    if (start_usec_) flush_to(std::max(now_frames(), mixer_.buffered_end()));
    if (heard_ && !between_tracks_) write_tail();
    if (write_failed_) code = 1;

    while (!captures_.empty()) close_capture(captures_.begin()->first, false);
    close_writer();

    pa_context_set_state_callback(ctx_, nullptr, nullptr);
    pa_context_disconnect(ctx_);
    if (hooks_.finished) hooks_.finished(code, total_frames_);
}

// ---------------------------------------------------------------- public API

Recorder::Recorder(pa_mainloop_api* api, RecordOptions opts, Hooks hooks)
    : impl_(std::make_unique<Impl>(api, std::move(opts), std::move(hooks))) {}

Recorder::~Recorder() = default;

bool Recorder::start() { return impl_->start(); }
void Recorder::stop() { impl_->stop(0); }
void Recorder::pause() { impl_->pause(); }
void Recorder::resume() { impl_->resume(); }
RecorderStatus Recorder::status() { return impl_->status(); }
const RecordOptions& Recorder::options() const { return impl_->options(); }
const std::vector<std::string>& Recorder::files() const { return impl_->files(); }
