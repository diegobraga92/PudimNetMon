#pragma once

#include <grpcpp/grpcpp.h>

#include "diagnostic.grpc.pb.h"

namespace pudimagent {

// gRPC service the collector calls to trigger on-demand deep diagnostics on
// this agent (traceroute, packet capture summary).
class DiagnosticServiceImpl final
    : public pudimnetmon::DiagnosticService::Service {
public:
    grpc::Status RunDiagnostic(
        grpc::ServerContext *ctx,
        const pudimnetmon::DiagnosticRequest *request,
        pudimnetmon::DiagnosticResponse *response) override;
};

} // namespace pudimagent
