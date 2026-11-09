#include "recorder.hpp"

#include "mixer.hpp"
#include "pulse_common.hpp"
#include "wav_writer.hpp"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <unistd.h>

namespace {

constexpr pa_usec_t kTickUsec = 100 * 1000;
constexpr pa_usec_t kFragmentUsec = 20 * 1000;
constexpr pa_usec_t kStatusUsec = 250 * 1000;
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

constexpr uint32_t kSystemKey = PA_INVALID_INDEX;

class Recorder;

struct Capture {
    Recorder* owner = nullptr;
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
    Recorder* owner;
    uint32_t key;
    uint32_t sink;
};

class Recorder {
public:
    explicit Recorder(const RecordOptions& opts) : opts_(opts), mixer_(opts.channels) {}
    int run();

private:
    static void context_state_cb(pa_context* c, void* ud);
    static void subscribe_cb(pa_context* c, pa_subscription_event_type_t t, uint32_t idx, void* ud);
    static void server_info_cb(pa_context* c, const pa_server_info* info, void* ud);
    static void system_sink_cb(pa_context* c, const pa_sink_info* info, int eol, void* ud);
    static void sink_input_cb(pa_context* c, const pa_sink_input_info* info, int eol, void* ud);
    static void app_sink_cb(pa_context* c, const pa_sink_info* info, int eol, void* ud);
    static void stream_state_cb(pa_stream* s, void* ud);
    static void stream_read_cb(pa_stream* s, size_t nbytes, void* ud);
    static void tick_cb(pa_mainloop_api* api, pa_time_event* e, const struct timeval* tv, void* ud);
    static void signal_cb(pa_mainloop_api* api, pa_signal_event* e, int sig, void* ud);

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
    void tick();
    void stop(int code);
    void print_status();
    void note(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void vlog(const char* fmt, va_list ap);

    RecordOptions opts_;
    pa_mainloop* ml_ = nullptr;
    pa_mainloop_api* api_ = nullptr;
    pa_context* ctx_ = nullptr;
    pa_time_event* tick_ev_ = nullptr;
    pa_sample_spec spec_{};
    pa_channel_map map_{};

    std::map<uint32_t, std::unique_ptr<Capture>> captures_;
    Mixer mixer_;
    WavWriter wav_;

    pa_usec_t start_usec_ = 0;  // 0 until the first audio arrives
    int64_t limit_frames_ = -1;
    float peak_ = 0;
    pa_usec_t last_status_ = 0;
    pa_usec_t last_sync_ = 0;
    pa_usec_t last_retry_ = 0;
    bool need_rescan_ = false;
    bool lookup_pending_ = false;
    bool waiting_shown_ = false;
    bool stopping_ = false;
    bool write_failed_ = false;
    bool status_tty_ = false;
    bool status_dirty_ = false;
};

// ---------------------------------------------------------------- logging

void Recorder::vlog(const char* fmt, va_list ap) {
    if (status_dirty_) {
        std::fputs("\r\033[K", stderr);
        status_dirty_ = false;
    }
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

void Recorder::note(const char* fmt, ...) {
    if (opts_.quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void Recorder::error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

std::string format_time(double seconds) {
    const long total = long(seconds);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
    return buf;
}

// ---------------------------------------------------------------- setup

int Recorder::run() {
    spec_.format = PA_SAMPLE_FLOAT32NE;
    spec_.rate = opts_.rate;
    spec_.channels = opts_.channels;
    if (!pa_sample_spec_valid(&spec_)) {
        error("Invalid sample rate or channel count.");
        return 1;
    }
    pa_channel_map_init_extend(&map_, spec_.channels, PA_CHANNEL_MAP_DEFAULT);
    if (opts_.duration > 0) limit_frames_ = std::llround(opts_.duration * opts_.rate);
    status_tty_ = !opts_.quiet && isatty(STDERR_FILENO);

    if (!wav_.open(opts_.output, opts_.rate, opts_.channels, opts_.float32)) {
        error("Cannot open %s for writing.", opts_.output.c_str());
        return 1;
    }

    ml_ = pa_mainloop_new();
    api_ = pa_mainloop_get_api(ml_);
    pa_signal_init(api_);
    pa_signal_new(SIGINT, signal_cb, this);
    pa_signal_new(SIGTERM, signal_cb, this);

    pa_proplist* props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_ID, "deskrec");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ICON_NAME, "audio-input-microphone");
    ctx_ = pa_context_new_with_proplist(api_, "deskrec", props);
    pa_proplist_free(props);
    pa_context_set_state_callback(ctx_, context_state_cb, this);

    int ret = 1;
    if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        error("Could not connect to the sound server: %s", pa_strerror(pa_context_errno(ctx_)));
        wav_.close();
        std::remove(opts_.output.c_str());
    } else {
        pa_mainloop_run(ml_, &ret);
    }

    if (tick_ev_) api_->time_free(tick_ev_);
    pa_context_unref(ctx_);
    pa_signal_done();
    pa_mainloop_free(ml_);
    return ret;
}

void Recorder::context_state_cb(pa_context* c, void* ud) {
    auto* self = static_cast<Recorder*>(ud);
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

void Recorder::on_ready() {
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

void Recorder::refresh_system_sink() {
    if (lookup_pending_) return;
    lookup_pending_ = true;
    if (!opts_.sink.empty())
        pa_operation_unref(pa_context_get_sink_info_by_name(ctx_, opts_.sink.c_str(), system_sink_cb, this));
    else
        pa_operation_unref(pa_context_get_server_info(ctx_, server_info_cb, this));
}

void Recorder::server_info_cb(pa_context* c, const pa_server_info* info, void* ud) {
    auto* self = static_cast<Recorder*>(ud);
    if (!info || !info->default_sink_name) {
        self->lookup_pending_ = false;
        return;
    }
    pa_operation_unref(pa_context_get_sink_info_by_name(c, info->default_sink_name, system_sink_cb, self));
}

void Recorder::system_sink_cb(pa_context*, const pa_sink_info* info, int eol, void* ud) {
    auto* self = static_cast<Recorder*>(ud);
    if (eol != 0) {
        self->lookup_pending_ = false;
        if (eol < 0 && !self->opts_.sink.empty() && self->captures_.empty() && !self->start_usec_) {
            self->error("No output named '%s'. Run 'deskrec list' to see available outputs.",
                        self->opts_.sink.c_str());
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

void Recorder::rescan_sink_inputs() {
    need_rescan_ = false;
    pa_operation_unref(pa_context_get_sink_input_info_list(ctx_, sink_input_cb, this));
}

bool Recorder::matches(const pa_sink_input_info* info) const {
    if (std::find(opts_.indices.begin(), opts_.indices.end(), info->index) != opts_.indices.end())
        return true;
    const std::string app = get_prop(info->proplist, PA_PROP_APPLICATION_NAME);
    const std::string binary = get_prop(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    for (const auto& pattern : opts_.apps)
        if (contains_ci(app, pattern) || contains_ci(binary, pattern)) return true;
    return false;
}

void Recorder::sink_input_cb(pa_context*, const pa_sink_input_info* info, int eol, void* ud) {
    auto* self = static_cast<Recorder*>(ud);
    if (eol != 0) {
        if (eol > 0 && self->captures_.empty() && !self->waiting_shown_) {
            self->waiting_shown_ = true;
            self->note("Waiting for a matching app to play audio... (Ctrl+C to stop)");
        }
        return;
    }
    if (!self->stopping_) self->consider_sink_input(info);
}

void Recorder::consider_sink_input(const pa_sink_input_info* info) {
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

void Recorder::app_sink_cb(pa_context*, const pa_sink_info* info, int eol, void* ud) {
    auto* lookup = static_cast<SinkLookup*>(ud);
    Recorder* self = lookup->owner;
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

void Recorder::subscribe_cb(pa_context* c, pa_subscription_event_type_t t, uint32_t idx, void* ud) {
    auto* self = static_cast<Recorder*>(ud);
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

void Recorder::open_stream(Capture& c, const char* monitor_source) {
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

void Recorder::close_capture(uint32_t key, bool announce) {
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

void Recorder::stream_state_cb(pa_stream* s, void* ud) {
    auto* c = static_cast<Capture*>(ud);
    const auto state = pa_stream_get_state(s);
    if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
        // Cleanup is deferred to tick(): unref'ing a stream inside its own callback is unsafe.
        c->dead = true;
    }
}

void Recorder::stream_read_cb(pa_stream* s, size_t, void* ud) {
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

int64_t Recorder::now_frames() const {
    if (!start_usec_) return 0;
    return int64_t((pa_rtclock_now() - start_usec_) * spec_.rate / PA_USEC_PER_SEC);
}

void Recorder::on_data(Capture& c, const float* data, size_t frames, size_t queued_after) {
    if (stopping_ || frames == 0) return;
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

void Recorder::flush_to(int64_t upto) {
    if (limit_frames_ >= 0) upto = std::min(upto, limit_frames_);
    mixer_.flush(upto, [this](const float* samples, size_t frames) {
        if (!write_failed_ && !wav_.write(samples, frames)) {
            write_failed_ = true;
            error("Write to %s failed (disk full?).", opts_.output.c_str());
        }
    });
}

void Recorder::tick_cb(pa_mainloop_api*, pa_time_event*, const struct timeval*, void* ud) {
    static_cast<Recorder*>(ud)->tick();
}

void Recorder::tick() {
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
        if (write_failed_ || (limit_frames_ >= 0 && mixer_.flushed() >= limit_frames_)) {
            stop(write_failed_ ? 1 : 0);
            return;
        }
    }

    if (now - last_sync_ >= kSyncUsec) {
        last_sync_ = now;
        wav_.sync();
    }
    if (status_tty_ && now - last_status_ >= kStatusUsec) {
        last_status_ = now;
        print_status();
    }

    pa_context_rttime_restart(ctx_, tick_ev_, now + kTickUsec);
}

void Recorder::print_status() {
    constexpr int kWidth = 24;
    const float db = peak_ > 0 ? 20.0f * std::log10(peak_) : -100.0f;
    peak_ = 0;
    const int bars = std::clamp(int((db + 60.0f) / 60.0f * kWidth + 0.5f), 0, kWidth);
    const std::string meter = std::string(size_t(bars), '#') + std::string(size_t(kWidth - bars), '.');

    size_t live = 0;
    for (const auto& [key, cap] : captures_)
        if (cap->stream && !cap->dead) ++live;

    if (!start_usec_) {
        std::fprintf(stderr, "\r\033[K  waiting for audio...  [%zu stream%s]", live, live == 1 ? "" : "s");
    } else {
        double secs = double(now_frames()) / spec_.rate;
        if (opts_.duration > 0) secs = std::min(secs, opts_.duration);
        std::fprintf(stderr, "\r\033[K  REC %s  [%s] %5.1f dB  [%zu stream%s]", format_time(secs).c_str(),
                     meter.c_str(), double(std::max(db, -99.9f)), live, live == 1 ? "" : "s");
    }
    std::fflush(stderr);
    status_dirty_ = true;
}

void Recorder::signal_cb(pa_mainloop_api*, pa_signal_event*, int, void* ud) {
    static_cast<Recorder*>(ud)->stop(0);
}

void Recorder::stop(int code) {
    if (stopping_) return;
    stopping_ = true;
    if (start_usec_) flush_to(std::max(now_frames(), mixer_.buffered_end()));
    if (write_failed_) code = 1;

    while (!captures_.empty()) close_capture(captures_.begin()->first, false);

    const uint64_t frames = wav_.frames_written();
    wav_.close();
    if (frames == 0) {
        std::remove(opts_.output.c_str());
        note("No audio was captured; nothing saved.");
    } else {
        note("Saved %s of audio to %s", format_time(double(frames) / spec_.rate).c_str(), opts_.output.c_str());
    }

    pa_context_disconnect(ctx_);
    pa_mainloop_quit(ml_, code);
}

}  // namespace

int run_recorder(const RecordOptions& opts) {
    Recorder recorder(opts);
    return recorder.run();
}
