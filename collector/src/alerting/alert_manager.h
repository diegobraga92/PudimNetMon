#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "alert_rule.h"
#include "notifier.h"

namespace pudimcollector::alerting {

// One alert state transition shown in the dashboard alert history.
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

// Thread-safe in-memory alert state machine.
// State is tracked per (rule, agent, target). A threshold breach or a failed
// probe moves OK to FIRING. FIRING moves back to RESOLVED when the metric
// recovers and repeats notifications every rule.repeat_interval_sec.
class AlertManager {
public:
    AlertManager() = default;

    // Loads rules from a JSON file. Existing rules are kept on parse failure.
    bool LoadRulesFromFile(const std::string &path, std::string &error);

    // Parses rules from a JSON document.
    bool LoadRulesFromJson(const std::string &json, std::string &error);

    // Registers a notifier.
    void AddNotifier(std::unique_ptr<Notifier> notifier);

    // Evaluates a metric batch against the loaded rules.
    void Evaluate(const std::string &agent_id,
                  const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics);

    // JSON snapshots for the HTTP endpoints.
    std::string ActiveAlertsJson() const;
    std::string AlertHistoryJson(size_t max_events = 200) const;
    std::string RulesJson() const;

    // Marks a firing alert as acknowledged. Returns false when the alert is
    // not firing or is already acknowledged.
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
    // Bounded history with the newest record at the end.
    std::vector<AlertRecord> m_history;
    mutable std::mutex m_mutex;
    uint64_t m_alerts_fired_total = 0;
};

} // namespace pudimcollector::alerting
