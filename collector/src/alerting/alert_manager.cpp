#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "alert_manager.h"

namespace pudimcollector::alerting {

namespace {

constexpr size_t kMaxHistory = 1000;
constexpr char kKeySep = '\x1f';  // never occurs in rule ids or targets

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string EscapeJson(const std::string &s) {
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

std::string FormatDouble(double v) {
    if (v == static_cast<long long>(v)) {
        return std::to_string(static_cast<long long>(v));
    }
    std::string s = std::to_string(v);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// Factories for records and notifications.
AlertRecord MakeRecordFor(const AlertRule &rule,
                          const std::string &agent_id,
                          const pudimnetmon::Metric &m, double value,
                          const std::string &status, int64_t now) {
    AlertRecord rec;
    rec.rule_id = rule.id;
    rec.rule_name = rule.name;
    rec.agent_id = agent_id;
    rec.check_type = CheckTypeToString(m.check_type());
    rec.target = m.target();
    rec.severity = rule.severity;
    rec.status = status;
    rec.value = value;
    rec.threshold = rule.threshold;
    rec.detail = m.detail();
    rec.time_ms = now;
    return rec;
}

AlertNotification MakeNotification(const AlertRecord &rec) {
    AlertNotification n;
    n.rule_id = rec.rule_id;
    n.rule_name = rec.rule_name;
    n.severity = rec.severity;
    n.agent_id = rec.agent_id;
    n.check_type = rec.check_type;
    n.target = rec.target;
    n.status = rec.status;
    n.value = rec.value;
    n.threshold = rec.threshold;
    n.detail = rec.detail;
    n.time_ms = rec.time_ms;
    return n;
}

} // anonymous namespace

std::string AlertRecord::ToJson() const {
    std::string json = "{";
    json += "\"rule_id\":\"" + EscapeJson(rule_id) + "\",";
    json += "\"rule_name\":\"" + EscapeJson(rule_name) + "\",";
    json += "\"agent_id\":\"" + EscapeJson(agent_id) + "\",";
    json += "\"check_type\":\"" + EscapeJson(check_type) + "\",";
    json += "\"target\":\"" + EscapeJson(target) + "\",";
    json += "\"severity\":\"" + EscapeJson(severity) + "\",";
    json += "\"status\":\"" + EscapeJson(status) + "\",";
    json += "\"value\":" + FormatDouble(value) + ",";
    json += "\"threshold\":" + FormatDouble(threshold) + ",";
    json += "\"detail\":\"" + EscapeJson(detail) + "\",";
    json += "\"time_ms\":" + std::to_string(time_ms);
    json += "}";
    return json;
}

std::string AlertManager::StateKey(const std::string &rule_id,
                                   const std::string &agent_id,
                                   const std::string &target) {
    return rule_id + kKeySep + agent_id + kKeySep + target;
}

namespace {

// Serializes one rule to its canonical JSON representation
nlohmann::json RuleToJson(const AlertRule &r) {
    nlohmann::json j;
    j["id"] = r.id;
    j["name"] = r.name;
    j["agent_id"] = r.agent_id;
    j["check_type"] = r.check_type;
    j["target"] = r.target;
    j["metric"] = r.metric_field;
    j["op"] = r.greater_than ? ">" : "<";
    j["threshold"] = r.threshold;
    j["repeat_interval_sec"] = r.repeat_interval_sec;
    j["severity"] = r.severity;
    j["on_failure"] = r.on_failure;
    return j;
}

} // anonymous namespace

bool AlertManager::LoadRulesFromFile(const std::string &path, std::string &error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open rules file: " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    if (!LoadRulesFromJson(ss.str(), error)) return false;
    // Remember the path so UI edits can be persisted back to the same file.
    m_rules_path = path;
    return true;
}

bool AlertManager::ParseRule(const nlohmann::json &r, AlertRule &rule,
                             std::string &error) {
    rule = AlertRule{};
    rule.id = r.value("id", "");
    if (rule.id.empty()) {
        error = "rule missing required 'id'";
        return false;
    }
    rule.name = r.value("name", rule.id);
    rule.agent_id = r.value("agent_id", "");
    rule.check_type = r.value("check_type", "");
    rule.target = r.value("target", "");
    rule.metric_field = r.value("metric", "");
    rule.severity = r.value("severity", "warning");
    rule.threshold = r.value("threshold", 0.0);
    rule.repeat_interval_sec = r.value("repeat_interval_sec", 300);
    rule.on_failure = r.value("on_failure", false);

    std::string op = r.value("op", ">");
    if (op == ">") {
        rule.greater_than = true;
    } else if (op == "<") {
        rule.greater_than = false;
    } else {
        error = "rule '" + rule.id + "' has invalid op '" + op +
                "' (expected '>' or '<')";
        return false;
    }
    if (!rule.on_failure && rule.metric_field.empty()) {
        error = "rule '" + rule.id + "' missing required 'metric' field";
        return false;
    }
    return true;
}

bool AlertManager::LoadRulesFromJson(const std::string &json, std::string &error) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error &e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    std::vector<AlertRule> new_rules;
    if (doc.contains("rules") && !doc["rules"].is_null()) {
        if (!doc["rules"].is_array()) {
            error = "'rules' must be an array";
            return false;
        }
        for (const auto &r : doc["rules"]) {
            AlertRule rule;
            if (!ParseRule(r, rule, error)) return false;
            new_rules.push_back(std::move(rule));
        }
    }

    std::string webhook_url = doc.value("webhook_url", "");

    {
        std::lock_guard lock(m_mutex);
        m_rules = std::move(new_rules);
        m_states.clear();  // reset per-rule state for the new rule set
        m_webhook_url = webhook_url;
        m_notifiers.clear();
        m_notifiers.push_back(std::make_unique<LogNotifier>());
        if (!webhook_url.empty() && webhook_url != "log") {
            m_notifiers.push_back(std::make_unique<WebhookNotifier>(webhook_url));
        }
    }
    return true;
}

bool AlertManager::WriteRulesFileLocked(const std::vector<AlertRule> &rules,
                                        const std::string &path,
                                        std::string &error) const {
    nlohmann::json doc;
    doc["webhook_url"] = m_webhook_url;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &r : rules) arr.push_back(RuleToJson(r));
    doc["rules"] = arr;

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "cannot write alert rules file: " + path;
        return false;
    }
    out << doc.dump(2) << "\n";
    if (!out.good()) {
        error = "error writing alert rules file: " + path;
        return false;
    }
    return true;
}

