#pragma once

#include <memory>
#include <string>

#include "diagnostic.grpc.pb.h"

namespace pudimcollector {

// TLS material used when the collector dials an agent's diagnostic server
// (mutual TLS, same cert/key the collector presents as server identity).
// Empty strings => insecure channel (no --tls-* flags configured).
struct TlsOptions {
    std::string ca;
    std::string cert;
    std::string key;

    bool mtlsEnabled() const { return !ca.empty() || !cert.empty() || !key.empty(); }
};

// Dials `endpoint` (host:port of an agent's DiagnosticService) with the
// collector's mTLS material and returns a ready-to-use stub.
std::unique_ptr<pudimnetmon::DiagnosticService::Stub> DialAgentDiagnostic(
    const std::string &endpoint, const TlsOptions &tls);

} // namespace pudimcollector
