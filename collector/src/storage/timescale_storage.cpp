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

// JSON string escape without surrounding quotes.
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

// Serializes a metric's attributes map as a JSONB SQL literal.
std::string AttributesJsonLiteral(PGconn *conn, const pudimnetmon::Metric &m) {
    std::string body = "{";
    bool first = true;
    for (const auto &[k, v] : m.attributes()) {
        if (!first) body += ",";
        first = false;
        body += "\"" + JsonEscape(k) + "\":\"" + JsonEscape(v) + "\"";
    }
    body += "}";
    char *escaped = PQescapeLiteral(conn, body.c_str(), body.length());
    if (!escaped) return "'{}'::jsonb";
    std::string out(escaped);
    PQfreemem(escaped);
    return out + "::jsonb";
}

// Maps a PostgreSQL boolean column ("t"/"f") to JSON true or false.
const char *PgBoolToJson(const char *v) {
    return (v && v[0] == 't') ? "true" : "false";
}

bool PgBool(const char *v) {
    return v && v[0] == 't';
}

// Reads one command_schedules row into a CommandSchedule struct.
CommandSchedule ReadScheduleRow(PGresult *res, int row) {
    CommandSchedule s;
    s.id = PQgetvalue(res, row, 0);
    s.label = PQgetvalue(res, row, 1);
    s.agent_id = PQgetvalue(res, row, 2);
    s.command_id = PQgetvalue(res, row, 3);
    std::string params = PQgetvalue(res, row, 4);
    s.params_json = params.empty() ? "{}" : params;
    s.window_start_unix_ms = std::stoll(PQgetvalue(res, row, 5));
    s.window_end_unix_ms = std::stoll(PQgetvalue(res, row, 6));
    s.interval_sec = std::stoll(PQgetvalue(res, row, 7));
    s.next_run_unix_ms = std::stoll(PQgetvalue(res, row, 8));
    s.enabled = PgBool(PQgetvalue(res, row, 9));
    return s;
}

// Reads one command_runs row into a CommandRun struct.
CommandRun ReadRunRow(PGresult *res, int row) {
    CommandRun r;
    r.run_id = std::stoll(PQgetvalue(res, row, 0));
    r.schedule_id = PQgetvalue(res, row, 1);
    r.agent_id = PQgetvalue(res, row, 2);
    r.command_id = PQgetvalue(res, row, 3);
    r.scheduled_unix_ms = std::stoll(PQgetvalue(res, row, 4));
    r.started_unix_ms = std::stoll(PQgetvalue(res, row, 5));
    if (!PQgetisnull(res, row, 6)) r.finished_unix_ms = std::stoll(PQgetvalue(res, row, 6));
    if (!PQgetisnull(res, row, 7)) {
        r.success = PgBool(PQgetvalue(res, row, 7));
        r.has_result = true;
    }
    r.error = PQgetvalue(res, row, 8);
    r.summary = PQgetvalue(res, row, 9);
    std::string fields = PQgetvalue(res, row, 10);
    r.fields_json = fields.empty() ? "{}" : fields;
    std::string issues = PQgetvalue(res, row, 11);
    r.issues_json = issues.empty() ? "[]" : issues;
    r.detail = PQgetvalue(res, row, 12);
    return r;
}

// Wraps a raw JSON literal for a JSONB column.
std::string JsonbLiteral(PGconn *conn, const std::string &json) {
    const std::string &body = json.empty() ? "{}" : json;
    char *escaped = PQescapeLiteral(conn, body.c_str(), body.length());
    if (!escaped) return "'{}'::jsonb";
    std::string out(escaped);
    PQfreemem(escaped);
    return out + "::jsonb";
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
        case pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE: return "tls_certificate";
        case pudimnetmon::CHECK_TYPE_TCP_RETRANSMIT:  return "tcp_retransmit";
        case pudimnetmon::CHECK_TYPE_DNS_RECORD:      return "dns_record";
        case pudimnetmon::CHECK_TYPE_TCP_HANDSHAKE:   return "tcp_handshake";
        case pudimnetmon::CHECK_TYPE_NTP_OFFSET:      return "ntp_offset";
        default:                                     return "unspecified";
    }
}

} // anonymous namespace

