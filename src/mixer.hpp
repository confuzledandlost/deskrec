#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Sums several capture streams onto one shared timeline. Positions are absolute
// frame numbers; everything before flushed() has already been handed out.
class Mixer {
public:
    using Sink = std::function<void(const float* samples, size_t frames)>;

    explicit Mixer(uint32_t channels) : channels_(channels) {}

    // Mixes `frames` frames in at `cursor` and advances it. Audio that lands
    // before flushed() is too late and is dropped.
    void add(int64_t& cursor, const float* samples, size_t frames);

    // Emits all frames before `upto`; stretches no stream covered come out silent.
    void flush(int64_t upto, const Sink& out);

    int64_t flushed() const { return flushed_; }
    int64_t buffered_end() const { return flushed_ + int64_t(buf_.size() / channels_); }

private:
    uint32_t channels_;
    int64_t flushed_ = 0;
    std::vector<float> buf_;
    std::vector<float> silence_;
};
