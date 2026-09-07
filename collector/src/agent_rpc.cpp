#include "agent_rpc.h"

#include "tls_credentials.h"

namespace pudimcollector {

std::unique_ptr<pudimnetmon::DiagnosticService::Stub> DialAgentDiagnostic(
    const std::string &endpoint, const TlsOptions &tls) {
    auto channel = grpc::CreateChannel(
        endpoint, pudimagent::MakeChannelCredentials(tls.ca, tls.cert, tls.key));
    return pudimnetmon::DiagnosticService::NewStub(channel);
}

} // namespace pudimcollector
