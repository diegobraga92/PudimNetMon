#include "agent_endpoint.h"

namespace pudimcollector {

std::string DiagnosticEndpointFromPeer(const std::string &peer,
                                       const std::string &diagnostic_port) {
    std::string hostport = peer;

    // gRPC prefixes the peer URI with the transport scheme, e.g.
    // "ipv4:10.0.0.5:54321" or "ipv6:[fe80::1]:54321".
    const auto scheme_end = hostport.find(':');
    if (scheme_end != std::string::npos) {
        const std::string scheme = hostport.substr(0, scheme_end);
        if (scheme == "unix" || scheme == "unix-abstract") {
            return "";  // no host we can dial back
        }
        if (scheme == "ipv4" || scheme == "ipv6") {
            hostport = hostport.substr(scheme_end + 1);
        }
    }
    if (hostport.empty()) return "";

    std::string host;
    if (hostport.front() == '[') {
        // IPv6 peers are bracketed: "[fe80::1]:1234". Keep the brackets so the
        // result is a valid host:port URI.
        const auto close = hostport.find(']');
        if (close == std::string::npos) return "";
        host = hostport.substr(0, close + 1);
    } else {
        // IPv4/hostname peers are "host:port".
        const auto sep = hostport.rfind(':');
        host = sep == std::string::npos ? hostport : hostport.substr(0, sep);
    }

    // Wildcard addresses are not dialable back to a specific agent.
    if (host.empty() || host == "0.0.0.0" || host == "[::]") return "";

    return host + ":" + diagnostic_port;
}

} // namespace pudimcollector
