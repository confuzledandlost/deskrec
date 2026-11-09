#include "mixer.hpp"

#include <algorithm>

void Mixer::add(int64_t& cursor, const float* samples, size_t frames) {
    if (cursor < flushed_) {
        const size_t late = size_t(flushed_ - cursor);
        if (late >= frames) {
            cursor += int64_t(frames);
            return;
        }
        samples += late * channels_;
        frames -= late;
        cursor = flushed_;
    }

    const size_t offset = size_t(cursor - flushed_) * channels_;
    const size_t count = frames * channels_;
    if (buf_.size() < offset + count) buf_.resize(offset + count, 0.0f);

    float* dst = buf_.data() + offset;
    for (size_t i = 0; i < count; ++i) dst[i] += samples[i];
    cursor += int64_t(frames);
}

void Mixer::flush(int64_t upto, const Sink& out) {
    if (upto <= flushed_) return;

    const size_t want = size_t(upto - flushed_);
    const size_t have = std::min(want, buf_.size() / channels_);
    if (have > 0) {
        out(buf_.data(), have);
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(have * channels_));
    }
    if (const size_t gap = want - have; gap > 0) {
        silence_.assign(gap * channels_, 0.0f);
        out(silence_.data(), gap);
    }
    flushed_ = upto;
}
