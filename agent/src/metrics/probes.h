#pragma once

#include <map>
#include <string>
#include <vector>

#include "metrics.pb.h"

namespace pudimagent {

// Configuration for a collection cycle.
struct ProbeConfig {
    // Targets for each probe type. Empty vector means skipped.
    std::vector<std::string> dns_targets;      // e.g. {"example.com"}
    std::vector<std::string> tcp_targets;      // e.g. {"10.0.0.1:443"}
    std::vector<std::string> tls_targets;      // e.g. {"example.com:443"}
    std::vector<std::string> http_targets;     // e.g. {"https://example.com"}
    std::vector<std::string> ping_targets;     // e.g. {"8.8.8.8"}

    // Number of pings per target for ICMP (used for packet loss % and
    // jitter estimation). Ignored if <= 0.
    int ping_count = 4;

    // ---- Phase 4 deep diagnostics (all default-on; disabled via CLI) ----
    bool tls_cert_check = true;        // TLS_CERTIFICATE metrics for tls_targets
    bool tcp_retransmit_check = true;  // TCP_RETRANSMIT metrics for tcp_targets
    bool tcp_handshake_capture = true; // TCP_HANDSHAKE metrics (libpcap timing)
    // HTTP protocol versions to measure for each http_target; empty = default
    // (curl's negotiated version). Entries: "http1.1", "http2", "http3".
    std::vector<std::string> http_protocols;
    // Expected DNS records keyed by host: "example.com" -> {"A:93.184.216.34"}.
    // A configured record that mismatches the resolved value marks the metric
    // success=false (alarm on mismatch).
    std::map<std::string, std::vector<std::string>> dns_expected;
};

// Runs all configured probes and appends metrics to out_metrics.
// Each probe failure produces a Metric with success=false + detail.
void RunAllProbes(const ProbeConfig &config,
                  std::vector<pudimnetmon::Metric> &out_metrics);

// Individual probes (mostly for unit testing).
void ProbeDns(const std::string &host, pudimnetmon::Metric &metric);
void ProbeDnsRecord(const std::string &host,
                    const std::vector<std::string> &expected,
                    pudimnetmon::Metric &metric);
void ProbeTcp(const std::string &host_port, pudimnetmon::Metric &metric);
void ProbeTcpRetransmit(const std::string &host_port, pudimnetmon::Metric &metric);
void ProbeTcpHandshake(const std::string &host_port, pudimnetmon::Metric &metric);
void ProbeTls(const std::string &host_port, pudimnetmon::Metric &metric);
void ProbeTlsCert(const std::string &host_port, pudimnetmon::Metric &metric);
void ProbeHttp(const std::string &url, pudimnetmon::Metric &metric);
void ProbeHttpProtocol(const std::string &url, const std::string &protocol,
                       pudimnetmon::Metric &metric);
void ProbeIcmp(const std::string &host, int count,
               pudimnetmon::Metric &loss_metric,
               pudimnetmon::Metric &rtt_metric,
               pudimnetmon::Metric &jitter_metric);

} // namespace pudimagent