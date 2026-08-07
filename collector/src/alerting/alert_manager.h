#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "alert_rule.h"
#include "notifier.h"

namespace pudimcollector::alerting {

// A record of an alert state transition (firing or resolved), used for the
// dashboard history and the /alert-history endpoint.
struct AlertRecord {
    std::string rule_id;
    std::string rule_name;
    std::string agent_id;
    std::string check_type;
    std::string target;
    std::string severity;
    std::string status;      // "firing" | "resolved"
    double value = 0.0;
    double threshold = 0.0;
    std::string detail;
    int64_t time_ms = 0;

    std::string ToJson() const;
};

// In-memory alert state machine. Evaluates metric batches against loaded rules
// and pushes state transitions to notifiers. Thread-safe.
//
// State per (rule, agent, target):
//   OK → FIRING (threshold breached or probe failed)
//   FIRING → repeat notification after rule.repeat_interval_sec
//   FIRING → RESOLVED (metric back within bounds)
class AlertManager {
public:
    AlertManager() = default;

    // Clears rules, loads from JSON file. Returns false and fills `error` on
    // parse failure (previous rules are preserved).
    bool LoadRulesFromFile(const std::string &path, std::string &error);

    // Parses a JSON document (rules file format). Returns false on error.
    bool LoadRulesFromJson(const std::string &json, std::string &error);

    // Takes ownership of a notifier channel (e.g. LogNotifier).
    void AddNotifier(std::unique_ptr<Notifier> notifier);

    // Evaluates a batch of metrics (after a successful storage write).
    void Evaluate(const std::string &agent_id,
                  const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics);

    // Snapshot accessors (JSON for the HTTP endpoints).
    std::string ActiveAlertsJson() const;
    std::string AlertHistoryJson(size_t max_events = 200) const;
    std::string RulesJson() const;

    // Marks a firing alert acknowledged (dashboard "Acknowledge" action).
    // No-op if the alert is not currently firing. Returns true if changed.
    bool Ack(const std::string &rule_id, const std::string &agent_id,
             const std::string &target);

    size_t ActiveAlertCount() const;
    uint64_t TotalAlertsFired() const;
    size_t RuleCount() const { return m_rules.size(); }
    bool Enabled() const { return !m_rules.empty(); }

private:
    struct FiringState {
        bool firing = false;
        bool acknowledged = false;
        int64_t first_fired_ms = 0;
        int64_t last_notified_ms = 0;
        double last_value = 0.0;
        std::string last_detail;
    };

    static std::string StateKey(const std::string &rule_id,
                                const std::string &agent_id,
                                const std::string &target);

    const AlertRule *FindRule(const std::string &id) const;
    void TrimHistory();

    std::vector<AlertRule> m_rules;
    std::unordered_map<std::string, FiringState> m_states;
    std::vector<std::unique_ptr<Notifier>> m_notifiers;
    // History is a bounded list; newest appended at the end.
    std::vector<AlertRecord> m_history;
    mutable std::mutex m_mutex;
    uint64_t m_alerts_fired_total = 0;
};

} // namespace pudimcollector::alerting
