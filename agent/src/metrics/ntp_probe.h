#pragma once

#include <string>

#include "metrics.pb.h"

namespace pudimagent {

// Sets the NTP server used by the offset probe on platforms that measure
// offset over the network (Windows SNTP client). No-op on Linux, where the
// kernel discipline offset is read via ntp_adjtime().
void SetNtpServer(const std::string &server);

// Reads the clock offset and fills a CHECK_TYPE_NTP_OFFSET metric.
// On Linux this is the kernel clock discipline offset (ntp_adjtime);
// on Windows it is the offset measured against the configured NTP server.
// `latency_ms` carries the signed offset in milliseconds (positive = local
// clock ahead).
void ProbeNtpOffset(pudimnetmon::Metric &metric);

} // namespace pudimagent