namespace {

// Builds a libpq conninfo string with a connect timeout, a statement timeout
// and TCP keepalives so a dropped database cannot block a request indefinitely.
std::string BuildConnInfo(const StorageConfig &cfg) {
    std::ostringstream conninfo;
    conninfo << "host=" << cfg.host
             << " port=" << cfg.port
             << " dbname=" << cfg.dbname
             << " user=" << cfg.user
             << " password=" << cfg.password
             // Fail fast when the database is unreachable.
             << " connect_timeout=5"
             // Cap every statement at 5s.
             << " options='-c statement_timeout=5000'"
             // TCP keepalives detect a dead connection within ~25s.
             << " keepalives=1 keepalives_idle=10 keepalives_interval=5 keepalives_count=3";
    return conninfo.str();
}

} // anonymous namespace

struct TimescaleStorage::Impl {
    PGconn *conn = nullptr;
    StorageConfig config;
    std::atomic<uint64_t> metrics_written{0};
    std::atomic<uint64_t> batches_written{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> insert_latency_total_ms{0};
    // Recursive so EnsureConnected() can run under methods that already hold
    // it. Serialises every PQexec on the shared connection.
    std::recursive_mutex write_mutex;

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
    std::lock_guard lock(m_impl->write_mutex);
    m_impl->conn = PQconnectdb(BuildConnInfo(m_impl->config).c_str());
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

    // Network metrics hypertable schema.
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

    // Retention policy drops chunks older than 30 days.
    if (!m_impl->ExecSimple(
            "SELECT add_retention_policy('network_metrics', INTERVAL '30 days');")) {
        // Non-fatal if policy already exists
    }

    // Command schedules
    const std::string schedule_schema = R"SQL(
CREATE TABLE IF NOT EXISTS command_schedules (
    id                    TEXT PRIMARY KEY,
    label                 TEXT NOT NULL DEFAULT '',
    agent_id              TEXT NOT NULL,
    command_id            TEXT NOT NULL,
    params                JSONB NOT NULL DEFAULT '{}'::jsonb,
    window_start_unix_ms  BIGINT NOT NULL,
    window_end_unix_ms    BIGINT NOT NULL,
    interval_sec          BIGINT NOT NULL,
    next_run_unix_ms      BIGINT NOT NULL,
    enabled               BOOLEAN NOT NULL DEFAULT TRUE,
    created_at            TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_command_schedules_due
    ON command_schedules (enabled, window_end_unix_ms, next_run_unix_ms);
)SQL";
    if (!m_impl->ExecSimple(schedule_schema)) {
        return false;
    }

    // Command run history
    const std::string runs_schema = R"SQL(
CREATE TABLE IF NOT EXISTS command_runs (
    run_id             BIGSERIAL PRIMARY KEY,
    schedule_id        TEXT NOT NULL DEFAULT '',
    agent_id           TEXT NOT NULL,
    command_id         TEXT NOT NULL,
    scheduled_unix_ms  BIGINT NOT NULL,
    started_unix_ms    BIGINT NOT NULL,
    finished_unix_ms   BIGINT,
    success            BOOLEAN,
    error              TEXT NOT NULL DEFAULT '',
    summary            TEXT NOT NULL DEFAULT '',
    fields             JSONB NOT NULL DEFAULT '{}'::jsonb,
    issues             JSONB NOT NULL DEFAULT '[]'::jsonb,
    detail             TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_command_runs_agent_time
    ON command_runs (agent_id, started_unix_ms DESC);
CREATE INDEX IF NOT EXISTS idx_command_runs_schedule_time
    ON command_runs (schedule_id, started_unix_ms DESC);
)SQL";
    if (!m_impl->ExecSimple(runs_schema)) {
        return false;
    }

    return true;
}

// Reconnects when the connection is missing or stale. Called before every SQL
// method so a database restart self-heals on the next request.
void TimescaleStorage::EnsureConnected() const {
    std::lock_guard lock(m_impl->write_mutex);
    if (m_impl->conn && PQstatus(m_impl->conn) == CONNECTION_OK) return;

    if (m_impl->conn) {
        std::cerr << "PostgreSQL connection lost; reconnecting...\n";
        PQfinish(m_impl->conn);
        m_impl->conn = nullptr;
    }

    m_impl->conn = PQconnectdb(BuildConnInfo(m_impl->config).c_str());
    if (PQstatus(m_impl->conn) != CONNECTION_OK) {
        std::cerr << "PostgreSQL reconnect failed: "
                  << PQerrorMessage(m_impl->conn) << "\n";
        PQfinish(m_impl->conn);
        m_impl->conn = nullptr;
    }
}

bool TimescaleStorage::InsertMetrics(
    const std::string &agent_id,
    int64_t batch_timestamp_unix_ms,
    const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics) {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();

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
        // Redelivered metrics are skipped via ON CONFLICT DO NOTHING.
        sql += " ON CONFLICT DO NOTHING;";
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

        // No per-metric time field exists, so each metric gets a 1ms offset
        // from the batch timestamp to preserve ordering.
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
        sql += AttributesJsonLiteral(m_impl->conn, m) + ")";

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
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();

    if (!m_impl->conn) return "[]";
    if (PQstatus(m_impl->conn) != CONNECTION_OK) return "[]";

    std::string sql = "SELECT "
                      "  EXTRACT(EPOCH FROM time) * 1000 AS time_ms, "
                      "  agent_id, check_type, target, success, "
                      "  COALESCE(latency_ms, packet_loss_pct, jitter_ms, rtt_ms, "
                      "           status_code::double precision, 0) AS value, "
                      "  attributes "
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
        json += "\"value\":" + value + ",";
        // The JSONB column arrives from PostgreSQL as JSON text.
        std::string attrs = PQgetvalue(res, i, 6);
        if (attrs.empty() || attrs == "{}") {
            json += "\"attributes\":{}";
        } else {
            json += "\"attributes\":" + attrs;
        }
        json += "}";
    }
    json += "]";
    PQclear(res);
    return json;
}

