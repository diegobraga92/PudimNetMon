#pragma once

#include <memory>
#include <string>

#include "diagnostic.grpc.pb.h"

namespace pudimcollector {

// TLS material used when the collector dials an agent's diagnostic server.
// All-empty strings mean an insecure channel.
struct TlsOptions {
    std::string ca;
    std::string cert;
    std::string key;

    bool mtlsEnabled() const { return !ca.empty() || !cert.empty() || !key.empty(); }
};

// Dials an agent's diagnostic server and returns a ready-to-use stub.
std::unique_ptr<pudimnetmon::DiagnosticService::Stub> DialAgentDiagnostic(
    const std::string &endpoint, const TlsOptions &tls);

} // namespace pudimcollector
