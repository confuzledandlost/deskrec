#include "audio_writer.hpp"

#include <algorithm>
#include <cctype>

std::unique_ptr<AudioWriter> make_writer(const EncoderOptions& opts, std::string* error) {
    switch (opts.format) {
    case Format::Wav:
        return make_wav_writer(opts);
    case Format::Flac:
#ifdef DESKREC_HAVE_FLAC
        return make_flac_writer(opts);
#else
        *error = "This build has no FLAC support (install the libFLAC dev package and rebuild).";
        return nullptr;
#endif
    case Format::Mp3:
#ifdef DESKREC_HAVE_MP3
        return make_mp3_writer(opts);
#else
        *error = "This build has no MP3 support (install the LAME dev package and rebuild).";
        return nullptr;
#endif
    }
    *error = "Unknown format.";
    return nullptr;
}

bool parse_format(const std::string& name, Format& out) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (n == "wav") out = Format::Wav;
    else if (n == "flac") out = Format::Flac;
    else if (n == "mp3") out = Format::Mp3;
    else return false;
    return true;
}

const char* format_name(Format f) {
    switch (f) {
    case Format::Wav: return "wav";
    case Format::Flac: return "flac";
    case Format::Mp3: return "mp3";
    }
    return "?";
}

bool format_available(Format f) {
    switch (f) {
    case Format::Wav: return true;
#ifdef DESKREC_HAVE_FLAC
    case Format::Flac: return true;
#endif
#ifdef DESKREC_HAVE_MP3
    case Format::Mp3: return true;
#endif
    default: return false;
    }
}

std::string available_formats() {
    std::string out;
    for (Format f : {Format::Wav, Format::Flac, Format::Mp3}) {
        if (!format_available(f)) continue;
        if (!out.empty()) out += ", ";
        out += format_name(f);
    }
    return out;
}
