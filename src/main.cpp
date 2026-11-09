#include "list.hpp"
#include "recorder.hpp"

#include <getopt.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace {

const char* kUsage = R"(deskrec - record desktop audio to WAV, FLAC or MP3

Usage:
  deskrec list                List outputs and apps currently playing audio
  deskrec record [options]    Record (default: everything on the default output)

Record options:
  -o, --output FILE       File to write (default: deskrec-YYYYMMDD-HHMMSS.<format>)
  -F, --format FORMAT     wav (uncompressed), flac (lossless, ~half the size) or
                          mp3 (lossy, smallest). Default: taken from the -o
                          extension, otherwise wav
  -a, --app NAME          Record only apps whose name or binary contains NAME
                          (case-insensitive, repeatable: -a firefox -a spotify)
  -i, --index N           Record only the app stream with index N (see 'list', repeatable)
  -s, --sink NAME         Record a specific output instead of the default one
  -d, --duration SECS     Stop automatically after SECS seconds of audio
  -r, --rate HZ           Sample rate (default: 48000)
  -c, --channels N        Channel count (default: 2)
  -f, --float             Higher precision: 32-bit float WAV or 24-bit FLAC
                          (default: 16-bit)
  -b, --bitrate KBPS      MP3 constant bitrate, 8-320 (default: high-quality VBR,
                          around 190 kbps)
  -q, --quiet             No status output
  -h, --help              Show this help

Recording starts when the first audio arrives. Stop it with Ctrl+C.
)";

std::string default_filename(Format format) {
    char buf[64];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "deskrec-%Y%m%d-%H%M%S.", std::localtime(&t));
    return std::string(buf) + format_name(format);
}

bool format_from_extension(const std::string& path, Format& out) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
    return parse_format(path.substr(dot + 1), out);
}

void print_usage(std::FILE* f) {
    std::fputs(kUsage, f);
    std::fprintf(f, "Formats in this build: %s\n", available_formats().c_str());
}

bool parse_uint(const char* s, unsigned long max, unsigned long& out) {
    errno = 0;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (errno || end == s || *end || v > max) return false;
    out = v;
    return true;
}

int bad_arg(const char* opt, const char* value) {
    std::fprintf(stderr, "deskrec: invalid value for %s: '%s'\n", opt, value);
    return 2;
}

int cmd_record(int argc, char** argv) {
    static const option long_opts[] = {
        {"output", required_argument, nullptr, 'o'},  {"app", required_argument, nullptr, 'a'},
        {"index", required_argument, nullptr, 'i'},   {"sink", required_argument, nullptr, 's'},
        {"duration", required_argument, nullptr, 'd'}, {"rate", required_argument, nullptr, 'r'},
        {"channels", required_argument, nullptr, 'c'}, {"float", no_argument, nullptr, 'f'},
        {"format", required_argument, nullptr, 'F'},  {"bitrate", required_argument, nullptr, 'b'},
        {"quiet", no_argument, nullptr, 'q'},         {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    RecordOptions opts;
    bool format_given = false;
    unsigned long n = 0;
    int c;
    while ((c = getopt_long(argc, argv, "o:a:i:s:d:r:c:fF:b:qh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'o': opts.output = optarg; break;
        case 'a': opts.apps.emplace_back(optarg); break;
        case 's': opts.sink = optarg; break;
        case 'f': opts.high_precision = true; break;
        case 'q': opts.quiet = true; break;
        case 'F':
            if (!parse_format(optarg, opts.format)) return bad_arg("--format", optarg);
            format_given = true;
            break;
        case 'b':
            if (!parse_uint(optarg, 320, n) || n < 8) return bad_arg("--bitrate", optarg);
            opts.mp3_bitrate = int(n);
            break;
        case 'i':
            if (!parse_uint(optarg, 0xFFFFFFFEul, n)) return bad_arg("--index", optarg);
            opts.indices.push_back(uint32_t(n));
            break;
        case 'r':
            if (!parse_uint(optarg, 384000, n) || n < 8000) return bad_arg("--rate", optarg);
            opts.rate = uint32_t(n);
            break;
        case 'c':
            if (!parse_uint(optarg, 32, n) || n < 1) return bad_arg("--channels", optarg);
            opts.channels = uint8_t(n);
            break;
        case 'd': {
            char* end = nullptr;
            opts.duration = std::strtod(optarg, &end);
            if (end == optarg || *end || !(opts.duration > 0)) return bad_arg("--duration", optarg);
            break;
        }
        case 'h': print_usage(stdout); return 0;
        default: print_usage(stderr); return 2;
        }
    }
    if (optind < argc) {
        std::fprintf(stderr, "deskrec: unexpected argument '%s'\n", argv[optind]);
        return 2;
    }
    if (!opts.sink.empty() && opts.app_mode()) {
        std::fprintf(stderr, "deskrec: --sink can't be combined with --app/--index\n");
        return 2;
    }
    if (!format_given && !opts.output.empty()) format_from_extension(opts.output, opts.format);
    if (!format_available(opts.format)) {
        std::fprintf(stderr, "deskrec: this build can't write %s files (available: %s)\n",
                     format_name(opts.format), available_formats().c_str());
        return 2;
    }
    if (opts.mp3_bitrate && opts.format != Format::Mp3) {
        std::fprintf(stderr, "deskrec: --bitrate only applies to mp3\n");
        return 2;
    }
    if (opts.high_precision && opts.format == Format::Mp3) {
        std::fprintf(stderr, "deskrec: --float doesn't apply to mp3 (use --bitrate for quality)\n");
        return 2;
    }
    if (opts.output.empty()) opts.output = default_filename(opts.format);

    return run_recorder(opts);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "list") return run_list();
    if (cmd == "record") return cmd_record(argc - 1, argv + 1);
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        print_usage(stdout);
        return 0;
    }
    std::fprintf(stderr, "deskrec: unknown command '%s'\n\n", cmd.c_str());
    print_usage(stderr);
    return 2;
}
