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

// One persisted schedule for running a pre-set agent command repeatedly inside a time window
struct CommandSchedule {
    std::string id;
    std::string label;
    std::string agent_id;
    std::string command_id;
    std::string params_json = "{}";
    int64_t window_start_unix_ms = 0;
    int64_t window_end_unix_ms = 0;
    int64_t interval_sec = 0;
    int64_t next_run_unix_ms = 0;
    bool enabled = false;
};

// One execution of a scheduled (or ad-hoc) command, as persisted in the command_runs history table.
struct CommandRun {
    int64_t run_id = 0;
    std::string schedule_id;
    std::string agent_id;
    std::string command_id;
    int64_t scheduled_unix_ms = 0;
    int64_t started_unix_ms = 0;
    int64_t finished_unix_ms = 0;
    bool success = false;
    // True once FinishCommandRun() stored the result payload.
    bool has_result = false;
    std::string error;
    std::string summary;
    // JSON object / array literals ("" serializes to defaults).
    std::string fields_json;
    std::string issues_json;
    std::string detail;
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

    // Connects to the database and applies the schema.
    bool Connect();

    // Inserts a batch of metrics. Returns true on success.
    // Metrics are batched internally into transactions of config.batch_size.
    bool InsertMetrics(const std::string &agent_id,
                       int64_t batch_timestamp_unix_ms,
                       const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics);

    // Queries recent metrics for the dashboard. Empty agent_id and check_type
    // match everything. Returns a JSON array of metric rows.
    std::string QueryMetricsJson(const std::string &agent_id,
                                 const std::string &check_type,
                                 int64_t window_seconds) const;

    // Every method below requires a healthy connection. 
    // Return empty/false/-1 when the database is unavailable.

    // All schedules, most recently created first.
    std::vector<CommandSchedule> ListCommandSchedules() const;

    // Schedules that are enabled, inside their window and whose next_run is
    // within the claim tolerance around `now_ms` (used by the scheduler).
    std::vector<CommandSchedule> ListDueCommandSchedules(int64_t now_ms) const;

    // Re-bases the next_run of enabled schedules whose slot fell behind (for
    // example while the collector was down) to the next future slot without
    // running a catch-up burst.
    void RescheduleStaleSchedules(int64_t now_ms) const;

    bool CreateCommandSchedule(const CommandSchedule &schedule,
                               std::string *err);

    bool SetCommandScheduleEnabled(const std::string &id, bool enabled,
                                   std::string *err);

    bool DeleteCommandSchedule(const std::string &id, std::string *err);

    // Atomically advances a schedule's next_run when it still equals
    // `expected_ms` (claim guard against duplicate dispatch).
    bool AdvanceCommandScheduleNextRun(const std::string &id,
                                       int64_t expected_ms,
                                       int64_t new_next_ms);

    // Inserts a started run and returns its run_id (or -1 on failure).
    int64_t InsertCommandRun(const CommandRun &run, std::string *err);

    // Stores the result payload of a previously inserted run.
    bool FinishCommandRun(const CommandRun &run, std::string *err);

    // Recent command executions, newest first. Empty agent_id/command_id 
    // match everything, with a cap.
    std::vector<CommandRun> ListCommandRuns(const std::string &agent_id,
                                            const std::string &command_id,
                                            int limit) const;

    // Deletes run history older than `older_than_unix_ms`
    bool PruneCommandRuns(int64_t older_than_unix_ms, int64_t *removed) const;

    bool IsHealthy() const;

    StorageStats GetStats() const;

private:
    // Reconnects when the connection is missing or stale. Called before every
    // method that runs SQL.
    void EnsureConnected() const;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace pudimcollector