bool AlertManager::UpsertRule(const std::string &rule_json, std::string &error) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(rule_json);
    } catch (const nlohmann::json::parse_error &e) {
        error = std::string("invalid rule JSON: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        error = "rule must be a JSON object";
        return false;
    }

    AlertRule rule;
    if (!ParseRule(doc, rule, error)) return false;

    std::lock_guard lock(m_mutex);
    std::vector<AlertRule> candidate = m_rules;
    bool replaced = false;
    for (auto &r : candidate) {
        if (r.id == rule.id) {
            r = rule;
            replaced = true;
            break;
        }
    }
    if (!replaced) candidate.push_back(std::move(rule));

    // Persist before committing so the file always mirrors the in-memory set.
    if (!m_rules_path.empty() &&
        !WriteRulesFileLocked(candidate, m_rules_path, error)) {
        return false;
    }
    m_rules = std::move(candidate);
    return true;
}

bool AlertManager::DeleteRule(const std::string &rule_id, std::string &error) {
    if (rule_id.empty()) {
        error = "rule_id is required";
        return false;
    }

    std::lock_guard lock(m_mutex);
    std::vector<AlertRule> candidate = m_rules;
    const size_t before = candidate.size();
    candidate.erase(
        std::remove_if(candidate.begin(), candidate.end(),
                       [&rule_id](const AlertRule &r) { return r.id == rule_id; }),
        candidate.end());
    if (candidate.size() == before) {
        error = "no rule with id '" + rule_id + "'";
        return false;
    }

    if (!m_rules_path.empty() &&
        !WriteRulesFileLocked(candidate, m_rules_path, error)) {
        return false;
    }
    m_rules = std::move(candidate);

    // Forget firing/ack state of the deleted rule (keys are id + agent + target).
    const std::string prefix = rule_id + kKeySep;
    for (auto st = m_states.begin(); st != m_states.end();) {
        if (st->first.compare(0, prefix.size(), prefix) == 0) {
            st = m_states.erase(st);
        } else {
            ++st;
        }
    }
    return true;
}

