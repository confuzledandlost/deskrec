#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RecordOptions {
    std::string output;
    std::vector<std::string> apps;   // case-insensitive substrings of app name / binary
    std::vector<uint32_t> indices;   // sink input (app stream) indices
    std::string sink;                // empty = follow the default output
    double duration = 0;             // seconds; 0 = until interrupted
    uint32_t rate = 48000;
    uint8_t channels = 2;
    bool float32 = false;
    bool quiet = false;

    bool app_mode() const { return !apps.empty() || !indices.empty(); }
};

int run_recorder(const RecordOptions& opts);
