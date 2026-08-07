// Unit tests for the alerting state machine (no database required).
#include <cassert>
#include <iostream>
#include <string>

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

    // OK -> FIRING
    *batch.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_TCP_CONNECT, "example.com:443", 600.0);
    mgr.Evaluate(batch.agent_id(), batch.metrics());
    assert(mgr.ActiveAlertCount() == 1);
    assert(mgr.TotalAlertsFired() == 1);
    std::cout << "PASS: violation fired alert\n";

    // Still firing with repeat_interval_sec=0 -> repeat notification.
    batch.clear_metrics();
    *batch.add_metrics() = LatencyMetric(CheckType::CHECK_TYPE_TCP_CONNECT, "example.com:443", 700.0);
    mgr.Evaluate(batch.agent_id(), batch.metrics());
    assert(mgr.ActiveAlertCount() == 1);
    assert(mgr.TotalAlertsFired() == 2);
    std::cout << "PASS: repeat notification sent while still firing\n";

    // Back within bounds -> FIRING -> RESOLVED.
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

// Phase 8: acknowledging a firing alert is reflected in ActiveAlertsJson.
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

    // Ack a non-existent alert → no-op.
    assert(!mgr.Ack("nope", "agent-a", "1.1.1.1"));

    // Ack the real one.
    assert(mgr.Ack("high-loss", "agent-a", "1.1.1.1"));
    assert(mgr.ActiveAlertsJson().find("\"acknowledged\":true") != std::string::npos);

    // Acking twice is a no-op.
    assert(!mgr.Ack("high-loss", "agent-a", "1.1.1.1"));
    std::cout << "PASS: alert ack marks firing alert acknowledged\n";
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
    std::cout << "ALL ALERTING TESTS PASSED\n";
    return 0;
}
