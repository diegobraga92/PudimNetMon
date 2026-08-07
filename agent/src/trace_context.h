#pragma once

#include <string>

namespace pudimagent {

// Minimal W3C Trace Context helpers (RFC 4 of the W3C trace-context spec).
// Generates traceparent headers of the form:
//   "00-<32-hex-trace-id>-<16-hex-span-id>-01"
// No external OTel SDK is required — these helpers are sufficient to carry a
// trace across gRPC metadata and Kafka headers.

// Generates a brand-new traceparent (new trace-id and span-id).
std::string GenerateTraceParent();

// Given a traceparent, returns a new traceparent that preserves the trace-id
// but uses a fresh span-id (for the next service hop).
std::string NextSpanId(const std::string &traceparent);

// Extracts the trace-id component (32 hex chars) from a traceparent, or "".
std::string TraceIdOf(const std::string &traceparent);

} // namespace pudimagent