bool AlertManager::SaveRulesFile(std::string &error) const {
    if (m_rules_path.empty()) {
        error = "no alert rules file configured (--alert-rules-path unset)";
        return false;
    }
    std::lock_guard lock(m_mutex);
    return WriteRulesFileLocked(m_rules, m_rules_path, error);
}

bool AlertManager::SaveRulesFile(const std::string &path,
                                 std::string &error) const {
    std::lock_guard lock(m_mutex);
    return WriteRulesFileLocked(m_rules, path, error);
}

void AlertManager::Evaluate(
    const std::string &agent_id,
    const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics) {
    if (m_rules.empty()) return;

    std::vector<AlertNotification> notifications;
    std::vector<Notifier *> notifiers;

    {
        std::lock_guard lock(m_mutex);
        auto now = NowMs();

        for (const auto &nt : m_notifiers) {
            notifiers.push_back(nt.get());
        }

        for (const auto &m : metrics) {
            for (const auto &rule : m_rules) {
                if (!rule.MatchesAgent(agent_id)) continue;
                if (!rule.check_type.empty()) {
                    pudimnetmon::CheckType ct;
                    if (CheckTypeFromString(rule.check_type, ct) &&
                        ct != m.check_type()) {
                        continue;
                    }
                }
                if (!rule.MatchesTarget(m.target())) continue;

                auto &st = m_states[StateKey(rule.id, agent_id, m.target())];

                bool violated = false;
                double value = 0.0;
                std::string detail;

                if (rule.on_failure) {
                    if (!m.success()) {
                        violated = true;
                        detail = m.detail();
                    }
                } else if (m.success() &&
                           GetMetricValue(m, rule.metric_field, value)) {
                    violated = rule.greater_than
                                   ? (value > rule.threshold)
                                   : (value < rule.threshold);
                }
                // A failed field-based probe has no value, so it cannot fire
                // or resolve the alert.

                if (violated) {
                    if (!st.firing) {
                        st.firing = true;
                        st.first_fired_ms = now;
                        st.last_notified_ms = now;
                        st.last_value = value;
                        st.last_detail = detail;
                        m_alerts_fired_total++;

                        AlertRecord rec = MakeRecordFor(rule, agent_id, m, value,
                                                        "firing", now);
                        m_history.push_back(rec);
                        TrimHistory();
                        notifications.push_back(MakeNotification(rec));
                    } else {
                        // Re-notify once the repeat interval has elapsed.
                        int64_t interval_ms =
                            static_cast<int64_t>(rule.repeat_interval_sec) * 1000;
                        if (now - st.last_notified_ms >= interval_ms) {
                            st.last_notified_ms = now;
                            st.last_value = value;
                            st.last_detail = detail;
                            m_alerts_fired_total++;

                            AlertRecord rec = MakeRecordFor(rule, agent_id, m, value,
                                                            "firing", now);
                            m_history.push_back(rec);
                            TrimHistory();
                            notifications.push_back(MakeNotification(rec));
                        }
                    }
                } else if (st.firing) {
                    double last_value = st.last_value;
                    std::string last_detail = st.last_detail;
                    st = FiringState{};

                    AlertRecord rec = MakeRecordFor(rule, agent_id, m, last_value,
                                                    "resolved", now);
                    rec.detail = last_detail;
                    m_history.push_back(rec);
                    TrimHistory();
                    notifications.push_back(MakeNotification(rec));
                }
            }
        }
    }

    // Notify outside the lock so blocking network calls cannot stall evaluation.
    for (const auto &alert : notifications) {
        for (Notifier *n : notifiers) {
            n->Notify(alert);
        }
    }
}


