#include "audio_writer.hpp"

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Lossless FLAC: 16-bit, or 24-bit in high-precision mode.
class FlacWriter : public AudioWriter {
public:
    explicit FlacWriter(const EncoderOptions& opts)
        : rate_(opts.rate), channels_(opts.channels), bits_(opts.high_precision ? 24 : 16) {}
    FlacWriter(const FlacWriter&) = delete;
    FlacWriter& operator=(const FlacWriter&) = delete;
    ~FlacWriter() override { close(); }

    bool open(const std::string& path) override {
        close();
        enc_ = FLAC__stream_encoder_new();
        if (!enc_) {
            error_ = "out of memory";
            return false;
        }
        FLAC__stream_encoder_set_channels(enc_, channels_);
        FLAC__stream_encoder_set_bits_per_sample(enc_, bits_);
        FLAC__stream_encoder_set_sample_rate(enc_, rate_);
        FLAC__stream_encoder_set_compression_level(enc_, 5);

        const auto status = FLAC__stream_encoder_init_file(enc_, path.c_str(), nullptr, nullptr);
        if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            error_ = status == FLAC__STREAM_ENCODER_INIT_STATUS_ENCODER_ERROR
                         ? FLAC__stream_encoder_get_resolved_state_string(enc_)
                         : FLAC__StreamEncoderInitStatusString[status];
            FLAC__stream_encoder_delete(enc_);
            enc_ = nullptr;
            return false;
        }
        frames_ = 0;
        return true;
    }

    bool write(const float* samples, size_t frames) override {
        if (!enc_) return false;
        const size_t count = frames * channels_;
        const float scale = float((1 << (bits_ - 1)) - 1);
        buf_.resize(count);
        for (size_t i = 0; i < count; ++i)
            buf_[i] = FLAC__int32(std::lrintf(std::clamp(samples[i], -1.0f, 1.0f) * scale));

        if (!FLAC__stream_encoder_process_interleaved(enc_, buf_.data(), unsigned(frames))) {
            error_ = FLAC__stream_encoder_get_resolved_state_string(enc_);
            return false;
        }
        frames_ += frames;
        return true;
    }

    void close() override {
        if (!enc_) return;
        FLAC__stream_encoder_finish(enc_);
        FLAC__stream_encoder_delete(enc_);
        enc_ = nullptr;
    }

private:
    FLAC__StreamEncoder* enc_ = nullptr;
    uint32_t rate_;
    uint16_t channels_;
    unsigned bits_;
    std::vector<FLAC__int32> buf_;
};

}  // namespace

std::unique_ptr<AudioWriter> make_flac_writer(const EncoderOptions& opts) {
    return std::make_unique<FlacWriter>(opts);
}
