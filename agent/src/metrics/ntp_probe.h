#pragma once

#include <string>

#include "metrics.pb.h"

namespace pudimagent {

void SetNtpServer(const std::string &server);
void ProbeNtpOffset(pudimnetmon::Metric &metric);

} // namespace pudimagent