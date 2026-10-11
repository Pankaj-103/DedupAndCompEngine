#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>

// Chunk storage codecs, persisted in the index — values must never change.
enum class Codec : int { None = 0, Zstd = 1 };

// Streams src through zstd into dst (truncating dst). Returns compressed size.
uint64_t zstd_compress_file(const std::filesystem::path& src,
                            const std::filesystem::path& dst, int level);

// Streams zstd-compressed src, appending decompressed bytes to out.
void zstd_decompress_file(const std::filesystem::path& src, std::ofstream& out);

// Appends raw bytes of src to out.
void append_raw_file(const std::filesystem::path& src, std::ofstream& out);
