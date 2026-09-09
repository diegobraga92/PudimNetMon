#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

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
// State is tracked per (rule, agent, target).
class AlertManager {
public:
    AlertManager() = default;

    bool LoadRulesFromFile(const std::string &path, std::string &error);

    bool LoadRulesFromJson(const std::string &json, std::string &error);

    bool UpsertRule(const std::string &rule_json, std::string &error);

    bool DeleteRule(const std::string &rule_id, std::string &error);

    bool SaveRulesFile(std::string &error) const;
    bool SaveRulesFile(const std::string &path, std::string &error) const;

    std::string RulesFilePath() const { return m_rules_path; }

    void AddNotifier(std::unique_ptr<Notifier> notifier);

    void Evaluate(const std::string &agent_id,
                  const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics);

    // JSON snapshots for the HTTP endpoints.
    std::string ActiveAlertsJson() const;
    std::string AlertHistoryJson(size_t max_events = 200) const;
    std::string RulesJson() const;

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

    static bool ParseRule(const nlohmann::json &r, AlertRule &rule,
                          std::string &error);

    // Writes `rules` to `path` as the rules file (webhook_url preserved) while
    // m_mutex is held.
    bool WriteRulesFileLocked(const std::vector<AlertRule> &rules,
                              const std::string &path,
                              std::string &error) const;

    std::vector<AlertRule> m_rules;
    std::unordered_map<std::string, FiringState> m_states;
    std::vector<std::unique_ptr<Notifier>> m_notifiers;
    // Bounded history with the newest record at the end.
    std::vector<AlertRecord> m_history;
    mutable std::mutex m_mutex;
    uint64_t m_alerts_fired_total = 0;
    // Rules file that backs the UI edits (empty = in-memory only).
    std::string m_rules_path;
    // Top-level webhook_url from the rules file (kept across UI edits).
    std::string m_webhook_url;
};

} // namespace pudimcollector::alerting
