#include "commands.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <iostream>
#include <stdexcept>

#include "codec.h"
#include "db.h"
#include "hash.h"
#include "repo.h"

namespace fs = std::filesystem;

namespace {

int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

int64_t mtime_seconds(const fs::path& p) {
    using namespace std::chrono;
    const auto sys = clock_cast<system_clock>(fs::last_write_time(p));
    return duration_cast<seconds>(sys.time_since_epoch()).count();
}

std::string format_bytes(uint64_t n) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0)
        std::snprintf(buf, sizeof(buf), "%" PRIu64 " B", n);
    else
        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
    return buf;
}

bool is_inside(const fs::path& p, const fs::path& root) {
    const auto rp = fs::weakly_canonical(p);
    const auto rr = fs::weakly_canonical(root);
    auto it = std::mismatch(rr.begin(), rr.end(), rp.begin(), rp.end()).first;
    return it == rr.end();
}

struct IngestStats {
    uint64_t files = 0;
    uint64_t rawBytes = 0;
    uint64_t dedupedBytes = 0;   // bytes that matched existing chunks
    uint64_t storedBytes = 0;    // compressed bytes newly written
    uint64_t errors = 0;
};

// Phase 1: whole-file dedup — each file is a single chunk keyed by its BLAKE3.
void ingest_file(Repo& repo, int64_t jobId, const fs::path& p, int level, IngestStats& st) {
    const uint64_t size = fs::file_size(p);
    const int64_t mtime = mtime_seconds(p);
    const Hash256 h = hash_file(p);
    const std::string hex = to_hex(h);

    repo.db().exec("BEGIN");
    try {
        bool newChunk = false;
        uint64_t stored = 0;
        if (size > 0) {
            Stmt find(repo.db(), "SELECT stored_size FROM chunks WHERE hash=?1");
            find.bind_blob(1, h.data(), h.size());
            if (!find.step()) {
                newChunk = true;
                const fs::path tmp = repo.temp_chunk_path();
                const uint64_t comp = zstd_compress_file(p, tmp, level);
                Codec codec = Codec::Zstd;
                stored = comp;
                if (comp >= size) {  // incompressible: store raw
                    fs::remove(tmp);
                    fs::copy_file(p, tmp, fs::copy_options::overwrite_existing);
                    codec = Codec::None;
                    stored = size;
                }
                fs::rename(tmp, repo.chunk_path(hex));
                Stmt ins(repo.db(),
                         "INSERT INTO chunks(hash, raw_size, stored_size, codec, refcount) "
                         "VALUES(?1,?2,?3,?4,0)");
                ins.bind_blob(1, h.data(), h.size())
                    .bind(2, static_cast<int64_t>(size))
                    .bind(3, static_cast<int64_t>(stored))
                    .bind(4, static_cast<int64_t>(codec));
                ins.run();
            }
        }

        Stmt insFile(repo.db(),
                     "INSERT INTO files(job_id, path, size, mtime, file_hash) "
                     "VALUES(?1,?2,?3,?4,?5)");
        insFile.bind(1, jobId)
            .bind(2, fs::absolute(p).lexically_normal().string())
            .bind(3, static_cast<int64_t>(size))
            .bind(4, mtime)
            .bind_blob(5, h.data(), h.size());
        insFile.run();
        const int64_t fileId = repo.db().last_insert_id();

        if (size > 0) {
            Stmt fc(repo.db(), "INSERT INTO file_chunks(file_id, seq, chunk_hash) VALUES(?1,0,?2)");
            fc.bind(1, fileId).bind_blob(2, h.data(), h.size());
            fc.run();
            Stmt rc(repo.db(), "UPDATE chunks SET refcount = refcount + 1 WHERE hash=?1");
            rc.bind_blob(1, h.data(), h.size());
            rc.run();
        }

        repo.db().exec("COMMIT");
        st.files++;
        st.rawBytes += size;
        if (newChunk)
            st.storedBytes += stored;
        else
            st.dedupedBytes += size;
    } catch (...) {
        repo.db().exec("ROLLBACK");
        throw;
    }
}

