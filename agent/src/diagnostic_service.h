#pragma once

#include <memory>
#include <mutex>

#include <grpcpp/grpcpp.h>

#include "diagnostic.grpc.pb.h"
#include "metrics/probes.h"

namespace pudimagent {

// gRPC service the collector calls to trigger on-demand deep diagnostics on
// this agent (traceroute, packet capture summary) and, in Phase 8, to
// reconfigure its probes at runtime.
class DiagnosticServiceImpl final
    : public pudimnetmon::DiagnosticService::Service {
public:
    explicit DiagnosticServiceImpl(std::shared_ptr<ProbeConfigStore> store)
        : m_store(std::move(store)) {}

    grpc::Status RunDiagnostic(
        grpc::ServerContext *ctx,
        const pudimnetmon::DiagnosticRequest *request,
        pudimnetmon::DiagnosticResponse *response) override;

    grpc::Status Reconfigure(
        grpc::ServerContext *ctx,
        const pudimnetmon::AgentConfigRequest *request,
        pudimnetmon::AgentConfigResponse *response) override;

    grpc::Status GetConfig(
        grpc::ServerContext *ctx,
        const pudimnetmon::GetConfigRequest *request,
        pudimnetmon::AgentConfigResponse *response) override;

    grpc::Status ListCommands(
        grpc::ServerContext *ctx,
        const pudimnetmon::ListCommandsRequest *request,
        pudimnetmon::ListCommandsResponse *response) override;

    grpc::Status RunCommand(
        grpc::ServerContext *ctx,
        const pudimnetmon::RunCommandRequest *request,
        pudimnetmon::CommandResponse *response) override;

private:
    std::shared_ptr<ProbeConfigStore> m_store;
};

} // namespace pudimagent
