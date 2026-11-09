#pragma once

#include "audio_writer.hpp"

#include <pulse/mainloop-api.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct RecordOptions {
    std::string output;
    std::vector<std::string> apps;   // case-insensitive substrings of app name / binary
    std::vector<uint32_t> indices;   // sink input (app stream) indices
    std::string sink;                // empty = follow the default output
    double duration = 0;             // seconds; 0 = until stopped
    uint32_t rate = 48000;
    uint8_t channels = 2;
    Format format = Format::Wav;
    bool high_precision = false;     // WAV: 32-bit float, FLAC: 24-bit
    int mp3_bitrate = 0;             // kbps; 0 = VBR

    bool app_mode() const { return !apps.empty() || !indices.empty(); }
};

struct RecorderStatus {
    bool started = false;  // false while waiting for the first audio
    bool paused = false;
    double seconds = 0;
    float peak = 0;        // linear peak since the previous status() call
    size_t streams = 0;    // capture streams currently delivering audio
};

// Records on a caller-provided PulseAudio main loop; hooks run on that loop.
class Recorder {
public:
    struct Hooks {
        std::function<void(const std::string&)> note;
        std::function<void(const std::string&)> error;
        // Runs once when recording ends (stop(), duration reached, or failure),
        // after the file is closed. frames == 0 means nothing was captured and
        // the file was deleted. Must not destroy the Recorder synchronously.
        std::function<void(int code, uint64_t frames)> finished;
    };

    Recorder(pa_mainloop_api* api, RecordOptions opts, Hooks hooks);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Opens the output file and connects to the sound server. On failure the
    // error hook has been called and `finished` won't be.
    bool start();
    void stop();
    // While paused, incoming audio is discarded and the timeline is frozen, so
    // the file continues seamlessly after resume().
    void pause();
    void resume();
    RecorderStatus status();
    const RecordOptions& options() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::string format_duration(double seconds);  // HH:MM:SS

// "<dir>/<label> - YYYY-MM-DD HH-MM.<ext>", with " (2)", " (3)", ... appended if
// that file already exists. An empty dir means the current directory.
std::string recording_path(const std::string& dir, const std::string& label, Format format);
