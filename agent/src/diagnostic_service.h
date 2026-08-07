#pragma once

#include <memory>
#include <mutex>

#include <grpcpp/grpcpp.h>

#include "diagnostic.grpc.pb.h"
#include "metrics/probes.h"

namespace pudimagent {

// Runtime probe configuration store (Phase 8). The metric loop copies the
// current config each cycle; the collector's Reconfigure RPC swaps in a new
// one so checks can be added/edited without restarting the agent.
struct ProbeConfigStore {
    mutable std::mutex mu;
    ProbeConfig cfg;

    void Set(const ProbeConfig &c) {
        std::lock_guard<std::mutex> lock(mu);
        cfg = c;
    }
    ProbeConfig Get() const {
        std::lock_guard<std::mutex> lock(mu);
        return cfg;
    }
};

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

private:
    std::shared_ptr<ProbeConfigStore> m_store;
};

} // namespace pudimagent
