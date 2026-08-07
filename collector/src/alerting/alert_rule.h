#pragma once

#include <string>

#include "metrics.pb.h"

namespace pudimcollector::alerting {

// A single alert rule: fires when a metric violates a threshold (or when a
// probe fails, if on_failure is set).
struct AlertRule {
    std::string id;             // unique rule identifier
    std::string name;           // human-readable name
    std::string agent_id;       // empty = matches all agents
    std::string check_type;     // empty = matches all check types ("tcp_connect", ...)
    std::string target;         // empty = matches all targets
    std::string metric_field;   // "latency_ms" | "packet_loss_pct" | "jitter_ms" |
                                // "rtt_ms" | "status_code" (unused when on_failure)
    bool greater_than = true;   // comparison operator: > (true) or < (false)
    double threshold = 0.0;     // threshold for comparison
    int repeat_interval_sec = 300;  // min seconds between repeat notifications
    std::string severity = "warning";  // info | warning | critical
    bool on_failure = false;    // if true, fires when the probe itself fails

    bool MatchesAgent(const std::string &agent) const {
        return agent_id.empty() || agent_id == agent;
    }
    bool MatchesTarget(const std::string &t) const {
        return target.empty() || target == t;
    }
};

// Map a proto CheckType enum to its config string ("tcp_connect", ...).
std::string CheckTypeToString(pudimnetmon::CheckType type);
// Inverse of CheckTypeToString. Returns false for unknown strings.
bool CheckTypeFromString(const std::string &s, pudimnetmon::CheckType &out);

// Extracts the numeric value of `field` from a Metric. Returns false when the
// field is not set on the metric.
bool GetMetricValue(const pudimnetmon::Metric &m, const std::string &field,
                    double &out);

} // namespace pudimcollector::alerting
