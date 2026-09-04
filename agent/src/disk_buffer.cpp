#include "disk_buffer.h"
#include "logger.h"

#ifdef HAVE_SQLITE3
#include <sqlite3.h>

namespace pudimagent {

struct DiskBuffer::Impl {
    sqlite3 *db = nullptr;
    sqlite3_stmt *insert_stmt = nullptr;
    sqlite3_stmt *peek_stmt = nullptr;
    sqlite3_stmt *delete_stmt = nullptr;
    sqlite3_stmt *count_stmt = nullptr;
    uint64_t rows = 0;
    uint64_t bytes = 0;

    ~Impl() {
        if (insert_stmt) sqlite3_finalize(insert_stmt);
        if (peek_stmt) sqlite3_finalize(peek_stmt);
        if (delete_stmt) sqlite3_finalize(delete_stmt);
        if (count_stmt) sqlite3_finalize(count_stmt);
        if (db) sqlite3_close(db);
    }
};

namespace {

bool Exec(sqlite3 *db, const std::string &sql) {
    char *err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR(std::string("DiskBuffer SQL error: ") +
                  (err ? err : "unknown"));
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool Prepare(sqlite3 *db, sqlite3_stmt **stmt, const char *sql) {
    if (*stmt) return true;
    return sqlite3_prepare_v2(db, sql, -1, stmt, nullptr) == SQLITE_OK;
}

} // anonymous namespace

DiskBuffer::DiskBuffer(std::string db_path, uint64_t max_bytes)
    : m_impl(std::make_unique<Impl>()),
      m_db_path(std::move(db_path)),
      m_max_bytes(max_bytes) {}

DiskBuffer::~DiskBuffer() = default;

bool DiskBuffer::Open(std::string &error) {
    if (sqlite3_open(m_db_path.c_str(), &m_impl->db) != SQLITE_OK) {
        error = std::string("sqlite3_open failed: ") +
                (m_impl->db ? sqlite3_errmsg(m_impl->db) : "no db");
        m_impl->db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(m_impl->db, 5000);
    // Fast commits, crash-safe against process death
    // Can lose tail on crash/powerloss but its fine
    if (!Exec(m_impl->db, "PRAGMA journal_mode=WAL;") ||
        !Exec(m_impl->db, "PRAGMA synchronous=NORMAL;") ||
        !Exec(m_impl->db, "CREATE TABLE IF NOT EXISTS pending("
                          "  seq INTEGER PRIMARY KEY AUTOINCREMENT,"
                          "  payload BLOB NOT NULL);")) {
        error = "failed to initialise schema";
        return false;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_impl->db,
                           "SELECT count(*), COALESCE(sum(length(payload)),0) "
                           "FROM pending",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            m_impl->rows =
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            m_impl->bytes =
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

bool DiskBuffer::Available() const {
    return m_impl && m_impl->db != nullptr;
}

bool DiskBuffer::Push(const std::string &payload_blob) {
    if (!Available()) return false;

    Trim();
    if (m_impl->bytes + payload_blob.size() > m_max_bytes) {
        LOG_ERROR("DiskBuffer full; refusing to buffer more");
        return false;
    }

    if (!Prepare(m_impl->db, &m_impl->insert_stmt,
                 "INSERT INTO pending(payload) VALUES(?)")) {
        return false;
    }
    sqlite3_reset(m_impl->insert_stmt);
    sqlite3_clear_bindings(m_impl->insert_stmt);
    sqlite3_bind_blob(m_impl->insert_stmt, 1, payload_blob.data(),
                      static_cast<int>(payload_blob.size()), SQLITE_TRANSIENT);
    bool ok = sqlite3_step(m_impl->insert_stmt) == SQLITE_DONE;
    if (ok) {
        m_impl->rows++;
        m_impl->bytes += payload_blob.size();
    }
    return ok;
}

void DiskBuffer::Peek(std::vector<std::string> &out, size_t limit) {
    if (!Available() || limit == 0) return;
    if (!Prepare(m_impl->db, &m_impl->peek_stmt,
                 "SELECT payload FROM pending ORDER BY seq ASC LIMIT ?")) {
        return;
    }
    sqlite3_reset(m_impl->peek_stmt);
    sqlite3_clear_bindings(m_impl->peek_stmt);
    sqlite3_bind_int(m_impl->peek_stmt, 1, static_cast<int>(limit));
    while (sqlite3_step(m_impl->peek_stmt) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(m_impl->peek_stmt, 0);
        int n = sqlite3_column_bytes(m_impl->peek_stmt, 0);
        out.emplace_back(static_cast<const char *>(blob),
                         static_cast<size_t>(n));
    }
}

void DiskBuffer::Pop(size_t count) {
    if (!Available() || count == 0) return;

    if (!Prepare(m_impl->db, &m_impl->peek_stmt,
                 "SELECT payload FROM pending ORDER BY seq ASC LIMIT ?")) {
        return;
    }
    sqlite3_reset(m_impl->peek_stmt);
    sqlite3_clear_bindings(m_impl->peek_stmt);
    sqlite3_bind_int(m_impl->peek_stmt, 1, static_cast<int>(count));
    uint64_t removed = 0;
    while (sqlite3_step(m_impl->peek_stmt) == SQLITE_ROW) {
        removed +=
            static_cast<uint64_t>(sqlite3_column_bytes(m_impl->peek_stmt, 0));
    }

    if (!Prepare(m_impl->db, &m_impl->delete_stmt,
                 "DELETE FROM pending WHERE seq IN "
                 "(SELECT seq FROM pending ORDER BY seq ASC LIMIT ?)")) {
        return;
    }
    sqlite3_reset(m_impl->delete_stmt);
    sqlite3_clear_bindings(m_impl->delete_stmt);
    sqlite3_bind_int(m_impl->delete_stmt, 1, static_cast<int>(count));
    if (sqlite3_step(m_impl->delete_stmt) == SQLITE_DONE) {
        m_impl->rows = m_impl->rows > count ? m_impl->rows - count : 0;
        m_impl->bytes = m_impl->bytes > removed ? m_impl->bytes - removed : 0;
    }
}

uint64_t DiskBuffer::Size() const {
    if (!Available()) return 0;
    if (!Prepare(m_impl->db, &m_impl->count_stmt,
                 "SELECT count(*) FROM pending")) {
        return 0;
    }
    sqlite3_reset(m_impl->count_stmt);
    sqlite3_clear_bindings(m_impl->count_stmt);
    uint64_t n = 0;
    if (sqlite3_step(m_impl->count_stmt) == SQLITE_ROW) {
        n = static_cast<uint64_t>(sqlite3_column_int64(m_impl->count_stmt, 0));
    }
    return n;
}

uint64_t DiskBuffer::Trim() {
    if (!Available() || m_impl->bytes <= m_max_bytes) return 0;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_impl->db,
                           "SELECT length(payload) FROM pending ORDER BY seq ASC",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    uint64_t to_remove = 0;
    uint64_t kept = m_impl->bytes;
    while (sqlite3_step(stmt) == SQLITE_ROW && kept > m_max_bytes) {
        int64_t sz = sqlite3_column_int64(stmt, 0);
        kept -= sz > 0 ? static_cast<uint64_t>(sz) : 0;
        to_remove++;
    }
    sqlite3_finalize(stmt);

    if (to_remove == 0) return 0;
    if (to_remove > m_impl->rows) to_remove = m_impl->rows;

    // Single batched delete instead of one statement per row.
    if (!Prepare(m_impl->db, &m_impl->delete_stmt,
                 "DELETE FROM pending WHERE seq IN "
                 "(SELECT seq FROM pending ORDER BY seq ASC LIMIT ?)")) {
        return 0;
    }
    sqlite3_reset(m_impl->delete_stmt);
    sqlite3_clear_bindings(m_impl->delete_stmt);
    sqlite3_bind_int(m_impl->delete_stmt, 1, static_cast<int>(to_remove));
    if (sqlite3_step(m_impl->delete_stmt) != SQLITE_DONE) return 0;

    m_impl->rows = m_impl->rows > to_remove ? m_impl->rows - to_remove : 0;
    m_impl->bytes = kept;
    return to_remove;
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

