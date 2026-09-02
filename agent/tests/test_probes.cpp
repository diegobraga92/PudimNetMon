// Unit tests for the agent's probes.
//
// The checks below are always-on rather than assert() because the Windows CI
// builds Release binaries (/DNDEBUG), which compiles assert() out entirely:
// assert-only guards made crash-prone .at() calls reachable (protobuf Map::at
// CHECK-fails hard, exit code 0xc0000409, when the key is missing).
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
#include <windows.h>
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

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &name) {
    if (cond) {
        std::cout << "PASS " << name << "\n";
    } else {
        std::cerr << "FAIL " << name << "\n";
        ++g_failures;
    }
}

// Comma-separated dump of a metric's attributes for failure diagnostics.
// Iterates instead of using at() so a missing key cannot abort the process.
std::string DumpAttributes(const Metric &m) {
    std::string out;
    for (const auto &kv : m.attributes()) {
        if (!out.empty()) out += ", ";
        out += kv.first + "=" + kv.second;
    }
    return out;
}

}  // namespace

static void TestEmptyConfig() {
    ProbeConfig cfg;  // No targets
    cfg.ntp_check = false;
    std::vector<Metric> metrics;
    RunAllProbes(cfg, metrics);
    Check(metrics.empty(), "empty config produces no metrics");
}

static void TestDnsLocalhost() {
    Metric m;
    pudimagent::ProbeDns("localhost", m);
    Check(m.check_type() == CheckType::CHECK_TYPE_DNS_RESOLUTION &&
              m.success() && m.has_latency_ms() && m.latency_ms() >= 0,
          "DNS localhost resolved successfully (" +
              std::to_string(m.latency_ms()) + " ms)");
}

static void TestDnsInvalid() {
    Metric m;
    pudimagent::ProbeDns("invalid.invalid.invalid", m);
    Check(m.check_type() == CheckType::CHECK_TYPE_DNS_RESOLUTION &&
              !m.success() && !m.detail().empty(),
          "DNS invalid host failed gracefully: " + m.detail());
}

static void TestTcpLocalhost() {
    Metric m;
    pudimagent::ProbeTcp("localhost:8080", m);
    // localhost:8080 may or may not be listening; just verify we get a
    // metric with the right check_type and either success or failure detail.
    Check(m.check_type() == CheckType::CHECK_TYPE_TCP_CONNECT &&
              (m.success() || !m.detail().empty()),
          "TCP localhost probe produced metric " +
              (m.success() ? "(success)"
                           : "(failure: " + m.detail() + ")"));
}

// Deep-diagnostic probes.

static void TestDnsRecordLocalhost() {
    Metric m;
    pudimagent::ProbeDnsRecord("localhost", {}, m);
    const auto &attrs = m.attributes();
    bool has_a = attrs.count("A") == 1;
    bool has_aaaa = attrs.count("AAAA") == 1;
    // Accept whichever address-family record the resolver produced. Hosts may
    // resolve "localhost" to 127.0.0.1, ::1, or both; requiring IPv4
    // specifically is not portable across CI network stacks.
    Check(m.check_type() == CheckType::CHECK_TYPE_DNS_RECORD && m.success() &&
              (has_a || has_aaaa),
          "DNS record lookup found an address record [" +
              DumpAttributes(m) + "]");
}

static void TestDnsRecordMismatch() {
    Metric m;
    // Expect an impossible A record → success=false (alarm on mismatch). The
    // probe flags the mismatch whether localhost resolved to A, AAAA, or no
    // records at all (reported as "got none").
    pudimagent::ProbeDnsRecord("localhost", {"A:203.0.113.9"}, m);
    Check(m.check_type() == CheckType::CHECK_TYPE_DNS_RECORD && !m.success() &&
              !m.detail().empty(),
          "DNS record mismatch flagged: " + m.detail());
}

static void TestTcpRetransmitLocalhost() {
    Metric m;
    pudimagent::ProbeTcpRetransmit("localhost:8080", m);
    // Success if the socket connected (may fail if nothing listens on 8080).
    Check(m.check_type() == CheckType::CHECK_TYPE_TCP_RETRANSMIT &&
              (m.success() || !m.detail().empty()),
          "TCP retransmission probe produced metric " +
              (m.success()
                   ? "(status_code=" + std::to_string(m.status_code()) + ")"
                   : "(failure: " + m.detail() + ")"));
}

