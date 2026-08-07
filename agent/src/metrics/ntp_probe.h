#pragma once

#include <string>

#include "metrics.pb.h"

namespace pudimagent {

// Reads the kernel clock discipline offset via ntp_adjtime() and fills a
// CHECK_TYPE_NTP_OFFSET metric. `latency_ms` carries the signed offset in
// milliseconds (positive = local clock ahead). Requires no extra capability.
void ProbeNtpOffset(pudimnetmon::Metric &metric);

} // namespace pudimagent