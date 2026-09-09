// Unit tests for the alerting state machine (no database required).
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "metrics.pb.h"
#include "alerting/alert_manager.h"

using pudimnetmon::CheckType;
using pudimnetmon::Metric;
using pudimnetmon::MetricsBatch;
using pudimcollector::alerting::AlertManager;

namespace {

Metric LatencyMetric(CheckType type, const std::string &target,
                     double latency, bool success = true) {
    Metric m;
    m.set_check_type(type);
    m.set_target(target);
    m.set_latency_ms(latency);
    m.set_success(success);
    m.set_seq(1);
    return m;
}

Metric LossMetric(const std::string &target, double loss_pct) {
    Metric m;
    m.set_check_type(CheckType::CHECK_TYPE_ICMP_PING);
    m.set_target(target);
    m.set_packet_loss_pct(loss_pct);
    m.set_success(true);
    m.set_seq(1);
    return m;
}

void TestLoadRules() {
    AlertManager mgr;
    std::string err;
    bool ok = mgr.LoadRulesFromJson(R"({
        "rules": [
            { "id": "high-latency", "name": "High TCP Latency", "check_type": "tcp_connect",
              "metric": "latency_ms", "op": ">", "threshold": 500, "repeat_interval_sec": 0 },
            { "id": "high-loss", "check_type": "icmp_ping",
              "metric": "packet_loss_pct", "op": ">", "threshold": 5, "severity": "critical" },
            { "id": "dns-failure", "check_type": "dns_resolution",
              "on_failure": true, "severity": "critical" }
        ]
    })", err);
    assert(ok);
    assert(mgr.RuleCount() == 3);
    assert(mgr.Enabled());
    std::cout << "PASS: rules loaded (" << mgr.RuleCount() << " rules)\n";
}

void TestInvalidRule() {
    AlertManager mgr;
    std::string err;
    bool ok = mgr.LoadRulesFromJson(R"({
        "rules": [ { "id": "bad", "check_type": "tcp_connect", "op": "??",
                     "threshold": 1, "metric": "latency_ms" } ]
    })", err);
    assert(!ok);
    assert(!err.empty());
    std::cout << "PASS: invalid rule rejected: " << err << "\n";
}

void TestFiringRepeatResolved() {
    AlertManager mgr;
    std::string err;
    assert(mgr.LoadRulesFromJson(R"({
        "rules": [ { "id": "r1", "check_type": "tcp_connect", "metric": "latency_ms",
                     "op": ">", "threshold": 500, "repeat_interval_sec": 0 } ]
    })", err));

    MetricsBatch batch;
    batch.set_agent_id("agent-a");

    *batch.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_TCP_CONNECT, "example.com:443", 600.0);
    mgr.Evaluate(batch.agent_id(), batch.metrics());
    assert(mgr.ActiveAlertCount() == 1);
    assert(mgr.TotalAlertsFired() == 1);
    std::cout << "PASS: violation fired alert\n";

    // A repeat notification is expected while still firing.
    batch.clear_metrics();
    *batch.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_TCP_CONNECT, "example.com:443", 700.0);
    mgr.Evaluate(batch.agent_id(), batch.metrics());
    assert(mgr.ActiveAlertCount() == 1);
    assert(mgr.TotalAlertsFired() == 2);
    std::cout << "PASS: repeat notification sent while still firing\n";

    // Back within bounds, so the alert resolves.
    batch.clear_metrics();
    *batch.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_TCP_CONNECT, "example.com:443", 100.0);
    mgr.Evaluate(batch.agent_id(), batch.metrics());
    assert(mgr.ActiveAlertCount() == 0);
    std::cout << "PASS: alert resolved\n";

    std::string history = mgr.AlertHistoryJson();
    assert(history.find("firing") != std::string::npos);
    assert(history.find("resolved") != std::string::npos);
    std::cout << "PASS: history contains firing + resolved records\n";
}

void TestFiltering() {
    AlertManager mgr;
    std::string err;
    assert(mgr.LoadRulesFromJson(R"({
        "rules": [
            { "id": "agent-specific", "agent_id": "agent-a", "check_type": "dns_resolution",
              "metric": "latency_ms", "op": ">", "threshold": 100, "repeat_interval_sec": 0 }
        ]
    })", err));

    MetricsBatch b;
    b.set_agent_id("agent-b");
    *b.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_DNS_RESOLUTION, "example.com", 500.0);
    mgr.Evaluate(b.agent_id(), b.metrics());
    assert(mgr.ActiveAlertCount() == 0);
    std::cout << "PASS: agent filter excludes non-matching agent\n";

    b.set_agent_id("agent-a");
    mgr.Evaluate(b.agent_id(), b.metrics());
    assert(mgr.ActiveAlertCount() == 1);
    std::cout << "PASS: agent filter matches\n";
}