std::vector<fs::path> collect_files(const std::vector<fs::path>& inputs, const fs::path& repoRoot) {
    std::vector<fs::path> out;
    for (const auto& in : inputs) {
        if (!fs::exists(in)) {
            std::cerr << "warning: skipping missing path: " << in.string() << "\n";
            continue;
        }
        if (fs::is_regular_file(in)) {
            out.push_back(in);
            continue;
        }
        if (fs::is_directory(in)) {
            const auto opts = fs::directory_options::skip_permission_denied;
            for (auto it = fs::recursive_directory_iterator(in, opts);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_directory() && is_inside(it->path(), repoRoot)) {
                    it.disable_recursion_pending();  // never ingest the repository itself
                    continue;
                }
                if (it->is_symlink()) continue;
                if (it->is_regular_file()) out.push_back(it->path());
            }
            continue;
        }
        std::cerr << "warning: skipping non-regular path: " << in.string() << "\n";
    }
    return out;
}

}  // namespace

int cmd_init(const fs::path& repoPath) {
    Repo::init(repoPath);
    std::cout << "Initialized empty repository at " << fs::absolute(repoPath).string() << "\n";
    return 0;
}

int cmd_ingest(const fs::path& repoPath, const std::vector<fs::path>& inputs, int level) {
    Repo repo = Repo::open(repoPath);

    const auto files = collect_files(inputs, repo.root());
    if (files.empty()) {
        std::cout << "Nothing to ingest.\n";
        return 0;
    }

    Stmt job(repo.db(), "INSERT INTO jobs(started_at) VALUES(?1)");
    job.bind(1, now_seconds());
    job.run();
    const int64_t jobId = repo.db().last_insert_id();

    IngestStats st;
    for (const auto& f : files) {
        try {
            ingest_file(repo, jobId, f, level, st);
        } catch (const std::exception& e) {
            std::cerr << "error: " << f.string() << ": " << e.what() << "\n";
            st.errors++;
        }
    }

    Stmt fin(repo.db(),
             "UPDATE jobs SET finished_at=?1, raw_bytes=?2, stored_bytes=?3 WHERE id=?4");
    fin.bind(1, now_seconds())
        .bind(2, static_cast<int64_t>(st.rawBytes))
        .bind(3, static_cast<int64_t>(st.storedBytes))
        .bind(4, jobId);
    fin.run();

    std::cout << "\nJob " << jobId << " complete\n"
              << "  files ingested : " << st.files << "\n"
              << "  raw data       : " << format_bytes(st.rawBytes) << "\n"
              << "  deduplicated   : " << format_bytes(st.dedupedBytes) << "\n"
              << "  newly stored   : " << format_bytes(st.storedBytes) << " (after compression)\n";
    if (st.storedBytes > 0)
        std::cout << "  overall ratio  : "
                  << static_cast<double>(st.rawBytes) / static_cast<double>(st.storedBytes)
                  << "x\n";
    if (st.errors > 0) {
        std::cout << "  errors         : " << st.errors << "\n";
        return 1;
    }
    return 0;
}