std::vector<CommandSchedule> TimescaleStorage::ListCommandSchedules() const {
    std::vector<CommandSchedule> out;
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) return out;

    PGresult *res = PQexec(
        m_impl->conn,
        "SELECT id, label, agent_id, command_id, params, "
        "window_start_unix_ms, window_end_unix_ms, interval_sec, "
        "next_run_unix_ms, enabled FROM command_schedules "
        "ORDER BY created_at DESC LIMIT 200;");
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); ++i) {
            out.push_back(ReadScheduleRow(res, i));
        }
    } else {
        std::cerr << "ListCommandSchedules error: "
                  << PQerrorMessage(m_impl->conn);
    }
    PQclear(res);
    return out;
}

std::vector<CommandSchedule> TimescaleStorage::ListDueCommandSchedules(
    int64_t now_ms) const {
    std::vector<CommandSchedule> out;
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) return out;

    const int64_t kToleranceMs = 2000;
    std::string sql =
        "SELECT id, label, agent_id, command_id, params, "
        "window_start_unix_ms, window_end_unix_ms, interval_sec, "
        "next_run_unix_ms, enabled FROM command_schedules "
        "WHERE enabled AND window_start_unix_ms <= " +
        std::to_string(now_ms) + " AND window_end_unix_ms >= " +
        std::to_string(now_ms) + " AND next_run_unix_ms BETWEEN " +
        std::to_string(now_ms - kToleranceMs) + " AND " +
        std::to_string(now_ms + kToleranceMs) +
        " ORDER BY next_run_unix_ms ASC LIMIT 20;";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); ++i) {
            out.push_back(ReadScheduleRow(res, i));
        }
    } else {
        std::cerr << "ListDueCommandSchedules error: "
                  << PQerrorMessage(m_impl->conn);
    }
    PQclear(res);
    return out;
}

