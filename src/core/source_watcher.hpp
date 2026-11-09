#pragma once

#include <pulse/pulseaudio.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct OutputInfo {
    uint32_t index;
    std::string name;
    std::string description;
    bool is_default;
};

struct AppStreamInfo {
    uint32_t index;
    std::string app;
    std::string binary;
    std::string icon;
    std::string media;
    uint32_t sink;
    bool corked;
};

// Tracks the sound server's outputs and the app streams playing on them.
class SourceWatcher {
public:
    struct Hooks {
        // A complete, consistent snapshot is available in outputs()/apps().
        std::function<void()> changed;
        std::function<void(const std::string&)> failed;
    };

    // With live = false, a single snapshot is taken and no updates follow.
    SourceWatcher(pa_mainloop_api* api, Hooks hooks, bool live);
    ~SourceWatcher();
    SourceWatcher(const SourceWatcher&) = delete;
    SourceWatcher& operator=(const SourceWatcher&) = delete;

    bool start();

    const std::vector<OutputInfo>& outputs() const { return outputs_; }
    const std::vector<AppStreamInfo>& apps() const { return apps_; }

private:
    static void state_cb(pa_context* c, void* ud);
    static void subscribe_cb(pa_context* c, pa_subscription_event_type_t type, uint32_t idx, void* ud);

    void refresh();

    pa_mainloop_api* api_;
    Hooks hooks_;
    bool live_;
    pa_context* ctx_ = nullptr;
    bool in_flight_ = false;
    bool dirty_ = false;

    std::string pending_default_;
    std::vector<OutputInfo> pending_outputs_;
    std::vector<AppStreamInfo> pending_apps_;
    std::vector<OutputInfo> outputs_;
    std::vector<AppStreamInfo> apps_;
};
