#include "source_watcher.hpp"

#include "pulse_common.hpp"

#include <pulse/pulseaudio.h>

SourceWatcher::SourceWatcher(pa_mainloop_api* api, Hooks hooks, bool live)
    : api_(api), hooks_(std::move(hooks)), live_(live) {}

SourceWatcher::~SourceWatcher() {
    if (!ctx_) return;
    pa_context_set_state_callback(ctx_, nullptr, nullptr);
    pa_context_set_subscribe_callback(ctx_, nullptr, nullptr);
    pa_context_disconnect(ctx_);
    pa_context_unref(ctx_);
}

bool SourceWatcher::start() {
    ctx_ = pa_context_new(api_, "deskrec");
    pa_context_set_state_callback(ctx_, state_cb, this);
    if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        if (hooks_.failed) hooks_.failed(pa_strerror(pa_context_errno(ctx_)));
        return false;
    }
    return true;
}

void SourceWatcher::state_cb(pa_context* c, void* ud) {
    auto* self = static_cast<SourceWatcher*>(ud);
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        if (self->live_) {
            pa_context_set_subscribe_callback(c, subscribe_cb, self);
            const auto mask = pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT |
                                                     PA_SUBSCRIPTION_MASK_SERVER);
            pa_operation_unref(pa_context_subscribe(c, mask, nullptr, nullptr));
        }
        self->refresh();
        break;
    case PA_CONTEXT_FAILED:
        if (self->hooks_.failed) self->hooks_.failed(pa_strerror(pa_context_errno(c)));
        break;
    default:
        break;
    }
}

void SourceWatcher::subscribe_cb(pa_context*, pa_subscription_event_type_t, uint32_t, void* ud) {
    static_cast<SourceWatcher*>(ud)->refresh();
}

void SourceWatcher::refresh() {
    if (in_flight_) {
        dirty_ = true;
        return;
    }
    in_flight_ = true;
    pending_default_.clear();
    pending_outputs_.clear();
    pending_apps_.clear();

    // Replies arrive in request order, so the sink input list completes last.
    pa_operation_unref(pa_context_get_server_info(
        ctx_,
        [](pa_context*, const pa_server_info* info, void* ud) {
            auto* self = static_cast<SourceWatcher*>(ud);
            if (info && info->default_sink_name) self->pending_default_ = info->default_sink_name;
        },
        this));

    pa_operation_unref(pa_context_get_sink_info_list(
        ctx_,
        [](pa_context*, const pa_sink_info* info, int eol, void* ud) {
            if (eol != 0) return;
            auto* self = static_cast<SourceWatcher*>(ud);
            self->pending_outputs_.push_back({info->index, info->name, info->description ? info->description : "",
                                              info->name == self->pending_default_});
        },
        this));

    pa_operation_unref(pa_context_get_sink_input_info_list(
        ctx_,
        [](pa_context*, const pa_sink_input_info* info, int eol, void* ud) {
            auto* self = static_cast<SourceWatcher*>(ud);
            if (eol == 0) {
                self->pending_apps_.push_back({info->index, get_prop(info->proplist, PA_PROP_APPLICATION_NAME),
                                               get_prop(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY),
                                               get_prop(info->proplist, PA_PROP_APPLICATION_ICON_NAME),
                                               info->name ? info->name : "", info->sink, info->corked != 0});
                return;
            }
            self->in_flight_ = false;
            if (eol > 0) {
                self->outputs_.swap(self->pending_outputs_);
                self->apps_.swap(self->pending_apps_);
                if (self->hooks_.changed) self->hooks_.changed();
            }
            if (self->dirty_ && self->live_) {
                self->dirty_ = false;
                self->refresh();
            }
        },
        this));
}
