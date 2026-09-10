#include "agent_registry.h"

#include <chrono>

#include "heartbeat.pb.h"
#include "logging.h"

namespace pudimcollector {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

void AgentRegistry::RecordHeartbeat(const pudimnetmon::HeartbeatRequest &req,
                                    const std::string &fallback_endpoint) {
    std::unique_lock lock(m_mutex);
    auto now = NowMs();
    // Prefer the endpoint the agent advertises
    const std::string endpoint = req.diagnostic_endpoint().empty()
                                     ? fallback_endpoint
                                     : req.diagnostic_endpoint();
    auto it = m_agents.find(req.agent_id());
    if (it == m_agents.end()) {
        AgentEntry entry;
        entry.agent_id = req.agent_id();
        entry.last_seen_unix_ms = now;
        entry.interval_ms = req.interval_ms();
        entry.version = req.version();
        entry.first_seen_unix_ms = now;
        entry.diagnostic_endpoint = endpoint;
        m_agents[req.agent_id()] = entry;
        logger::emit("info", "New agent registered", req.agent_id());
    } else {
        it->second.last_seen_unix_ms = now;
        it->second.interval_ms = req.interval_ms();
        it->second.version = req.version();
        it->second.diagnostic_endpoint = endpoint;
    }
    m_heartbeat_count++;
}

size_t AgentRegistry::ActiveAgentCount(int64_t timeout_ms) const {
    std::shared_lock lock(m_mutex);
    auto now = NowMs();
    size_t count = 0;
    for (const auto &[id, entry] : m_agents) {
        (void)id;
        if ((now - entry.last_seen_unix_ms) < timeout_ms) {
            count++;
        }
    }
    return count;
}

std::string AgentRegistry::GetDiagnosticEndpoint(const std::string &agent_id) const {
    std::shared_lock lock(m_mutex);
    auto it = m_agents.find(agent_id);
    return (it != m_agents.end()) ? it->second.diagnostic_endpoint : "";
}

bool AgentRegistry::IsAgentAlive(const std::string &agent_id,
                                 int64_t timeout_ms) const {
    std::shared_lock lock(m_mutex);
    auto it = m_agents.find(agent_id);
    return (it != m_agents.end()) &&
           (NowMs() - it->second.last_seen_unix_ms) < timeout_ms;
}

size_t AgentRegistry::TotalAgentCount() const {
    std::shared_lock lock(m_mutex);
    return m_agents.size();
}

bool AgentRegistry::RemoveAgent(const std::string &agent_id) {
    std::unique_lock lock(m_mutex);
    auto it = m_agents.find(agent_id);
    if (it == m_agents.end()) return false;
    m_agents.erase(it);
    logger::emit("info", "Agent removed from registry", agent_id);
    return true;
}

size_t AgentRegistry::ExpireStale(int64_t max_age_ms) {
    if (max_age_ms <= 0) return 0;
    std::unique_lock lock(m_mutex);
    const int64_t cutoff = NowMs() - max_age_ms;
    size_t removed = 0;
    for (auto it = m_agents.begin(); it != m_agents.end();) {
        if (it->second.last_seen_unix_ms < cutoff) {
            logger::emit("info", "Agent expired from registry", it->first);
            it = m_agents.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

uint64_t AgentRegistry::HeartbeatCount() const {
    std::shared_lock lock(m_mutex);
    return m_heartbeat_count;
}

std::string AgentRegistry::DumpAgents() const {
    std::shared_lock lock(m_mutex);
    auto now = NowMs();
    std::string json = "{\"agents\":[";
    bool first = true;
    for (const auto &[id, entry] : m_agents) {
        (void)id;
        if (!first) json += ",";
        first = false;
        bool alive = (now - entry.last_seen_unix_ms) < 30000;
        json += "{";
        json += "\"agent_id\":\"" + logger::escape(entry.agent_id) + "\",";
        json += "\"last_seen_unix_ms\":" + std::to_string(entry.last_seen_unix_ms) + ",";
        json += "\"interval_ms\":" + std::to_string(entry.interval_ms) + ",";
        json += "\"version\":\"" + logger::escape(entry.version) + "\",";
        json += "\"diagnostic_endpoint\":\"" + logger::escape(entry.diagnostic_endpoint) + "\",";
        json += "\"first_seen_unix_ms\":" + std::to_string(entry.first_seen_unix_ms) + ",";
        json += "\"alive\":" + std::string(alive ? "true" : "false");
        json += "}";
    }
    json += "]}";
    return json;
}

} // namespace pudimcollector