void AlertManager::AddNotifier(std::unique_ptr<Notifier> notifier) {
    std::lock_guard lock(m_mutex);
    m_notifiers.push_back(std::move(notifier));
}


std::string AlertManager::ActiveAlertsJson() const {
    std::lock_guard lock(m_mutex);
    std::string json = "[";
    bool first = true;
    for (const auto &[key, st] : m_states) {
        if (!st.firing) continue;

        auto sep1 = key.find(kKeySep);
        auto sep2 = key.find(kKeySep, sep1 == std::string::npos ? 0 : sep1 + 1);
        std::string rule_id = key.substr(0, sep1);
        std::string agent = key.substr(sep1 + 1, sep2 - sep1 - 1);
        std::string target = key.substr(sep2 + 1);

        const AlertRule *rule = FindRule(rule_id);

        if (!first) json += ",";
        first = false;
        json += "{";
        json += "\"rule_id\":\"" + EscapeJson(rule_id) + "\",";
        json += "\"rule_name\":\"" +
                EscapeJson(rule ? rule->name : rule_id) + "\",";
        json += "\"severity\":\"" +
                EscapeJson(rule ? rule->severity : "unknown") + "\",";
        json += "\"agent_id\":\"" + EscapeJson(agent) + "\",";
        json += "\"target\":\"" + EscapeJson(target) + "\",";
        json += "\"value\":" + FormatDouble(st.last_value) + ",";
        json += "\"threshold\":" +
                (rule ? FormatDouble(rule->threshold) : "0") + ",";
        json += "\"acknowledged\":" +
                std::string(st.acknowledged ? "true" : "false") + ",";
        json += "\"fired_ms\":" + std::to_string(st.first_fired_ms);
        json += "}";
    }
    json += "]";
    return json;
}

std::string AlertManager::AlertHistoryJson(size_t max_events) const {
    std::lock_guard lock(m_mutex);
    std::string json = "[";
    size_t count = 0;
    for (auto it = m_history.rbegin();
         it != m_history.rend() && count < max_events; ++it, ++count) {
        if (count > 0) json += ",";
        json += it->ToJson();
    }
    json += "]";
    return json;
}

std::string AlertManager::RulesJson() const {
    std::lock_guard lock(m_mutex);
    nlohmann::json doc;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &r : m_rules) arr.push_back(RuleToJson(r));
    doc["rules"] = arr;
    return doc.dump();
}

size_t AlertManager::ActiveAlertCount() const {
    std::lock_guard lock(m_mutex);
    size_t count = 0;
    for ([[maybe_unused]] const auto &[key, st] : m_states) {
        if (st.firing) count++;
    }
    return count;
}

bool AlertManager::Ack(const std::string &rule_id, const std::string &agent_id,
                       const std::string &target) {
    std::lock_guard lock(m_mutex);
    auto it = m_states.find(StateKey(rule_id, agent_id, target));
    if (it == m_states.end() || !it->second.firing || it->second.acknowledged) {
        return false;
    }
    it->second.acknowledged = true;
    return true;
}

uint64_t AlertManager::TotalAlertsFired() const {
    std::lock_guard lock(m_mutex);
    return m_alerts_fired_total;
}

void AlertManager::TrimHistory() {
    if (m_history.size() > kMaxHistory) {
        m_history.erase(m_history.begin(),
                        m_history.begin() + (m_history.size() - kMaxHistory));
    }
}

const AlertRule *AlertManager::FindRule(const std::string &id) const {
    for (const auto &r : m_rules) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

} // namespace pudimcollector::alerting

