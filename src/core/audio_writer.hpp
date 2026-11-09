#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class Format { Wav, Flac, Mp3 };

// Written as Vorbis comments (FLAC) or ID3v2 (MP3); WAV files get none.
// Empty fields are left out.
struct Tags {
    std::string title;
    std::string artist;
    std::string date;     // YYYY-MM-DD
    std::string comment;
    int track = 0;        // 0 = none
};

struct EncoderOptions {
    Format format = Format::Wav;
    uint32_t rate = 48000;
    uint16_t channels = 2;
    bool high_precision = false;  // WAV: 32-bit float, FLAC: 24-bit
    int mp3_bitrate = 0;          // kbps; 0 = high-quality VBR
    Tags tags;
};

// Encodes interleaved float samples (nominal range -1..1) into a file.
class AudioWriter {
public:
    virtual ~AudioWriter() = default;

    virtual bool open(const std::string& path) = 0;
    virtual bool write(const float* samples, size_t frames) = 0;
    // Pushes buffered data to disk so a killed process leaves a usable file.
    virtual void sync() {}
    virtual void close() = 0;

    uint64_t frames_written() const { return frames_; }
    const std::string& error() const { return error_; }

protected:
    uint64_t frames_ = 0;
    std::string error_;
};

// Returns nullptr (and sets *error) if the format isn't supported by this build.
std::unique_ptr<AudioWriter> make_writer(const EncoderOptions& opts, std::string* error);

bool parse_format(const std::string& name, Format& out);
const char* format_name(Format f);
bool format_available(Format f);
// Comma-separated list of the formats compiled into this build.
std::string available_formats();

std::unique_ptr<AudioWriter> make_wav_writer(const EncoderOptions& opts);
#ifdef DESKREC_HAVE_FLAC
std::unique_ptr<AudioWriter> make_flac_writer(const EncoderOptions& opts);
#endif
#ifdef DESKREC_HAVE_MP3
std::unique_ptr<AudioWriter> make_mp3_writer(const EncoderOptions& opts);
#endif
