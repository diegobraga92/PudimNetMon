// Unit tests for deriving a dialable diagnostic endpoint from a gRPC peer URI.
// No network, database or gRPC server is required.
#include <cstdlib>
#include <iostream>
#include <string>

#include "agent_endpoint.h"

namespace {

using pudimcollector::DiagnosticEndpointFromPeer;

int g_failures = 0;

void ExpectEq(const std::string &got, const std::string &want,
              const std::string &label) {
    if (got != want) {
        std::cerr << "FAIL " << label << ": got '" << got << "', want '" << want
                  << "'\n";
        ++g_failures;
    }
}

} // namespace

int main() {
    ExpectEq(DiagnosticEndpointFromPeer("ipv4:10.0.0.5:54321", "50052"),
             "10.0.0.5:50052", "ipv4 peer");
    ExpectEq(DiagnosticEndpointFromPeer("10.0.0.5:54321", "50052"),
             "10.0.0.5:50052", "ipv4 peer without scheme");
    ExpectEq(DiagnosticEndpointFromPeer("ipv6:[fe80::1]:54321", "50052"), "",
             "link-local ipv6 peer is not dialable");
    ExpectEq(DiagnosticEndpointFromPeer("ipv6:[FE80::A]:54321", "50052"), "",
             "link-local ipv6 peer (uppercase) is not dialable");
    ExpectEq(DiagnosticEndpointFromPeer("ipv6:[2001:db8::5]:54321", "50052"),
             "[2001:db8::5]:50052", "routable ipv6 peer");
    ExpectEq(DiagnosticEndpointFromPeer("ipv6:[::ffff:10.0.0.5]:54321", "50052"),
             "10.0.0.5:50052", "ipv4-mapped ipv6 peer becomes ipv4");
    ExpectEq(DiagnosticEndpointFromPeer("ipv4:172.18.0.1:4321", "50052"),
             "172.18.0.1:50052", "docker gateway peer");
    ExpectEq(DiagnosticEndpointFromPeer("ipv4:10.0.0.5:54321", "51000"),
             "10.0.0.5:51000", "custom diagnostic port");
    ExpectEq(DiagnosticEndpointFromPeer("unix:/tmp/agent.sock", "50052"), "",
             "unix peer has no host");
    ExpectEq(DiagnosticEndpointFromPeer("ipv4:0.0.0.0:1", "50052"), "",
             "wildcard peer is not dialable");
    ExpectEq(DiagnosticEndpointFromPeer("", "50052"), "", "empty peer");

    if (g_failures > 0) {
        std::cerr << g_failures << " agent-peer test(s) failed\n";
        return 1;
    }
    std::cout << "All agent-peer tests passed\n";
    return 0;
}
