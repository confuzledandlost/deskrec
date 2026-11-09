#pragma once

#include "audio_writer.hpp"

#include <cstdio>
#include <vector>

// Uncompressed WAV: 16-bit PCM, or 32-bit float in high-precision mode.
class WavWriter : public AudioWriter {
public:
    explicit WavWriter(const EncoderOptions& opts);
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    ~WavWriter() override;

    bool open(const std::string& path) override;
    bool write(const float* samples, size_t frames) override;
    // Rewrites the header so the file is playable even if the process is killed.
    void sync() override;
    void close() override;

private:
    void write_header();

    std::FILE* file_ = nullptr;
    uint32_t rate_;
    uint16_t channels_;
    bool float32_;
    std::vector<uint8_t> scratch_;
};
