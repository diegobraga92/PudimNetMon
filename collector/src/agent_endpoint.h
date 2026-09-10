#pragma once

#include <string>

namespace pudimcollector {

// Default port the agent's diagnostic gRPC server listens on.
inline constexpr char kDefaultAgentDiagnosticPort[] = "50052";

// Derives a dialable "host:port" diagnostic endpoint from a gRPC peer URI such
// as "ipv4:10.0.0.5:54321" or "ipv6:[fe80::1]:54321".
std::string DiagnosticEndpointFromPeer(const std::string &peer,
                                       const std::string &diagnostic_port);

} // namespace pudimcollector
