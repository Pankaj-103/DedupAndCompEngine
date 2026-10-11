#pragma once

#include <filesystem>
#include <string>

#include "db.h"

// A repository: config.json + index.db + chunks/ fan-out directories.
class Repo {
public:
    static void init(const std::filesystem::path& root);
    static Repo open(const std::filesystem::path& root);

    // repo/chunks/ab/abcdef... for hex chunk hash "abcdef..."
    std::filesystem::path chunk_path(const std::string& hex) const;
    std::filesystem::path temp_chunk_path() const;
    const std::filesystem::path& root() const { return root_; }
    Db& db() { return db_; }

private:
    Repo() = default;
    std::filesystem::path root_;
    Db db_;
};
