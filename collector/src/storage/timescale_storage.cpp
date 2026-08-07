#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <libpq-fe.h>

#include "timescale_storage.h"

namespace pudimcollector {

namespace {

std::string EscapeLiteral(PGconn *conn, const std::string &s) {
    if (s.empty()) return "''";
    char *escaped = PQescapeLiteral(conn, s.c_str(), s.length());
    if (!escaped) return "''";
    std::string out(escaped);
    PQfreemem(escaped);
    return out;
}

// JSON string escape (not SQL literal escape). Produces a JSON-ready string
// WITHOUT surrounding quotes, so callers embed as: "\"..." + JsonEscape(...) + "\"".
std::string JsonEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// PostgreSQL boolean column returns "t"/"f"; map to JSON true/false.
const char *PgBoolToJson(const char *v) {
    return (v && v[0] == 't') ? "true" : "false";
}

// Map a protobuf CheckType enum to a string for the SQL check_type column.
const char *CheckTypeToString(pudimnetmon::CheckType type) {
    switch (type) {
        case pudimnetmon::CHECK_TYPE_DNS_RESOLUTION: return "dns_resolution";
        case pudimnetmon::CHECK_TYPE_TCP_CONNECT:    return "tcp_connect";
        case pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE:  return "tls_handshake";
        case pudimnetmon::CHECK_TYPE_HTTP_REQUEST:   return "http_request";
        case pudimnetmon::CHECK_TYPE_ICMP_PING:      return "icmp_ping";
        case pudimnetmon::CHECK_TYPE_JITTER:         return "jitter";
        default:                                     return "unspecified";
    }
}

} // anonymous namespace

struct TimescaleStorage::Impl {
    PGconn *conn = nullptr;
    StorageConfig config;
    std::atomic<uint64_t> metrics_written{0};
    std::atomic<uint64_t> batches_written{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> insert_latency_total_ms{0};
    std::mutex write_mutex;

    explicit Impl(StorageConfig cfg) : config(std::move(cfg)) {}

    ~Impl() {
        if (conn) PQfinish(conn);
    }

    bool ExecSimple(const std::string &sql) {
        PGresult *res = PQexec(conn, sql.c_str());
        bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK ||
                   PQresultStatus(res) == PGRES_TUPLES_OK);
        if (!ok) {
            std::cerr << "SQL error: " << PQerrorMessage(conn);
        }
        PQclear(res);
        return ok;
    }
};

TimescaleStorage::TimescaleStorage(StorageConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

TimescaleStorage::~TimescaleStorage() = default;

bool TimescaleStorage::Connect() {
    std::ostringstream conninfo;
    conninfo << "host=" << m_impl->config.host
             << " port=" << m_impl->config.port
             << " dbname=" << m_impl->config.dbname
             << " user=" << m_impl->config.user
             << " password=" << m_impl->config.password;

    m_impl->conn = PQconnectdb(conninfo.str().c_str());
    if (PQstatus(m_impl->conn) != CONNECTION_OK) {
        std::cerr << "PostgreSQL connection failed: "
                  << PQerrorMessage(m_impl->conn) << "\n";
        PQfinish(m_impl->conn);
        m_impl->conn = nullptr;
        return false;
    }

    // Enable TimescaleDB extension
    if (!m_impl->ExecSimple("CREATE EXTENSION IF NOT EXISTS timescaledb;")) {
        std::cerr << "Failed to enable TimescaleDB extension\n";
        return false;
    }

    // Schema: hypertable for network metrics
    std::string schema = R"SQL(
CREATE TABLE IF NOT EXISTS network_metrics (
    time           TIMESTAMPTZ NOT NULL,
    agent_id       TEXT        NOT NULL,
    check_type     TEXT        NOT NULL,
    target         TEXT        NOT NULL,
    success        BOOLEAN     NOT NULL,
    latency_ms     DOUBLE PRECISION,
    packet_loss_pct DOUBLE PRECISION,
    jitter_ms      DOUBLE PRECISION,
    rtt_ms         DOUBLE PRECISION,
    status_code    BIGINT,
    detail         TEXT,
    seq            BIGINT,
    monotonic_us   BIGINT,
    attributes     JSONB,
    PRIMARY KEY (time, agent_id, check_type, target, seq)
);
)SQL";

    if (!m_impl->ExecSimple(schema)) {
        return false;
    }

    // Convert to hypertable if not already
    std::string hypertable = R"SQL(
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM timescaledb_information.hypertables
                   WHERE hypertable_name = 'network_metrics') THEN
        PERFORM create_hypertable('network_metrics', 'time', chunk_time_interval => INTERVAL '1 hour');
    END IF;
END $$;
)SQL";

    if (!m_impl->ExecSimple(hypertable)) {
        return false;
    }

    // Index for dashboard per-agent, per-check queries
    if (!m_impl->ExecSimple(
            "CREATE INDEX IF NOT EXISTS idx_network_metrics_agent_check "
            "ON network_metrics (agent_id, check_type, time DESC);")) {
        return false;
    }

    // Compression policy on chunks older than 1 day
    if (!m_impl->ExecSimple(
            "ALTER TABLE network_metrics SET (timescaledb.compress, "
            "timescaledb.compress_segmentby = 'agent_id');")) {
        // Non-fatal if already set or not supported
    }

    // Retention: drop chunks older than 30 days (informational, Phase 7 will
    // make configurable).
    if (!m_impl->ExecSimple(
            "SELECT add_retention_policy('network_metrics', INTERVAL '30 days');")) {
        // Non-fatal if policy already exists
    }

    return true;
}

