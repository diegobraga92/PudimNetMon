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
    std::string diagnostic_endpoint;  // host:port of the agent's diagnostic server
};

// Live registry of agents that have sent heartbeats. Heartbeats arrive on
// gRPC server threads while the dashboard reads this from the HTTP thread
// pool, so every accessor is synchronized.
class AgentRegistry {
public:
    AgentRegistry() = default;

    // Records a heartbeat, registering the agent on first sight and bumping
    // the heartbeat counter.
    void RecordHeartbeat(const pudimnetmon::HeartbeatRequest &req);

    // Agents whose most recent heartbeat is within `timeout_ms`.
    size_t ActiveAgentCount(int64_t timeout_ms = 30000) const;

    // The agent's advertised diagnostic endpoint, or "" when unknown.
    std::string GetDiagnosticEndpoint(const std::string &agent_id) const;

    size_t TotalAgentCount() const;

    uint64_t HeartbeatCount() const;

    // JSON snapshot for the dashboard:
    //   {"agents":[{agent_id, last_seen_unix_ms, interval_ms, version,
    //               diagnostic_endpoint, first_seen_unix_ms, alive}, ...]}
    std::string DumpAgents() const;

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, AgentEntry> m_agents;
    std::atomic<uint64_t> m_heartbeat_count{0};
};

} // namespace pudimcollector
