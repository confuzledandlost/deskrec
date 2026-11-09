#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Streams interleaved float samples into a WAV file (16-bit PCM or 32-bit float).
class WavWriter {
public:
    WavWriter() = default;
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    ~WavWriter();

    bool open(const std::string& path, uint32_t rate, uint16_t channels, bool float32);
    bool write(const float* samples, size_t frames);
    // Rewrites the header so the file is playable even if the process is killed.
    void sync();
    void close();

    bool is_open() const { return file_ != nullptr; }
    uint64_t frames_written() const { return frames_; }

private:
    void write_header();

    std::FILE* file_ = nullptr;
    uint32_t rate_ = 0;
    uint16_t channels_ = 0;
    bool float32_ = false;
    uint64_t frames_ = 0;
    std::vector<uint8_t> scratch_;
};
