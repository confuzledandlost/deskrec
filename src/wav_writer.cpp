#include "wav_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace {

void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

void put_tag(std::vector<uint8_t>& b, const char* tag) {
    b.insert(b.end(), tag, tag + 4);
}

uint32_t clamp32(uint64_t v) {
    // RIFF sizes are 32-bit; past ~4 GiB the header saturates and players read to EOF.
    return v > 0xFFFFFFF0u ? 0xFFFFFFF0u : uint32_t(v);
}

}  // namespace

std::unique_ptr<AudioWriter> make_wav_writer(const EncoderOptions& opts) {
    return std::make_unique<WavWriter>(opts);
}

WavWriter::WavWriter(const EncoderOptions& opts)
    : rate_(opts.rate), channels_(opts.channels), float32_(opts.high_precision) {}

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::string& path) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        error_ = std::strerror(errno);
        return false;
    }
    frames_ = 0;
    write_header();
    return std::ferror(file_) == 0;
}

void WavWriter::write_header() {
    const uint16_t bytes_per_sample = float32_ ? 4 : 2;
    const uint16_t block_align = uint16_t(channels_ * bytes_per_sample);
    const uint32_t data_bytes = clamp32(frames_ * block_align);
    const uint32_t fmt_size = float32_ ? 18 : 16;
    const uint32_t fact_chunk = float32_ ? 12 : 0;

    std::vector<uint8_t> h;
    put_tag(h, "RIFF");
    put32(h, clamp32(uint64_t(4) + 8 + fmt_size + fact_chunk + 8 + data_bytes));
    put_tag(h, "WAVE");

    put_tag(h, "fmt ");
    put32(h, fmt_size);
    put16(h, float32_ ? 3 : 1);
    put16(h, channels_);
    put32(h, rate_);
    put32(h, rate_ * block_align);
    put16(h, block_align);
    put16(h, uint16_t(bytes_per_sample * 8));
    if (float32_) {
        put16(h, 0);
        put_tag(h, "fact");
        put32(h, 4);
        put32(h, clamp32(frames_));
    }

    put_tag(h, "data");
    put32(h, data_bytes);

    std::fseek(file_, 0, SEEK_SET);
    std::fwrite(h.data(), 1, h.size(), file_);
    std::fseek(file_, 0, SEEK_END);
}

bool WavWriter::write(const float* samples, size_t frames) {
    if (!file_ || frames == 0) return file_ != nullptr;
    const size_t count = frames * channels_;
    scratch_.clear();
    scratch_.reserve(count * (float32_ ? 4 : 2));

    for (size_t i = 0; i < count; ++i) {
        if (float32_) {
            uint32_t bits;
            std::memcpy(&bits, &samples[i], 4);
            put32(scratch_, bits);
        } else {
            const float s = std::clamp(samples[i], -1.0f, 1.0f);
            put16(scratch_, uint16_t(int16_t(std::lrintf(s * 32767.0f))));
        }
    }

    frames_ += frames;
    if (std::fwrite(scratch_.data(), 1, scratch_.size(), file_) != scratch_.size()) {
        error_ = std::strerror(errno);
        return false;
    }
    return true;
}

void WavWriter::sync() {
    if (!file_) return;
    write_header();
    std::fflush(file_);
}

void WavWriter::close() {
    if (!file_) return;
    write_header();
    std::fclose(file_);
    file_ = nullptr;
}
