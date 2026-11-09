#pragma once

#include <pulse/proplist.h>

#include <string>

std::string get_prop(const pa_proplist* props, const char* key);
bool contains_ci(const std::string& haystack, const std::string& needle);
