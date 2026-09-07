#pragma once

#include <grpcpp/grpcpp.h>

#include "heartbeat.grpc.pb.h"

namespace pudimcollector {

class AgentRegistry;

// Heartbeat gRPC service. Records heartbeats into the shared AgentRegistry.
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
