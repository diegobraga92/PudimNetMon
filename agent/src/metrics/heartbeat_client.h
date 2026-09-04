#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "heartbeat.grpc.pb.h"

namespace pudimagent {

class HeartbeatClient {
public:
    HeartbeatClient(const std::string &endpoint,
                    std::shared_ptr<grpc::ChannelCredentials> creds = nullptr,
                    std::string node_id = "",
                    std::string diagnostic_endpoint = "");

    bool SendHeartbeat(int interval_ms, const std::string &version,
                       const std::string &traceparent = "");

private:
    std::unique_ptr<pudimnetmon::AgentService::Stub> m_stub;
    std::string m_node_id;
    std::string m_diagnostic_endpoint;
};

} // namespace pudimagent
