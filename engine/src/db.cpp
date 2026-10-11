#include "db.h"

#include <stdexcept>
#include <utility>

#include "sqlite3.h"

namespace {

[[noreturn]] void throw_sqlite(sqlite3* db, const std::string& context) {
    throw std::runtime_error(context + ": " + (db ? sqlite3_errmsg(db) : "unknown error"));
}

}  // namespace

Db::~Db() {
    if (db_) sqlite3_close(db_);
}

Db::Db(Db&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Db& Db::operator=(Db&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

void Db::open(const std::filesystem::path& file) {
    if (db_) throw std::runtime_error("database already open");
    if (sqlite3_open(file.string().c_str(), &db_) != SQLITE_OK)
        throw_sqlite(db_, "cannot open database " + file.string());
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");
}

void Db::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sql error: " + msg + " in: " + sql);
    }
}

int64_t Db::last_insert_id() {
    return sqlite3_last_insert_rowid(db_);
}

Stmt::Stmt(Db& db, const char* sql) : db_(db.raw()) {
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK)
        throw_sqlite(db_, std::string("cannot prepare: ") + sql);
}

Stmt::~Stmt() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Stmt& Stmt::bind(int idx, int64_t v) {
    if (sqlite3_bind_int64(stmt_, idx, v) != SQLITE_OK) throw_sqlite(db_, "bind int64");
    return *this;
}

Stmt& Stmt::bind(int idx, const std::string& v) {
    if (sqlite3_bind_text(stmt_, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT) !=
        SQLITE_OK)
        throw_sqlite(db_, "bind text");
    return *this;
}

Stmt& Stmt::bind_blob(int idx, const void* data, size_t n) {
    if (sqlite3_bind_blob(stmt_, idx, data, static_cast<int>(n), SQLITE_TRANSIENT) != SQLITE_OK)
        throw_sqlite(db_, "bind blob");
    return *this;
}

bool Stmt::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw_sqlite(db_, "step");
}

void Stmt::run() {
    while (step()) {
    }
}

int64_t Stmt::col_int64(int i) {
    return sqlite3_column_int64(stmt_, i);
}

std::string Stmt::col_text(int i) {
    const unsigned char* t = sqlite3_column_text(stmt_, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

std::vector<uint8_t> Stmt::col_blob(int i) {
    const void* p = sqlite3_column_blob(stmt_, i);
    const int n = sqlite3_column_bytes(stmt_, i);
    const auto* b = static_cast<const uint8_t*>(p);
    return std::vector<uint8_t>(b, b + n);
}

bool Stmt::col_is_null(int i) {
    return sqlite3_column_type(stmt_, i) == SQLITE_NULL;
}