bool TimescaleStorage::InsertMetrics(
    const std::string &agent_id,
    int64_t batch_timestamp_unix_ms,
    const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics) {
    std::lock_guard lock(m_impl->write_mutex);

    if (!m_impl->conn) return false;
    if (PQstatus(m_impl->conn) != CONNECTION_OK) return false;

    auto start = std::chrono::steady_clock::now();

    int total_inserted = 0;
    int batch_rows = 0;
    std::string sql = "INSERT INTO network_metrics "
                      "(time, agent_id, check_type, target, success, "
                      " latency_ms, packet_loss_pct, jitter_ms, rtt_ms, "
                      " status_code, detail, seq, monotonic_us, attributes) VALUES ";

    bool first_value = true;

    auto flush = [&]() -> bool {
        if (batch_rows == 0) return true;
        sql += ";";
        PGresult *res = PQexec(m_impl->conn, sql.c_str());
        bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
        PQclear(res);
        if (!ok) {
            std::cerr << "Batch insert error: " << PQerrorMessage(m_impl->conn);
            m_impl->errors++;
            sql.clear();
            batch_rows = 0;
            first_value = true;
            return false;
        }
        total_inserted += batch_rows;
        m_impl->batches_written++;
        sql.clear();
        batch_rows = 0;
        first_value = true;
        return true;
    };

    for (const auto &m : metrics) {
        if (!first_value) {
            sql += ",";
        }
        first_value = false;

        // Use agent-reported timestamp (metric itself carries no dedicated
        // time; use batch timestamp + a 1ms increment per metric to preserve
        // ordering within a batch; Phase 5 will normalise timestamps).
        int64_t ts_ms = batch_timestamp_unix_ms + static_cast<int64_t>(total_inserted + batch_rows);

        auto tmp = m;
        double latency = tmp.has_latency_ms() ? tmp.latency_ms() : 0;
        double loss = tmp.has_packet_loss_pct() ? tmp.packet_loss_pct() : 0;
        double jitter = tmp.has_jitter_ms() ? tmp.jitter_ms() : 0;
        double rtt = tmp.has_rtt_ms() ? tmp.rtt_ms() : 0;
        int64_t status = tmp.has_status_code() ? tmp.status_code() : 0;

        sql += "(to_timestamp(" + std::to_string(ts_ms / 1000) +
               "::double precision + " +
               std::to_string((ts_ms % 1000) / 1000.0) + "), ";
        sql += EscapeLiteral(m_impl->conn, agent_id) + ", ";
        sql += "'" + std::string(CheckTypeToString(m.check_type())) + "', ";
        sql += EscapeLiteral(m_impl->conn, m.target()) + ", ";
        sql += (m.success() ? "true" : "false") + std::string(", ");
        sql += (tmp.has_latency_ms() ? std::to_string(latency) : "NULL") + ", ";
        sql += (tmp.has_packet_loss_pct() ? std::to_string(loss) : "NULL") + ", ";
        sql += (tmp.has_jitter_ms() ? std::to_string(jitter) : "NULL") + ", ";
        sql += (tmp.has_rtt_ms() ? std::to_string(rtt) : "NULL") + ", ";
        sql += (tmp.has_status_code() ? std::to_string(status) : "NULL") + ", ";
        sql += (m.detail().empty() ? "NULL" : EscapeLiteral(m_impl->conn, m.detail())) + ", ";
        sql += std::to_string(m.seq()) + ", ";
        sql += std::to_string(m.monotonic_us()) + ", ";
        sql += "'{}'::jsonb)";

        batch_rows++;

        if (batch_rows >= m_impl->config.batch_size) {
            if (!flush()) return false;
        }
    }

    if (!flush()) return false;

    m_impl->metrics_written += total_inserted;

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    m_impl->insert_latency_total_ms += static_cast<uint64_t>(ms);

    return true;
}

