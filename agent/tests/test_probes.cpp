#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

#include "metrics.pb.h"
#include "metrics/probes.h"
#include "metrics/ntp_probe.h"
#include "dns_resolver.h"
#include "disk_buffer.h"

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

// ---- DnsResolver: time-bounded, cached lookups ----

static void TestDnsResolverLocalhost() {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto r1 = pudimagent::GlobalResolver().Lookup("localhost", "", hints, 3000);
    assert(r1.ok);
    assert(r1.addrs);
    // Second lookup should be served from the cache and still succeed.
    auto r2 = pudimagent::GlobalResolver().Lookup("localhost", "", hints, 3000);
    assert(r2.ok);
    assert(r2.addrs);
    std::cout << "PASS: DNS resolver resolves localhost (repeated lookups)\n";
}

static void TestDnsResolverInvalid() {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto r = pudimagent::GlobalResolver().Lookup("invalid.invalid.invalid", "",
                                                 hints, 3000);
    assert(!r.ok);
    assert(!r.error.empty());
    std::cout << "PASS: DNS resolver fails gracefully for invalid host: "
              << r.error << "\n";
}

static void TestDnsResolverIpLiteral() {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto r = pudimagent::GlobalResolver().Lookup("127.0.0.1", "80", hints, 3000);
    assert(r.ok);
    assert(r.addrs);
    std::cout << "PASS: DNS resolver accepts IP literals\n";
}

// ---- DiskBuffer: counters survive a restart (Phase 7 DR) ----

static void TestDiskBufferRestartCounters() {
    const std::string path = "test_disk_buffer.db";
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());

    // First session: write 3 x 300-byte batches (900 bytes total).
    {
        pudimagent::DiskBuffer db(path, 10000);
        std::string err;
        assert(db.Open(err));
        std::string blob(300, 'x');
        assert(db.Push(blob));
        assert(db.Push(blob));
        assert(db.Push(blob));
        assert(db.Size() == 3);
    }  // destructor closes the DB (rows persist)

    // Reopen with a cap (500) below the persisted 900 bytes: the counters
    // must be seeded from the DB so Push() trims the overflow instead of
    // allowing the buffer to exceed the cap.
    {
        pudimagent::DiskBuffer db(path, 500);
        std::string err;
        assert(db.Open(err));
        std::string blob(200, 'y');
        assert(db.Push(blob));
        // Trim dropped at least the 2 oldest rows (900 -> 300) and the new
        // batch fits; had the counters started at 0 the buffer would now hold
        // 4 rows instead of the trimmed 2.
        assert(db.Size() <= 3);
        assert(db.Size() >= 1);
        std::cout << "PASS: disk buffer restores counters on reopen (size="
                  << db.Size() << ")\n";
    }

    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
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
    TestDnsResolverLocalhost();
    TestDnsResolverInvalid();
    TestDnsResolverIpLiteral();
    TestDiskBufferRestartCounters();
    TestAllProbesLocalhost();
    std::cout << "ALL AGENT PROBE TESTS PASSED\n";
    return 0;
}