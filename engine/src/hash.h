#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

using Hash256 = std::array<uint8_t, 32>;

std::string to_hex(const Hash256& h);

// Streams the file through BLAKE3; throws std::runtime_error on I/O failure.
Hash256 hash_file(const std::filesystem::path& p);
