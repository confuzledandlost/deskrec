#include "audio_writer.hpp"

#include <lame/lame.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Lossy MP3 via LAME: high-quality VBR by default, or a constant bitrate.
class Mp3Writer : public AudioWriter {
public:
    explicit Mp3Writer(const EncoderOptions& opts)
        : rate_(opts.rate), channels_(opts.channels), bitrate_(opts.mp3_bitrate), tags_(opts.tags) {}
    Mp3Writer(const Mp3Writer&) = delete;
    Mp3Writer& operator=(const Mp3Writer&) = delete;
    ~Mp3Writer() override { close(); }

    bool open(const std::string& path) override {
        close();
        if (channels_ > 2) {
            error_ = "MP3 supports only mono or stereo (use -c 1 or -c 2)";
            return false;
        }

        lame_ = lame_init();
        if (!lame_) {
            error_ = "out of memory";
            return false;
        }
        lame_set_in_samplerate(lame_, int(rate_));
        lame_set_num_channels(lame_, channels_);
        lame_set_mode(lame_, channels_ == 1 ? MONO : JOINT_STEREO);
        lame_set_quality(lame_, 2);
        if (bitrate_ > 0) {
            lame_set_VBR(lame_, vbr_off);
            lame_set_brate(lame_, bitrate_);
        } else {
            lame_set_VBR(lame_, vbr_default);
            lame_set_VBR_quality(lame_, 2.0f);
        }

        // LAME puts the ID3v2 tag at the front of the first encoded output.
        id3tag_init(lame_);
        id3tag_v2_only(lame_);
        if (!tags_.title.empty()) id3tag_set_title(lame_, tags_.title.c_str());
        if (!tags_.artist.empty()) id3tag_set_artist(lame_, tags_.artist.c_str());
        if (tags_.date.size() >= 4) id3tag_set_year(lame_, tags_.date.substr(0, 4).c_str());
        if (!tags_.comment.empty()) id3tag_set_comment(lame_, tags_.comment.c_str());
        if (tags_.track > 0) id3tag_set_track(lame_, std::to_string(tags_.track).c_str());

        if (lame_init_params(lame_) < 0) {
            error_ = "the MP3 encoder rejected this sample rate / bitrate combination";
            lame_close(lame_);
            lame_ = nullptr;
            return false;
        }

        // "w+b": LAME re-reads the start of the file to fill in the VBR header on close.
        file_ = std::fopen(path.c_str(), "w+b");
        if (!file_) {
            error_ = std::strerror(errno);
            lame_close(lame_);
            lame_ = nullptr;
            return false;
        }
        frames_ = 0;
        return true;
    }

    bool write(const float* samples, size_t frames) override {
        if (!lame_) return false;
        out_.resize(frames * 5 / 4 + 7200);
        const int n = channels_ == 2
            ? lame_encode_buffer_interleaved_ieee_float(lame_, samples, int(frames), out_.data(), int(out_.size()))
            : lame_encode_buffer_ieee_float(lame_, samples, samples, int(frames), out_.data(), int(out_.size()));
        if (n < 0) {
            error_ = "MP3 encoding failed";
            return false;
        }
        frames_ += frames;
        if (std::fwrite(out_.data(), 1, size_t(n), file_) != size_t(n)) {
            error_ = std::strerror(errno);
            return false;
        }
        return true;
    }

    void sync() override {
        if (file_) std::fflush(file_);
    }

    void close() override {
        if (!lame_) return;
        out_.resize(7200);
        const int n = lame_encode_flush(lame_, out_.data(), int(out_.size()));
        if (n > 0) std::fwrite(out_.data(), 1, size_t(n), file_);
        lame_mp3_tags_fid(lame_, file_);
        lame_close(lame_);
        lame_ = nullptr;
        std::fclose(file_);
        file_ = nullptr;
    }

private:
    lame_t lame_ = nullptr;
    std::FILE* file_ = nullptr;
    uint32_t rate_;
    uint16_t channels_;
    int bitrate_;
    Tags tags_;
    std::vector<unsigned char> out_;
};

}  // namespace

std::unique_ptr<AudioWriter> make_mp3_writer(const EncoderOptions& opts) {
    return std::make_unique<Mp3Writer>(opts);
}
