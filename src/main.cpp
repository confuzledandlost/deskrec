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

const char* kUsage = R"(deskrec - record desktop audio to WAV

Usage:
  deskrec list                List outputs and apps currently playing audio
  deskrec record [options]    Record (default: everything on the default output)

Record options:
  -o, --output FILE       WAV file to write (default: deskrec-YYYYMMDD-HHMMSS.wav)
  -a, --app NAME          Record only apps whose name or binary contains NAME
                          (case-insensitive, repeatable: -a firefox -a spotify)
  -i, --index N           Record only the app stream with index N (see 'list', repeatable)
  -s, --sink NAME         Record a specific output instead of the default one
  -d, --duration SECS     Stop automatically after SECS seconds of audio
  -r, --rate HZ           Sample rate (default: 48000)
  -c, --channels N        Channel count (default: 2)
  -f, --float             Write 32-bit float WAV instead of 16-bit PCM
  -q, --quiet             No status output
  -h, --help              Show this help

Recording starts when the first audio arrives. Stop it with Ctrl+C.
)";

std::string default_filename() {
    char buf[64];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "deskrec-%Y%m%d-%H%M%S.wav", std::localtime(&t));
    return buf;
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
        {"quiet", no_argument, nullptr, 'q'},         {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    RecordOptions opts;
    unsigned long n = 0;
    int c;
    while ((c = getopt_long(argc, argv, "o:a:i:s:d:r:c:fqh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'o': opts.output = optarg; break;
        case 'a': opts.apps.emplace_back(optarg); break;
        case 's': opts.sink = optarg; break;
        case 'f': opts.float32 = true; break;
        case 'q': opts.quiet = true; break;
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
        case 'h': std::fputs(kUsage, stdout); return 0;
        default: std::fputs(kUsage, stderr); return 2;
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
    if (opts.output.empty()) opts.output = default_filename();

    return run_recorder(opts);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "list") return run_list();
    if (cmd == "record") return cmd_record(argc - 1, argv + 1);
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        std::fputs(kUsage, stdout);
        return 0;
    }
    std::fprintf(stderr, "deskrec: unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
    return 2;
}
