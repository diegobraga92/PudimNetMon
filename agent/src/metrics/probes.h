#pragma once

#include <string>
#include <vector>

#include "metrics.pb.h"
#include "probe_config.h"

namespace pudimagent {

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
void ProbeIcmp(const std::string &host, int count, int gap_ms,
               pudimnetmon::Metric &loss_metric,
               pudimnetmon::Metric &rtt_metric,
               pudimnetmon::Metric &jitter_metric);

} // namespace pudimagent