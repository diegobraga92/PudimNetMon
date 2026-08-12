#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "metrics.pb.h"
#include "metrics/probes.h"
#include "metrics/ntp_probe.h"

using pudimnetmon::CheckType;
using pudimnetmon::Metric;
using pudimagent::ProbeConfig;
using pudimagent::RunAllProbes;

static void TestEmptyConfig() {
    ProbeConfig cfg; // No targets
    cfg.ntp_check = false;
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
    // `detail` is a proto3 plain string field (no has_detail()); an unset
    // string reads back as empty.
    assert(!m.detail().empty());
    std::cout << "PASS: DNS invalid host failed gracefully: "
              << m.detail() << "\n";
}

static void TestTcpLocalhost() {
    Metric m;
    pudimagent::ProbeTcp("localhost:8080", m);
    // localhost:8080 may or may not be listening; just verify we get a
    // metric with the right check_type and either success or failure detail.
    assert(m.check_type() == CheckType::CHECK_TYPE_TCP_CONNECT);
    assert(m.success() || !m.detail().empty());
    std::cout << "PASS: TCP localhost probe produced metric "
              << (m.success() ? "(success)" : "(failure: " + m.detail() + ")") << "\n";
}

// ---- Phase 4 deep-diagnostic probes ----

static void TestDnsRecordLocalhost() {
    Metric m;
    pudimagent::ProbeDnsRecord("localhost", {}, m);
    assert(m.check_type() == CheckType::CHECK_TYPE_DNS_RECORD);
    assert(m.success());
    assert(m.attributes().count("A") == 1);  // 127.0.0.1
    std::cout << "PASS: DNS record lookup found A="
              << m.attributes().at("A") << "\n";
}

static void TestDnsRecordMismatch() {
    Metric m;
    // Expect an impossible A record → success=false (alarm on mismatch).
    pudimagent::ProbeDnsRecord("localhost", {"A:203.0.113.9"}, m);
    assert(m.check_type() == CheckType::CHECK_TYPE_DNS_RECORD);
    assert(!m.success());
    assert(!m.detail().empty());
    std::cout << "PASS: DNS record mismatch flagged: " << m.detail() << "\n";
}

static void TestTcpRetransmitLocalhost() {
    Metric m;
    pudimagent::ProbeTcpRetransmit("localhost:8080", m);
    assert(m.check_type() == CheckType::CHECK_TYPE_TCP_RETRANSMIT);
    // Success if the socket connected (may fail if nothing listens on 8080).
    assert(m.success() || !m.detail().empty());
    std::cout << "PASS: TCP retransmission probe produced metric "
              << (m.success() ? "(status_code=" + std::to_string(m.status_code()) + ")"
                              : "(failure: " + m.detail() + ")") << "\n";
}

static void TestTlsCertMetric() {
    Metric m;
    pudimagent::ProbeTlsCert("localhost:443", m);
    assert(m.check_type() == CheckType::CHECK_TYPE_TLS_CERTIFICATE);
    // localhost:443 likely has no TLS server; verify a metric is produced
    // either way (success with cert attributes, or failure with detail).
    assert(m.success() || !m.detail().empty());
    std::cout << "PASS: TLS cert probe produced metric "
              << (m.success() ? "(expiry_days=" +
                                    m.attributes().at("tls_cert_expiry_days") + ")"
                              : "(failure: " + m.detail() + ")") << "\n";
}

static void TestHttpProtocolHttp11() {
    Metric m;
    pudimagent::ProbeHttpProtocol("http://localhost:8080", "http1.1", m);
    assert(m.check_type() == CheckType::CHECK_TYPE_HTTP_REQUEST);
    // The protocol is appended to the target for dashboard grouping.
    assert(m.target().find(";http1.1") != std::string::npos);
    std::cout << "PASS: HTTP/1.1 probe produced target '" << m.target() << "'\n";
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

    // ntp + dns(+record) + tcp(+retransmit+handshake) + tls(+cert) + http = 9
    assert(metrics.size() >= 9);
    std::cout << "PASS: RunAllProbes produced " << metrics.size()
              << " metrics\n";
}

static void TestNtpOffset() {
    Metric m;
    pudimagent::ProbeNtpOffset(m);
    assert(m.check_type() == CheckType::CHECK_TYPE_NTP_OFFSET);
    // Linux reads the kernel discipline offset; Windows measures it over the
    // network, which can legitimately fail in offline CI environments.
    assert(m.success() || !m.detail().empty());
    std::cout << "PASS: NTP offset probe produced offset="
              << (m.success() ? std::to_string(m.latency_ms()) + " ms"
                              : "failure (" + m.detail() + ")")
              << "\n";
}

int main() {
    TestEmptyConfig();
    TestDnsLocalhost();
    TestDnsInvalid();
    TestTcpLocalhost();
    TestDnsRecordLocalhost();
    TestDnsRecordMismatch();
    TestTcpRetransmitLocalhost();
    TestTlsCertMetric();
    TestHttpProtocolHttp11();
    TestNtpOffset();
    TestAllProbesLocalhost();
    std::cout << "ALL AGENT PROBE TESTS PASSED\n";
    return 0;
}