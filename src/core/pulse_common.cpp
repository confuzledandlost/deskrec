#include "pulse_common.hpp"

#include <algorithm>
#include <cctype>

std::string get_prop(const pa_proplist* props, const char* key) {
    const char* v = props ? pa_proplist_gets(props, key) : nullptr;
    return v ? v : "";
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    return it != haystack.end();
}
