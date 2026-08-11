#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "metrics.pb.h"

namespace pudimcollector {

// Configuration for connecting to TimescaleDB/PostgreSQL.
struct StorageConfig {
    std::string host = "localhost";
    int port = 5432;
    std::string dbname = "pudimnetmon";
    std::string user = "pudim";
    std::string password = "pudim";
    // Max rows per batch insert transaction.
    int batch_size = 500;
};

struct StorageStats {
    uint64_t metrics_written = 0;
    uint64_t batches_written = 0;
    uint64_t errors = 0;
    // Cumulative insert latency in milliseconds.
    uint64_t insert_latency_total_ms = 0;
};

// TimescaleDB-backed storage for network metrics.
class TimescaleStorage {
public:
    explicit TimescaleStorage(StorageConfig config);
    ~TimescaleStorage();

    // Connects to the database and applies schema (idempotent CREATE TABLE IF NOT EXISTS).
    bool Connect();

    // Inserts a batch of metrics. Returns true on success, false on error.
    // Metrics are batched internally into transactions of `config.batch_size`.
    bool InsertMetrics(const std::string &agent_id,
                       int64_t batch_timestamp_unix_ms,
                       const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics);

    // Queries recent metrics for the dashboard. `agent_id` and `check_type`
    // filter results; empty string means "all". Returns a JSON array string:
    //   [{"time_ms":..., "agent_id":"...", "check_type":"...",
    //     "target":"...", "value":..., "success":true}, ...]
    std::string QueryMetricsJson(const std::string &agent_id,
                                 const std::string &check_type,
                                 int64_t window_seconds) const;

    bool IsHealthy() const;

    StorageStats GetStats() const;

private:
    // Re-establishes the DB connection if it is missing or dead (e.g. after a
    // TimescaleDB restart). Called at the top of every method that runs SQL.
    void EnsureConnected() const;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace pudimcollector