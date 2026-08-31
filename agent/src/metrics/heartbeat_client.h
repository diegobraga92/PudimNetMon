#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "heartbeat.grpc.pb.h"

namespace pudimagent {

// gRPC client for the agent->collector heartbeat RPC. Mirrors MetricsClient in
// shape: constructed against a collector endpoint (optionally with mTLS channel
// credentials); each SendHeartbeat() returns success/failure so the caller can
// drive endpoint failover.
class HeartbeatClient {
public:
    HeartbeatClient(const std::string &endpoint,
                    std::shared_ptr<grpc::ChannelCredentials> creds = nullptr,
                    std::string node_id = "",
                    std::string diagnostic_endpoint = "");

    // Sends one heartbeat. Returns true if the collector ACKed.
    bool SendHeartbeat(int interval_ms, const std::string &version,
                       const std::string &traceparent = "");

private:
    std::unique_ptr<pudimnetmon::AgentService::Stub> m_stub;
    std::string m_node_id;
    std::string m_diagnostic_endpoint;
};

} // namespace pudimagent
