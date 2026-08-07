#include <iostream>

#include "disk_buffer.h"

#ifdef HAVE_SQLITE3
#include <sqlite3.h>

namespace pudimagent {

struct DiskBuffer::Impl {
    sqlite3 *db = nullptr;
    uint64_t rows = 0;
    uint64_t bytes = 0;
};

namespace {

bool Exec(sqlite3 *db, const std::string &sql) {
    char *err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "DiskBuffer SQL error: " << (err ? err : "unknown")
                  << "\n";
        sqlite3_free(err);
        return false;
    }
    return true;
}

} // anonymous namespace

DiskBuffer::DiskBuffer(std::string db_path, uint64_t max_bytes)
    : m_impl(std::make_unique<Impl>()),
      m_db_path(std::move(db_path)),
      m_max_bytes(max_bytes) {}

DiskBuffer::~DiskBuffer() {
    if (m_impl->db) sqlite3_close(m_impl->db);
}

bool DiskBuffer::Open(std::string &error) {
    if (sqlite3_open(m_db_path.c_str(), &m_impl->db) != SQLITE_OK) {
        error = std::string("sqlite3_open failed: ") +
                (m_impl->db ? sqlite3_errmsg(m_impl->db) : "no db");
        m_impl->db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(m_impl->db, 5000);
    if (!Exec(m_impl->db, "PRAGMA journal_mode=WAL;") ||
        !Exec(m_impl->db, "CREATE TABLE IF NOT EXISTS pending("
                          "  seq INTEGER PRIMARY KEY AUTOINCREMENT,"
                          "  payload BLOB NOT NULL);")) {
        error = "failed to initialise schema";
        return false;
    }
    return true;
}

bool DiskBuffer::Available() const {
    return m_impl && m_impl->db != nullptr;
}

bool DiskBuffer::Push(const std::string &payload_blob) {
    if (!Available()) return false;

    // Enforce the size cap before inserting.
    Trim();
    if (m_impl->bytes + payload_blob.size() > m_max_bytes) {
        std::cerr << "DiskBuffer full; refusing to buffer more\n";
        return false;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_impl->db,
                           "INSERT INTO pending(payload) VALUES(?)",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_blob(stmt, 1, payload_blob.data(),
                      static_cast<int>(payload_blob.size()), SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok) {
        m_impl->rows++;
        m_impl->bytes += payload_blob.size();
    }
    return ok;
}

void DiskBuffer::Peek(std::vector<std::string> &out, size_t limit) {
    if (!Available() || limit == 0) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_impl->db,
                           "SELECT payload FROM pending ORDER BY seq ASC LIMIT ?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int(stmt, 1, static_cast<int>(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(stmt, 0);
        int n = sqlite3_column_bytes(stmt, 0);
        out.emplace_back(static_cast<const char *>(blob),
                         static_cast<size_t>(n));
    }
    sqlite3_finalize(stmt);
}

void DiskBuffer::Pop(size_t count) {
    if (!Available() || count == 0) return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_impl->db,
                           "SELECT payload FROM pending ORDER BY seq ASC LIMIT ?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int(stmt, 1, static_cast<int>(count));
    uint64_t removed = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        removed += static_cast<uint64_t>(sqlite3_column_bytes(stmt, 0));
    }
    sqlite3_finalize(stmt);

    if (Exec(m_impl->db,
             "DELETE FROM pending WHERE seq IN "
             "(SELECT seq FROM pending ORDER BY seq ASC LIMIT " +
                 std::to_string(count) + ");")) {
        m_impl->rows = m_impl->rows > count ? m_impl->rows - count : 0;
        m_impl->bytes = m_impl->bytes > removed ? m_impl->bytes - removed : 0;
    }
}

uint64_t DiskBuffer::Size() const {
    if (!Available()) return 0;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_impl->db, "SELECT count(*) FROM pending",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    uint64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        n = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return n;
}

uint64_t DiskBuffer::Trim() {
    if (!Available()) return 0;
    uint64_t dropped = 0;
    while (m_impl->bytes > m_max_bytes) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_impl->db,
                               "DELETE FROM pending WHERE seq = "
                               "(SELECT seq FROM pending ORDER BY seq ASC LIMIT 1)",
                               -1, &stmt, nullptr) != SQLITE_OK) {
            break;
        }
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            break;
        }
        sqlite3_finalize(stmt);
        dropped++;
        m_impl->bytes = m_impl->bytes > 0 ? m_impl->bytes - 512 : 0;
        m_impl->rows = m_impl->rows > 0 ? m_impl->rows - 1 : 0;
    }
    return dropped;
}

} // namespace pudimagent

#else  // !HAVE_SQLITE3

namespace pudimagent {

struct DiskBuffer::Impl {};

DiskBuffer::DiskBuffer(std::string, uint64_t)
    : m_impl(std::make_unique<Impl>()) {}
DiskBuffer::~DiskBuffer() = default;
bool DiskBuffer::Open(std::string &) { return false; }
bool DiskBuffer::Available() const { return false; }
bool DiskBuffer::Push(const std::string &) { return false; }
void DiskBuffer::Peek(std::vector<std::string> &, size_t) {}
void DiskBuffer::Pop(size_t) {}
uint64_t DiskBuffer::Size() const { return 0; }
uint64_t DiskBuffer::Trim() { return 0; }

} // namespace pudimagent

#endif