void TimescaleStorage::RescheduleStaleSchedules(int64_t now_ms) const {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) return;

    const int64_t kToleranceMs = 2000;
    // Re-bases overdue schedules to the next interval slot after now without
    // running a catch-up burst (e.g. after the collector was down).
    std::string sql =
        "UPDATE command_schedules "
        "SET next_run_unix_ms = window_start_unix_ms + "
        "  interval_sec * 1000 * ((( " +
        std::to_string(now_ms) +
        " - window_start_unix_ms) / (interval_sec * 1000)) + 1) "
        "WHERE enabled AND window_start_unix_ms <= " +
        std::to_string(now_ms - kToleranceMs) +
        " AND window_end_unix_ms >= " + std::to_string(now_ms) +
        " AND next_run_unix_ms < " +
        std::to_string(now_ms - kToleranceMs) + ";";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    PQclear(res);
}

bool TimescaleStorage::CreateCommandSchedule(const CommandSchedule &schedule,
                                             std::string *err) {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) {
        if (err) *err = "storage not available";
        return false;
    }

    std::string sql =
        "INSERT INTO command_schedules (id, label, agent_id, command_id, "
        "params, window_start_unix_ms, window_end_unix_ms, interval_sec, "
        "next_run_unix_ms, enabled) VALUES (" +
        EscapeLiteral(m_impl->conn, schedule.id) + ", " +
        EscapeLiteral(m_impl->conn, schedule.label) + ", " +
        EscapeLiteral(m_impl->conn, schedule.agent_id) + ", " +
        EscapeLiteral(m_impl->conn, schedule.command_id) + ", " +
        JsonbLiteral(m_impl->conn, schedule.params_json) + ", " +
        std::to_string(schedule.window_start_unix_ms) + ", " +
        std::to_string(schedule.window_end_unix_ms) + ", " +
        std::to_string(schedule.interval_sec) + ", " +
        std::to_string(schedule.next_run_unix_ms) + ", " +
        (schedule.enabled ? "TRUE" : "FALSE") + ");";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (!ok && err) {
        *err = std::string(PQerrorMessage(m_impl->conn));
    }
    PQclear(res);
    return ok;
}

bool TimescaleStorage::SetCommandScheduleEnabled(const std::string &id,
                                                 bool enabled,
                                                 std::string *err) {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) {
        if (err) *err = "storage not available";
        return false;
    }

    std::string sql =
        "UPDATE command_schedules SET enabled = " +
        std::string(enabled ? "TRUE" : "FALSE") + " WHERE id = " +
        EscapeLiteral(m_impl->conn, id) + ";";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (ok && std::string(PQcmdTuples(res)) == "0") {
        if (err) *err = "schedule not found";
        ok = false;
    }
    if (!ok && err) {
        *err = std::string(PQerrorMessage(m_impl->conn));
    }
    PQclear(res);
    return ok;
}

bool TimescaleStorage::DeleteCommandSchedule(const std::string &id,
                                             std::string *err) {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) {
        if (err) *err = "storage not available";
        return false;
    }

    std::string sql =
        "DELETE FROM command_schedules WHERE id = " +
        EscapeLiteral(m_impl->conn, id) + ";";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (ok && std::string(PQcmdTuples(res)) == "0") {
        if (err) *err = "schedule not found";
        ok = false;
    }
    if (!ok && err) {
        *err = std::string(PQerrorMessage(m_impl->conn));
    }
    PQclear(res);
    return ok;
}

bool TimescaleStorage::AdvanceCommandScheduleNextRun(
    const std::string &id, int64_t expected_ms, int64_t new_next_ms) {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) return false;

    std::string sql =
        "UPDATE command_schedules SET next_run_unix_ms = " +
        std::to_string(new_next_ms) + " WHERE id = " +
        EscapeLiteral(m_impl->conn, id) + " AND next_run_unix_ms = " +
        std::to_string(expected_ms) + " AND enabled;";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    bool ok = PQresultStatus(res) == PGRES_COMMAND_OK &&
              std::string(PQcmdTuples(res)) == "1";
    PQclear(res);
    return ok;
}

