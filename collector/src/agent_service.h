#pragma once

#include <grpcpp/grpcpp.h>

#include "heartbeat.grpc.pb.h"

namespace pudimcollector {

class AgentRegistry;

// gRPC service implementation for the heartbeat AgentService. Heartbeats are
// recorded into the shared AgentRegistry, which feeds the dashboard's agent
// liveness views and the agent-proxy HTTP endpoints.
class AgentServiceImpl final : public pudimnetmon::AgentService::Service {
public:
    explicit AgentServiceImpl(AgentRegistry &registry);

    grpc::Status SendHeartbeat(
        [[maybe_unused]] grpc::ServerContext *ctx,
        const pudimnetmon::HeartbeatRequest *request,
        pudimnetmon::HeartbeatResponse *response) override;

private:
    AgentRegistry &m_registry;
};

} // namespace pudimcollector