void TestOnFailure() {
    AlertManager mgr;
    std::string err;
    assert(mgr.LoadRulesFromJson(R"({
        "rules": [ { "id": "dns-fail", "check_type": "dns_resolution",
                     "on_failure": true, "repeat_interval_sec": 0 } ]
    })", err));

    MetricsBatch b;
    b.set_agent_id("agent-a");
    *b.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_DNS_RESOLUTION, "example.com", 0.0, false);
    mgr.Evaluate(b.agent_id(), b.metrics());
    assert(mgr.ActiveAlertCount() == 1);
    std::cout << "PASS: on_failure rule fired on failed probe\n";

    b.clear_metrics();
    *b.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_DNS_RESOLUTION, "example.com", 10.0, true);
    mgr.Evaluate(b.agent_id(), b.metrics());
    assert(mgr.ActiveAlertCount() == 0);
    std::cout << "PASS: on_failure rule resolved on successful probe\n";
}

void TestActiveAlertsJson() {
    AlertManager mgr;
    std::string err;
    assert(mgr.LoadRulesFromJson(R"({
        "rules": [ { "id": "high-loss", "check_type": "icmp_ping",
                     "metric": "packet_loss_pct", "op": ">", "threshold": 5,
                     "repeat_interval_sec": 0 } ]
    })", err));

    MetricsBatch b;
    b.set_agent_id("agent-a");
    *b.add_metrics() = LossMetric("1.1.1.1", 30.0);
    mgr.Evaluate(b.agent_id(), b.metrics());

    std::string active = mgr.ActiveAlertsJson();
    assert(active.find("high-loss") != std::string::npos);
    assert(active.find("agent-a") != std::string::npos);
    assert(active.find("30") != std::string::npos);
    assert(active.find("\"acknowledged\":false") != std::string::npos);
    std::cout << "PASS: ActiveAlertsJson: " << active << "\n";
}

// Acknowledging a firing alert is reflected in ActiveAlertsJson.
void TestAck() {
    AlertManager mgr;
    std::string err;
    assert(mgr.LoadRulesFromJson(R"({
        "rules": [ { "id": "high-loss", "check_type": "icmp_ping",
                     "metric": "packet_loss_pct", "op": ">", "threshold": 5,
                     "repeat_interval_sec": 0 } ]
    })", err));
    MetricsBatch b;
    b.set_agent_id("agent-a");
    *b.add_metrics() = LossMetric("1.1.1.1", 30.0);
    mgr.Evaluate(b.agent_id(), b.metrics());
    assert(mgr.ActiveAlertCount() == 1);

    assert(!mgr.Ack("nope", "agent-a", "1.1.1.1"));
    assert(mgr.Ack("high-loss", "agent-a", "1.1.1.1"));
    assert(mgr.ActiveAlertsJson().find("\"acknowledged\":true") != std::string::npos);
    assert(!mgr.Ack("high-loss", "agent-a", "1.1.1.1"));
    std::cout << "PASS: alert ack marks firing alert acknowledged\n";
}

