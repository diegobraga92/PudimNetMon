#include <iostream>
#include <string>

#include "platform/platform.h"

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &label) {
    if (cond) {
        std::cout << "PASS " << label << "\n";
    } else {
        std::cerr << "FAIL " << label << "\n";
        ++g_failures;
    }
}

} // namespace

int main() {
    using pudimagent::platform::AdvertisedDiagnosticEndpoint;

    Check(AdvertisedDiagnosticEndpoint("127.0.0.1:50051", "50052") ==
              "127.0.0.1:50052",
          "ipv4 loopback collector endpoint");
    const std::string v6_ep =
        AdvertisedDiagnosticEndpoint("[::1]:50051", "50052");
    Check(v6_ep.empty() || v6_ep == "[::1]:50052",
          "ipv6 loopback endpoint (empty when the host has no IPv6 route)");
    Check(AdvertisedDiagnosticEndpoint("127.0.0.1", "50052") ==
              "127.0.0.1:50052",
          "collector endpoint without port");
    const std::string host_ep =
        AdvertisedDiagnosticEndpoint("localhost:50051", "51000");
    Check(host_ep == "127.0.0.1:51000" || host_ep == "[::1]:51000",
          "hostname collector endpoint resolves to a loopback address");

    // Unresolvable collectors and missing inputs must not invent an endpoint.
    Check(AdvertisedDiagnosticEndpoint("no.such.host.invalid:50051", "50052")
              .empty(),
          "unresolvable collector endpoint yields no advertisement");
    Check(AdvertisedDiagnosticEndpoint("", "50052").empty(),
          "empty collector endpoint");
    Check(AdvertisedDiagnosticEndpoint("127.0.0.1:50051", "").empty(),
          "empty diagnostic port");

    if (g_failures > 0) {
        std::cerr << g_failures << " platform test(s) failed\n";
        return 1;
    }
    std::cout << "All platform tests passed\n";
    return 0;
}