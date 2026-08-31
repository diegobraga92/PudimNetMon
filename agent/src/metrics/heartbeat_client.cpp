#include "heartbeat_client.h"

#include <chrono>
#include <utility>

#include "logger.h"

using grpc::ClientContext;
using grpc::Status;
using pudimnetmon::AgentService;
using pudimnetmon::HeartbeatRequest;
using pudimnetmon::HeartbeatResponse;

namespace pudimagent {

HeartbeatClient::HeartbeatClient(
    const std::string &endpoint,
    std::shared_ptr<grpc::ChannelCredentials> creds,
    std::string node_id,
    std::string diagnostic_endpoint)
    : m_stub(AgentService::NewStub(grpc::CreateChannel(
          endpoint,
          creds ? creds : grpc::InsecureChannelCredentials()))),
      m_node_id(std::move(node_id)),
      m_diagnostic_endpoint(std::move(diagnostic_endpoint)) {
    LOG_INFO("gRPC channel created to " + endpoint);
}

bool HeartbeatClient::SendHeartbeat(int interval_ms, const std::string &version,
                                    const std::string &traceparent) {
    HeartbeatRequest req;
    req.set_agent_id(m_node_id);
    req.set_timestamp_unix_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    req.set_interval_ms(interval_ms);
    req.set_version(version);
    if (!m_diagnostic_endpoint.empty()) {
        req.set_diagnostic_endpoint(m_diagnostic_endpoint);
    }

    HeartbeatResponse resp;
    ClientContext ctx;
    if (!traceparent.empty()) {
        ctx.AddMetadata("traceparent", traceparent);
    }

    // Set a deadline for the RPC
    auto deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(10);
    ctx.set_deadline(deadline);

    Status status = m_stub->SendHeartbeat(&ctx, req, &resp);

    if (status.ok()) {
        LOG_INFO("Heartbeat ACK received from collector (ack=" +
                 std::to_string(resp.ack()) + ")");
        return true;
    }
    LOG_WARN("Heartbeat failed: " + status.error_message() +
             " (code=" + std::to_string(status.error_code()) + ")");
    return false;
}

} // namespace pudimagent
