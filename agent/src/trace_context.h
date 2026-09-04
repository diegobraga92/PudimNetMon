#pragma once

#include <string>

namespace pudimagent {

std::string GenerateTraceParent();

std::string NextSpanId(const std::string &traceparent);

std::string TraceIdOf(const std::string &traceparent);

} // namespace pudimagent
