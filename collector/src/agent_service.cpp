#include "agent_service.h"

#include <chrono>

#include "agent_registry.h"
#include "logging.h"

namespace pudimcollector {

AgentServiceImpl::AgentServiceImpl(AgentRegistry &registry)
    : m_registry(registry) {}

grpc::Status AgentServiceImpl::SendHeartbeat(
    grpc::ServerContext *ctx, const pudimnetmon::HeartbeatRequest *request,
    pudimnetmon::HeartbeatResponse *response) {
    (void)ctx;

    // Record the heartbeat
    m_registry.RecordHeartbeat(*request);

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
