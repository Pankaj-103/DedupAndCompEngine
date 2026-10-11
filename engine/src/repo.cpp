#include "repo.h"

#include <fstream>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS chunks (
    hash        BLOB PRIMARY KEY,
    raw_size    INTEGER NOT NULL,
    stored_size INTEGER NOT NULL,
    codec       INTEGER NOT NULL,
    pack_id     INTEGER,
    pack_offset INTEGER,
    refcount    INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS jobs (
    id           INTEGER PRIMARY KEY,
    started_at   INTEGER NOT NULL,
    finished_at  INTEGER,
    raw_bytes    INTEGER NOT NULL DEFAULT 0,
    stored_bytes INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS files (
    id        INTEGER PRIMARY KEY,
    job_id    INTEGER NOT NULL REFERENCES jobs(id),
    path      TEXT NOT NULL,
    size      INTEGER NOT NULL,
    mtime     INTEGER NOT NULL,
    file_hash BLOB NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
CREATE TABLE IF NOT EXISTS file_chunks (
    file_id    INTEGER NOT NULL REFERENCES files(id),
    seq        INTEGER NOT NULL,
    chunk_hash BLOB NOT NULL REFERENCES chunks(hash),
    PRIMARY KEY (file_id, seq)
);
)sql";

}  // namespace

void Repo::init(const fs::path& root) {
    if (fs::exists(root) && !fs::is_empty(root))
        throw std::runtime_error("refusing to init: directory exists and is not empty: " +
                                 root.string());
    fs::create_directories(root / "chunks");

    std::ofstream cfg(root / "config.json");
    cfg << "{\n"
        << "  \"format_version\": 1,\n"
        << "  \"chunking\": \"whole-file\",\n"
        << "  \"hash\": \"blake3-256\"\n"
        << "}\n";
    if (!cfg) throw std::runtime_error("cannot write config.json");

    Db db;
    db.open(root / "index.db");
    db.exec(kSchema);
}

Repo Repo::open(const fs::path& root) {
    if (!fs::exists(root / "config.json"))
        throw std::runtime_error("not a repository (config.json missing): " + root.string());
    Repo r;
    r.root_ = fs::absolute(root);
    r.db_.open(root / "index.db");
    r.db_.exec(kSchema);
    return r;
}

fs::path Repo::chunk_path(const std::string& hex) const {
    fs::path dir = root_ / "chunks" / hex.substr(0, 2);
    fs::create_directories(dir);
    return dir / hex;
}

fs::path Repo::temp_chunk_path() const {
    static std::mt19937_64 rng{std::random_device{}()};
    return root_ / "chunks" / (".tmp-" + std::to_string(rng()));
}
