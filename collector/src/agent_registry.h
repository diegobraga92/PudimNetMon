#pragma once

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace pudimnetmon {
class HeartbeatRequest;
}

namespace pudimcollector {

// One agent known to the collector via its heartbeats.
struct AgentEntry {
    std::string agent_id;
    int64_t last_seen_unix_ms;
    int32_t interval_ms;
    std::string version;
    int64_t first_seen_unix_ms;
    std::string diagnostic_endpoint;  // agent diagnostic server address
};

// Registry of agents seen through heartbeats. Thread-safe.
class AgentRegistry {
public:
    AgentRegistry() = default;

    // Records a heartbeat and registers the agent on first sight.
    void RecordHeartbeat(const pudimnetmon::HeartbeatRequest &req,
                         const std::string &fallback_endpoint = "");

    // Agents that heartbeated within timeout_ms.
    size_t ActiveAgentCount(int64_t timeout_ms = 30000) const;

    // The agent's advertised diagnostic endpoint, or "" when unknown.
    std::string GetDiagnosticEndpoint(const std::string &agent_id) const;

    // True when the agent has heartbeated within timeout_ms.
    bool IsAgentAlive(const std::string &agent_id,
                      int64_t timeout_ms = 30000) const;

    bool RemoveAgent(const std::string &agent_id);

    size_t TotalAgentCount() const;

    uint64_t HeartbeatCount() const;

    // JSON snapshot of all agents for the dashboard.
    std::string DumpAgents() const;

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, AgentEntry> m_agents;
    std::atomic<uint64_t> m_heartbeat_count{0};
};

} // namespace pudimcollector