int64_t TimescaleStorage::InsertCommandRun(const CommandRun &run,
                                           std::string *err) {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) {
        if (err) *err = "storage not available";
        return -1;
    }

    std::string sql =
        "INSERT INTO command_runs (schedule_id, agent_id, command_id, "
        "scheduled_unix_ms, started_unix_ms) VALUES (" +
        EscapeLiteral(m_impl->conn, run.schedule_id) + ", " +
        EscapeLiteral(m_impl->conn, run.agent_id) + ", " +
        EscapeLiteral(m_impl->conn, run.command_id) + ", " +
        std::to_string(run.scheduled_unix_ms) + ", " +
        std::to_string(run.started_unix_ms) + ") RETURNING run_id;";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    int64_t run_id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        run_id = std::stoll(PQgetvalue(res, 0, 0));
    } else if (err) {
        *err = std::string(PQerrorMessage(m_impl->conn));
    }
    PQclear(res);
    return run_id;
}

bool TimescaleStorage::FinishCommandRun(const CommandRun &run,
                                        std::string *err) {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) {
        if (err) *err = "storage not available";
        return false;
    }

    std::string sql =
        "UPDATE command_runs SET finished_unix_ms = " +
        std::to_string(run.finished_unix_ms) + ", success = " +
        (run.success ? "TRUE" : "FALSE") + ", error = " +
        EscapeLiteral(m_impl->conn, run.error) + ", summary = " +
        EscapeLiteral(m_impl->conn, run.summary) + ", fields = " +
        JsonbLiteral(m_impl->conn, run.fields_json) + ", issues = " +
        JsonbLiteral(m_impl->conn, run.issues_json.empty() ? "[]"
                                                           : run.issues_json) +
        ", detail = " + EscapeLiteral(m_impl->conn, run.detail) +
        " WHERE run_id = " + std::to_string(run.run_id) + ";";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (!ok && err) {
        *err = std::string(PQerrorMessage(m_impl->conn));
    }
    PQclear(res);
    return ok;
}

std::vector<CommandRun> TimescaleStorage::ListCommandRuns(
    const std::string &agent_id, const std::string &command_id,
    const std::string &schedule_id, int limit) const {
    std::vector<CommandRun> out;
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) return out;

    std::string sql =
        "SELECT run_id, schedule_id, agent_id, command_id, "
        "scheduled_unix_ms, started_unix_ms, finished_unix_ms, success, "
        "error, summary, fields, issues, detail FROM command_runs WHERE 1=1";
    if (!agent_id.empty()) {
        sql += " AND agent_id = " + EscapeLiteral(m_impl->conn, agent_id);
    }
    if (!command_id.empty()) {
        sql += " AND command_id = " + EscapeLiteral(m_impl->conn, command_id);
    }
    if (!schedule_id.empty()) {
        sql +=
            " AND schedule_id = " + EscapeLiteral(m_impl->conn, schedule_id);
    }
    if (limit <= 0 || limit > 200) limit = 200;
    sql += " ORDER BY started_unix_ms DESC LIMIT " + std::to_string(limit) +
           ";";

    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); ++i) {
            out.push_back(ReadRunRow(res, i));
        }
    } else {
        std::cerr << "ListCommandRuns error: " << PQerrorMessage(m_impl->conn);
    }
    PQclear(res);
    return out;
}

bool TimescaleStorage::PruneCommandRuns(int64_t older_than_unix_ms,
                                        int64_t *removed) const {
    if (removed) *removed = 0;
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();
    if (!m_impl->conn || PQstatus(m_impl->conn) != CONNECTION_OK) return false;

    std::string sql =
        "DELETE FROM command_runs WHERE started_unix_ms < " +
        std::to_string(older_than_unix_ms) + ";";
    PGresult *res = PQexec(m_impl->conn, sql.c_str());
    bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    if (ok && removed) {
        *removed = std::stoll(PQcmdTuples(res));
    }
    PQclear(res);
    return ok;
}

bool TimescaleStorage::IsHealthy() const {
    std::lock_guard lock(m_impl->write_mutex);
    EnsureConnected();

    if (!m_impl->conn) return false;
    if (PQstatus(m_impl->conn) != CONNECTION_OK) return false;

    PGresult *res = PQexec(m_impl->conn, "SELECT 1;");
    bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK);
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