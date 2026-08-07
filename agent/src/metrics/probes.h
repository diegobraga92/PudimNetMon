#pragma once

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
};

// Runs all configured probes and appends metrics to out_metrics.
// Each probe failure produces a Metric with success=false + detail.
void RunAllProbes(const ProbeConfig &config,
                  std::vector<pudimnetmon::Metric> &out_metrics);

// Individual probes (mostly for unit testing).
void ProbeDns(const std::string &host, pudimnetmon::Metric &metric);
void ProbeTcp(const std::string &host_port, pudimnetmon::Metric &metric);
void ProbeTls(const std::string &host_port, pudimnetmon::Metric &metric);
void ProbeHttp(const std::string &url, pudimnetmon::Metric &metric);
void ProbeIcmp(const std::string &host, int count,
               pudimnetmon::Metric &loss_metric,
               pudimnetmon::Metric &rtt_metric,
               pudimnetmon::Metric &jitter_metric);

} // namespace pudimagent