int cmd_restore(const fs::path& repoPath, const std::string& spec, const fs::path& outDir) {
    Repo repo = Repo::open(repoPath);

    int64_t fileId = -1;
    std::string origPath;
    uint64_t size = 0;
    std::vector<uint8_t> fileHash;

    const bool numeric = !spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos;
    {
        const char* sql = numeric
            ? "SELECT id, path, size, file_hash FROM files WHERE id=?1"
            : "SELECT id, path, size, file_hash FROM files WHERE path=?1 OR path LIKE ?2 "
              "ORDER BY id DESC LIMIT 1";
        Stmt q(repo.db(), sql);
        if (numeric)
            q.bind(1, static_cast<int64_t>(std::stoll(spec)));
        else
            q.bind(1, spec).bind(2, "%\\" + spec);
        if (!q.step()) throw std::runtime_error("no ingested file matches: " + spec);
        fileId = q.col_int64(0);
        origPath = q.col_text(1);
        size = static_cast<uint64_t>(q.col_int64(2));
        fileHash = q.col_blob(3);
    }

    fs::create_directories(outDir);
    const fs::path outFile = outDir / fs::path(origPath).filename();
    std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create: " + outFile.string());

    Stmt q(repo.db(),
           "SELECT c.hash, c.codec FROM file_chunks fc "
           "JOIN chunks c ON c.hash = fc.chunk_hash "
           "WHERE fc.file_id=?1 ORDER BY fc.seq");
    q.bind(1, fileId);
    while (q.step()) {
        const auto hashBlob = q.col_blob(0);
        Hash256 h{};
        std::copy(hashBlob.begin(), hashBlob.end(), h.begin());
        const fs::path chunk = repo.chunk_path(to_hex(h));
        if (static_cast<Codec>(q.col_int64(1)) == Codec::Zstd)
            zstd_decompress_file(chunk, out);
        else
            append_raw_file(chunk, out);
    }
    out.close();

    const Hash256 actual = hash_file(outFile);
    if (!std::equal(actual.begin(), actual.end(), fileHash.begin(), fileHash.end())) {
        throw std::runtime_error("VERIFICATION FAILED: restored file hash mismatch for " +
                                 outFile.string());
    }
    std::cout << "Restored " << origPath << "\n  -> " << fs::absolute(outFile).string() << " ("
              << format_bytes(size) << ", hash verified)\n";
    return 0;
}

int cmd_list(const fs::path& repoPath) {
    Repo repo = Repo::open(repoPath);
    Stmt q(repo.db(), "SELECT id, job_id, size, path FROM files ORDER BY id");
    std::printf("%6s %5s %12s  %s\n", "id", "job", "size", "path");
    while (q.step()) {
        std::printf("%6lld %5lld %12s  %s\n", static_cast<long long>(q.col_int64(0)),
                    static_cast<long long>(q.col_int64(1)),
                    format_bytes(static_cast<uint64_t>(q.col_int64(2))).c_str(),
                    q.col_text(3).c_str());
    }
    return 0;
}

int cmd_stats(const fs::path& repoPath) {
    Repo repo = Repo::open(repoPath);

    uint64_t fileCount = 0, logicalBytes = 0;
    {
        Stmt q(repo.db(), "SELECT COUNT(*), COALESCE(SUM(size),0) FROM files");
        q.step();
        fileCount = static_cast<uint64_t>(q.col_int64(0));
        logicalBytes = static_cast<uint64_t>(q.col_int64(1));
    }
    uint64_t chunkCount = 0, uniqueBytes = 0, storedBytes = 0;
    {
        Stmt q(repo.db(),
               "SELECT COUNT(*), COALESCE(SUM(raw_size),0), COALESCE(SUM(stored_size),0) "
               "FROM chunks");
        q.step();
        chunkCount = static_cast<uint64_t>(q.col_int64(0));
        uniqueBytes = static_cast<uint64_t>(q.col_int64(1));
        storedBytes = static_cast<uint64_t>(q.col_int64(2));
    }

    std::cout << "Repository: " << repo.root().string() << "\n"
              << "  files tracked     : " << fileCount << "\n"
              << "  logical data      : " << format_bytes(logicalBytes) << "\n"
              << "  unique data       : " << format_bytes(uniqueBytes) << " in " << chunkCount
              << " chunks\n"
              << "  stored on disk    : " << format_bytes(storedBytes) << "\n"
              << "  dedup savings     : " << format_bytes(logicalBytes - uniqueBytes) << "\n"
              << "  compression saving: " << format_bytes(uniqueBytes - storedBytes) << "\n";
    if (storedBytes > 0)
        std::cout << "  overall ratio     : "
                  << static_cast<double>(logicalBytes) / static_cast<double>(storedBytes) << "x\n";
    return 0;
}