std::string TimescaleStorage::QueryMetricsJson(
    const std::string &agent_id,
    const std::string &check_type,
    int64_t window_seconds) const {
    if (!m_impl->conn) return "[]";
    if (PQstatus(m_impl->conn) != CONNECTION_OK) return "[]";

    std::lock_guard lock(m_impl->write_mutex);

    std::string sql = "SELECT "
                      "  EXTRACT(EPOCH FROM time) * 1000 AS time_ms, "
                      "  agent_id, check_type, target, success, "
                      "  COALESCE(latency_ms, packet_loss_pct, jitter_ms, rtt_ms, "
                      "           status_code::double precision, 0) AS value "
                      "FROM network_metrics "
                      "WHERE time > now() - make_interval(secs => " +
                      std::to_string(window_seconds) + ")";

    if (!agent_id.empty()) {
        sql += " AND agent_id = " + EscapeLiteral(m_impl->conn, agent_id);
    }
    if (!check_type.empty()) {
        sql += " AND check_type = " + EscapeLiteral(m_impl->conn, check_type);
    }
    sql += " ORDER BY time ASC LIMIT 10000;";

    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "QueryMetricsJson error: " << PQerrorMessage(m_impl->conn);
        PQclear(res);
        return "[]";
    }

    int rows = PQntuples(res);
    std::string json = "[";
    for (int i = 0; i < rows; i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"time_ms\":" + std::string(PQgetvalue(res, i, 0)) + ",";
        json += "\"agent_id\":\"" + JsonEscape(PQgetvalue(res, i, 1)) + "\",";
        json += "\"check_type\":\"" + JsonEscape(PQgetvalue(res, i, 2)) + "\",";
        json += "\"target\":\"" + JsonEscape(PQgetvalue(res, i, 3)) + "\",";
        json += "\"success\":" +
                std::string(PgBoolToJson(PQgetvalue(res, i, 4))) + ",";
        std::string value = PQgetvalue(res, i, 5);
        if (value.empty()) value = "0";
        json += "\"value\":" + value;
        json += "}";
    }
    json += "]";
    PQclear(res);
    return json;
}

bool TimescaleStorage::IsHealthy() const {
    if (!m_impl->conn) return false;
    if (PQstatus(m_impl->conn) != CONNECTION_OK) return false;

    PGresult *res = PQexec(m_impl->conn, "SELECT 1;");
    bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK);
    // PQclear was called implicitly via ExecSimple? No: use PQclear directly.
    PQclear(res);
    return ok;
}

StorageStats TimescaleStorage::GetStats() const {
    StorageStats stats;
    stats.metrics_written = m_impl->metrics_written.load();
    stats.batches_written = m_impl->batches_written.load();
    stats.errors = m_impl->errors.load();
    stats.insert_latency_total_ms = m_impl->insert_latency_total_ms.load();
    return stats;
}

} // namespace pudimcollector