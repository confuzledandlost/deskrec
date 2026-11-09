#include "list.hpp"

#include "pulse_common.hpp"

#include <pulse/pulseaudio.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

struct SinkRow {
    uint32_t index;
    std::string name;
    std::string description;
};

struct InputRow {
    uint32_t index;
    std::string app;
    std::string binary;
    std::string media;
    uint32_t sink;
    bool corked;
};

struct ListState {
    pa_mainloop* ml = nullptr;
    std::string default_sink;
    std::vector<SinkRow> sinks;
    std::vector<InputRow> inputs;
};

std::string fit(const std::string& s, size_t width) {
    if (s.size() <= width) return s;
    return s.substr(0, width - 3) + "...";
}

void print(const ListState& st) {
    std::map<uint32_t, std::string> sink_names;

    std::printf("Outputs  (record one with: deskrec record -s NAME; * = default)\n");
    for (const auto& s : st.sinks) {
        sink_names[s.index] = s.description;
        std::printf("  %c %-55s %s\n", s.name == st.default_sink ? '*' : ' ', s.name.c_str(),
                    s.description.c_str());
    }

    std::printf("\nApps playing audio  (record with: deskrec record -a APP  or  -i INDEX)\n");
    if (st.inputs.empty()) {
        std::printf("  (none right now - start playback in the app and run 'deskrec list' again)\n");
        return;
    }
    std::printf("  %-6s %-20s %-16s %-8s %-28s %s\n", "INDEX", "APP", "BINARY", "STATE", "OUTPUT", "MEDIA");
    for (const auto& i : st.inputs) {
        std::printf("  %-6u %-20s %-16s %-8s %-28s %s\n", i.index, fit(i.app, 20).c_str(),
                    fit(i.binary, 16).c_str(), i.corked ? "paused" : "playing",
                    fit(sink_names[i.sink], 28).c_str(), fit(i.media, 40).c_str());
    }
}

void server_info_cb(pa_context*, const pa_server_info* info, void* ud) {
    auto* st = static_cast<ListState*>(ud);
    if (info && info->default_sink_name) st->default_sink = info->default_sink_name;
}

void sink_cb(pa_context*, const pa_sink_info* info, int eol, void* ud) {
    auto* st = static_cast<ListState*>(ud);
    if (eol != 0) return;
    st->sinks.push_back({info->index, info->name, info->description ? info->description : ""});
}

void sink_input_cb(pa_context*, const pa_sink_input_info* info, int eol, void* ud) {
    auto* st = static_cast<ListState*>(ud);
    if (eol != 0) {
        // Replies arrive in request order, so this is the last one.
        print(*st);
        pa_mainloop_quit(st->ml, eol < 0 ? 1 : 0);
        return;
    }
    st->inputs.push_back({info->index, get_prop(info->proplist, PA_PROP_APPLICATION_NAME),
                          get_prop(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY),
                          info->name ? info->name : "", info->sink, info->corked != 0});
}

void context_state_cb(pa_context* c, void* ud) {
    auto* st = static_cast<ListState*>(ud);
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        pa_operation_unref(pa_context_get_server_info(c, server_info_cb, st));
        pa_operation_unref(pa_context_get_sink_info_list(c, sink_cb, st));
        pa_operation_unref(pa_context_get_sink_input_info_list(c, sink_input_cb, st));
        break;
    case PA_CONTEXT_FAILED:
        std::fprintf(stderr, "Could not connect to the sound server: %s\n", pa_strerror(pa_context_errno(c)));
        pa_mainloop_quit(st->ml, 1);
        break;
    default:
        break;
    }
}

}  // namespace

int run_list() {
    ListState st;
    st.ml = pa_mainloop_new();
    pa_context* ctx = pa_context_new(pa_mainloop_get_api(st.ml), "deskrec");
    pa_context_set_state_callback(ctx, context_state_cb, &st);

    int ret = 1;
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
        std::fprintf(stderr, "Could not connect to the sound server: %s\n", pa_strerror(pa_context_errno(ctx)));
    else
        pa_mainloop_run(st.ml, &ret);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(st.ml);
    return ret;
}