// Upsert (create + replace) and delete keep the rule set
// consistent and reject invalid documents without mutating anything.
void TestUpsertDelete() {
    AlertManager mgr;
    std::string err;
    assert(mgr.LoadRulesFromJson(R"({
        "rules": [ { "id": "r1", "check_type": "tcp_connect",
                     "metric": "latency_ms", "op": ">", "threshold": 500,
                     "repeat_interval_sec": 0 } ]
    })", err));

    // Upsert a brand-new rule.
    assert(mgr.UpsertRule(R"({
        "id": "r2", "name": "Packet Loss", "check_type": "icmp_ping",
        "metric": "packet_loss_pct", "op": ">", "threshold": 5,
        "repeat_interval_sec": 60, "severity": "critical"
    })", err));
    assert(mgr.RuleCount() == 2);

    // Upserting an existing id replaces it (no duplicate, new threshold).
    assert(mgr.UpsertRule(R"({
        "id": "r1", "name": "Renamed", "check_type": "tcp_connect",
        "metric": "latency_ms", "op": ">", "threshold": 800,
        "repeat_interval_sec": 0
    })", err));
    assert(mgr.RuleCount() == 2);
    nlohmann::json rules = nlohmann::json::parse(mgr.RulesJson());
    for (const auto &r : rules["rules"]) {
        if (r["id"] == "r1") {
            assert(r["threshold"].get<double>() == 800.0);
            assert(r["name"] == "Renamed");
        }
    }

    // Invalid rule documents are rejected and change nothing.
    assert(!mgr.UpsertRule(R"({
        "id": "bad", "check_type": "icmp_ping", "metric": "packet_loss_pct",
        "op": "??", "threshold": 5
    })", err));
    assert(!err.empty());
    assert(mgr.RuleCount() == 2);

    // Deleting an unknown id is a no-op that reports an error.
    assert(!mgr.DeleteRule("nope", err));
    assert(mgr.RuleCount() == 2);

    // Deleting a firing rule forgets both the rule and its firing state.
    MetricsBatch b;
    b.set_agent_id("agent-a");
    *b.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_TCP_CONNECT, "example.com", 900.0);
    mgr.Evaluate(b.agent_id(), b.metrics());
    assert(mgr.ActiveAlertCount() == 1);

    assert(mgr.DeleteRule("r1", err));
    assert(mgr.RuleCount() == 1);
    assert(mgr.ActiveAlertCount() == 0);
    assert(mgr.RulesJson().find("r1") == std::string::npos);
    std::cout << "PASS: upsert + delete manage the rule set\n";
}

// Rules edited through the UI survive a restart via the rules file round trip,
// including the top-level webhook_url setting.
void TestPersistRulesFile() {
    const std::string path = "/tmp/pudim_alert_rules_test.json";
    std::remove(path.c_str());

    AlertManager writer;
    std::string err;
    assert(writer.LoadRulesFromJson(
        R"({ "webhook_url": "https://hooks.example.test/x", "rules": [] })", err));
    assert(writer.UpsertRule(R"({
        "id": "p1", "name": "Persisted", "check_type": "icmp_ping",
        "metric": "packet_loss_pct", "op": ">", "threshold": 5
    })", err));
    assert(writer.UpsertRule(R"({
        "id": "p2", "name": "Fail", "check_type": "dns_resolution",
        "on_failure": true, "severity": "critical"
    })", err));

    assert(writer.SaveRulesFile(path, err));
    assert(!err.empty() == false);  // sanity: err untouched on success
    std::cout << "PASS: rules persisted to " << path << "\n";

    // A fresh manager restores the rules + webhook from the file.
    AlertManager reader;
    assert(reader.LoadRulesFromFile(path, err));
    assert(reader.RuleCount() == 2);
    assert(reader.RulesFilePath() == path);
    assert(reader.RulesJson().find("p1") != std::string::npos);
    assert(reader.RulesJson().find("p2") != std::string::npos);

    // Saving through the remembered path round-trips (update + delete).
    assert(reader.UpsertRule(R"({
        "id": "p1", "name": "Persisted v2", "check_type": "icmp_ping",
        "metric": "packet_loss_pct", "op": ">", "threshold": 7
    })", err));
    assert(reader.RuleCount() == 2);

    // The upsert must be on disk immediately (HTTP mutations persist per call).
    AlertManager reader_mid;
    assert(reader_mid.LoadRulesFromFile(path, err));
    assert(reader_mid.RuleCount() == 2);
    assert(reader_mid.RulesJson().find("Persisted v2") != std::string::npos);
    assert(reader_mid.RulesJson().find("p2") != std::string::npos);

    assert(reader.DeleteRule("p2", err));
    assert(reader.RuleCount() == 1);

    // The delete must be on disk immediately too.
    AlertManager reader2;
    assert(reader2.LoadRulesFromFile(path, err));
    assert(reader2.RuleCount() == 1);
    assert(reader2.RulesJson().find("p2") == std::string::npos);
    assert(reader2.RulesJson().find("Persisted v2") != std::string::npos);
    std::cout << "PASS: rules file survives restart (webhook + edits)\n";

    // No path configured => in-memory edits still work but cannot persist.
    AlertManager mem;
    assert(mem.LoadRulesFromJson(R"({ "rules": [] })", err));
    assert(!mem.SaveRulesFile(err));
    assert(!err.empty());
    std::remove(path.c_str());
}

} // anonymous namespace

int main() {
    TestLoadRules();
    TestInvalidRule();
    TestFiringRepeatResolved();
    TestFiltering();
    TestOnFailure();
    TestActiveAlertsJson();
    TestAck();
    TestUpsertDelete();
    TestPersistRulesFile();
    std::cout << "ALL ALERTING TESTS PASSED\n";
    return 0;
}
