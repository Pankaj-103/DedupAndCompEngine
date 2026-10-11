#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

// Minimal RAII wrappers around SQLite. All errors throw std::runtime_error.

class Db {
public:
    Db() = default;
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;

    void open(const std::filesystem::path& file);
    void exec(const std::string& sql);
    int64_t last_insert_id();
    sqlite3* raw() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

class Stmt {
public:
    Stmt(Db& db, const char* sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int idx, int64_t v);
    Stmt& bind(int idx, const std::string& v);
    Stmt& bind_blob(int idx, const void* data, size_t n);

    bool step();  // true if a result row is available
    void run();   // step until done, discarding rows

    int64_t col_int64(int i);
    std::string col_text(int i);
    std::vector<uint8_t> col_blob(int i);
    bool col_is_null(int i);

private:
    sqlite3_stmt* stmt_ = nullptr;
    sqlite3* db_ = nullptr;
};
