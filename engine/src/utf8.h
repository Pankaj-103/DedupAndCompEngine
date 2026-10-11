#pragma once

#include <filesystem>
#include <string>

// Explicit UTF-8 <-> path conversions. fs::path::string() and fs::path(std::string)
// use the platform's native narrow encoding, which with MSVC is the legacy ANSI
// code page; the index and the command line always use UTF-8.

inline std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

inline std::filesystem::path path_from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
