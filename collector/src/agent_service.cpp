#include "agent_service.h"

#include <chrono>

#include "agent_endpoint.h"
#include "agent_registry.h"
#include "logging.h"

namespace pudimcollector {

AgentServiceImpl::AgentServiceImpl(AgentRegistry &registry)
    : m_registry(registry) {}

grpc::Status AgentServiceImpl::SendHeartbeat(
    grpc::ServerContext *ctx, const pudimnetmon::HeartbeatRequest *request,
    pudimnetmon::HeartbeatResponse *response) {
    // Fall back to the address the heartbeat arrived from.
    std::string fallback_endpoint;
    if (request->diagnostic_endpoint().empty() && ctx != nullptr) {
        fallback_endpoint = DiagnosticEndpointFromPeer(
            ctx->peer(), kDefaultAgentDiagnosticPort);
    }

    // Record the heartbeat
    m_registry.RecordHeartbeat(*request, fallback_endpoint);

    logger::emit("info", "Heartbeat received from " + request->agent_id(),
                 request->agent_id());

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

    response->set_ack(true);
    response->set_collector_time_unix_ms(now);
    response->set_status_message("ok");

    return grpc::Status::OK;
}

} // namespace pudimcollector
