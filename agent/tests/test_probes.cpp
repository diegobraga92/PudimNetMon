#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "metrics.pb.h"
#include "metrics/probes.h"

using pudimnetmon::CheckType;
using pudimnetmon::Metric;
using pudimagent::ProbeConfig;
using pudimagent::RunAllProbes;

static void TestEmptyConfig() {
    ProbeConfig cfg; // No targets
    std::vector<Metric> metrics;
    RunAllProbes(cfg, metrics);
    assert(metrics.empty());
    std::cout << "PASS: empty config produces no metrics\n";
}

static void TestDnsLocalhost() {
    Metric m;
    pudimagent::ProbeDns("localhost", m);
    assert(m.check_type() == CheckType::CHECK_TYPE_DNS_RESOLUTION);
    assert(m.success());
    assert(m.has_latency_ms());
    assert(m.latency_ms() >= 0);
    std::cout << "PASS: DNS localhost resolved successfully ("
              << m.latency_ms() << " ms)\n";
}

static void TestDnsInvalid() {
    Metric m;
    pudimagent::ProbeDns("invalid.invalid.invalid", m);
    assert(m.check_type() == CheckType::CHECK_TYPE_DNS_RESOLUTION);
    assert(!m.success());
    assert(m.has_detail());
    std::cout << "PASS: DNS invalid host failed gracefully: "
              << m.detail() << "\n";
}

static void TestTcpLocalhost() {
    Metric m;
    pudimagent::ProbeTcp("localhost:8080", m);
    // localhost:8080 may or may not be listening; just verify we get a
    // metric with the right check_type and either success or failure detail.
    assert(m.check_type() == CheckType::CHECK_TYPE_TCP_CONNECT);
    assert(m.success() || m.has_detail());
    std::cout << "PASS: TCP localhost probe produced metric "
              << (m.success() ? "(success)" : "(failure: " + m.detail() + ")") << "\n";
}

static void TestAllProbesLocalhost() {
    ProbeConfig cfg;
    cfg.dns_targets = {"localhost"};
    cfg.tcp_targets = {"localhost:8080"};
    cfg.tls_targets = {"localhost:443"};
    cfg.http_targets = {"http://localhost:8080"};
    // No ping targets (requires CAP_NET_RAW, may fail in CI containers)

    std::vector<Metric> metrics;
    RunAllProbes(cfg, metrics);

    // Expect at least 4 metrics (dns + tcp + tls + http)
    assert(metrics.size() >= 4);
    std::cout << "PASS: RunAllProbes produced " << metrics.size()
              << " metrics\n";
}

int main() {
    TestEmptyConfig();
    TestDnsLocalhost();
    TestDnsInvalid();
    TestTcpLocalhost();
    TestAllProbesLocalhost();
    std::cout << "ALL AGENT PROBE TESTS PASSED\n";
    return 0;
}