static void TestTlsCertMetric() {
    Metric m;
    pudimagent::ProbeTlsCert("localhost:443", m);
    // localhost:443 likely has no TLS server; verify a metric is produced
    // either way (success with cert attributes, or failure with detail).
    bool ok = m.check_type() == CheckType::CHECK_TYPE_TLS_CERTIFICATE;
    if (m.success()) {
        ok = ok && m.attributes().count("tls_cert_expiry_days") == 1;
    } else {
        ok = ok && !m.detail().empty();
    }
    Check(ok, "TLS cert probe produced metric [" + DumpAttributes(m) + "]");
}

static void TestHttpProtocolHttp11() {
    Metric m;
    pudimagent::ProbeHttpProtocol("http://localhost:8080", "http1.1", m);
    // The protocol is appended to the target for dashboard grouping.
    Check(m.check_type() == CheckType::CHECK_TYPE_HTTP_REQUEST &&
              m.target().find(";http1.1") != std::string::npos,
          "HTTP/1.1 probe produced target '" + m.target() + "'");
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
    Check(metrics.size() >= 9, "RunAllProbes produced " +
                                   std::to_string(metrics.size()) + " metrics");
}

static void TestNtpOffset() {
    Metric m;
    pudimagent::ProbeNtpOffset(m);
    // Linux reads the kernel discipline offset; Windows measures it over the
    // network, which can legitimately fail in offline CI environments.
    Check(m.check_type() == CheckType::CHECK_TYPE_NTP_OFFSET &&
              (m.success() || !m.detail().empty()),
          "NTP offset probe produced " +
              (m.success() ? std::to_string(m.latency_ms()) + " ms"
                           : "failure (" + m.detail() + ")"));
}

// DnsResolver: time-bounded, cached lookups.

static void TestDnsResolverLocalhost() {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto r1 = pudimagent::GlobalResolver().Lookup("localhost", "", hints, 3000);
    // Second lookup should be served from the cache and still succeed.
    auto r2 = pudimagent::GlobalResolver().Lookup("localhost", "", hints, 3000);
    Check(r1.ok && r1.addrs && r2.ok && r2.addrs,
          "DNS resolver resolves localhost (repeated lookups)");
}

static void TestDnsResolverInvalid() {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto r = pudimagent::GlobalResolver().Lookup("invalid.invalid.invalid", "",
                                                 hints, 3000);
    Check(!r.ok && !r.error.empty(),
          "DNS resolver fails gracefully for invalid host: " + r.error);
}

static void TestDnsResolverIpLiteral() {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto r = pudimagent::GlobalResolver().Lookup("127.0.0.1", "80", hints, 3000);
    Check(r.ok && r.addrs, "DNS resolver accepts IP literals");
}

// DiskBuffer: counters survive a restart.

static void TestDiskBufferRestartCounters() {
    const std::string path = "test_disk_buffer.db";
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());

    // First session: write 3 x 300-byte batches (900 bytes total).
    {
        pudimagent::DiskBuffer db(path, 10000);
        std::string err;
        Check(db.Open(err), "disk buffer opens in session 1");
        std::string blob(300, 'x');
        Check(db.Push(blob) && db.Push(blob) && db.Push(blob),
              "disk buffer stores 3 batches in session 1");
        Check(db.Size() == 3, "disk buffer reports 3 pending rows");
    }  // destructor closes the DB (rows persist)

    // Reopen with a cap (500) below the persisted 900 bytes: the counters
    // must be seeded from the DB so Push() trims the overflow instead of
    // allowing the buffer to exceed the cap.
    {
        pudimagent::DiskBuffer db(path, 500);
        std::string err;
        Check(db.Open(err), "disk buffer reopens in session 2");
        std::string blob(200, 'y');
        Check(db.Push(blob), "disk buffer push after reopen");
        // Trim dropped at least the 2 oldest rows (900 -> 300) and the new
        // batch fits; had the counters started at 0 the buffer would now hold
        // 4 rows instead of the trimmed 2.
        Check(db.Size() <= 3 && db.Size() >= 1,
              "disk buffer restores counters on reopen (size=" +
                  std::to_string(db.Size()) + ")");
    }

    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

int main() {
#ifdef _WIN32
    // Winsock must be initialised before any socket/getaddrinfo call. The real
    // agent does this through platform::InitNetwork(); the test binary must do
    // the same or every probe fails with WSANOTINITIALISED (10093) and no DNS
    // records are collected.
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

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

    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ALL AGENT PROBE TESTS PASSED\n";
    return 0;
}
