// Unit tests for the heartbeat agent registry: removal and TTL expiry.
// No database or gRPC server is required.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "agent_registry.h"
#include "heartbeat.pb.h"

namespace {

using pudimcollector::AgentRegistry;
using pudimnetmon::HeartbeatRequest;

int g_failures = 0;

void Check(bool cond, const std::string &label) {
    if (cond) {
        std::cout << "PASS " << label << "\n";
    } else {
        std::cerr << "FAIL " << label << "\n";
        ++g_failures;
    }
}

HeartbeatRequest MakeHeartbeat(const std::string &id) {
    HeartbeatRequest req;
    req.set_agent_id(id);
    req.set_interval_ms(5000);
    req.set_version("0.1.0");
    req.set_diagnostic_endpoint(id + ".lan:50052");
    return req;
}

} // namespace

int main() {
    AgentRegistry registry;
    registry.RecordHeartbeat(MakeHeartbeat("agent-a"));
    registry.RecordHeartbeat(MakeHeartbeat("agent-b"));
    Check(registry.TotalAgentCount() == 2, "two agents registered");
    Check(registry.IsAgentAlive("agent-a", 60000), "agent-a alive");

    // Removing an existing agent drops it and reports success.
    Check(registry.RemoveAgent("agent-a"), "remove existing agent returns true");
    Check(registry.TotalAgentCount() == 1, "registry shrinks after removal");
    Check(!registry.IsAgentAlive("agent-a", 60000), "removed agent not alive");
    Check(registry.GetDiagnosticEndpoint("agent-a").empty(),
          "removed agent has no endpoint");

    // Removing an unknown agent is a no-op that reports failure.
    Check(!registry.RemoveAgent("agent-a"), "remove unknown agent returns false");
    Check(registry.TotalAgentCount() == 1,
          "count unchanged after failed removal");

    // The remaining agent is still registered.
    Check(registry.IsAgentAlive("agent-b", 60000), "agent-b still alive");

    // TTL expiry: age the entry out, then prune with a short window.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(registry.ExpireStale(0) == 0, "non-positive ttl is a no-op");
    Check(registry.ExpireStale(100000) == 0,
          "fresh agent survives a long-lived ttl");
    Check(registry.ExpireStale(1) == 1, "stale agent expired");
    Check(registry.TotalAgentCount() == 0, "registry empty after expiry");

    if (g_failures > 0) {
        std::cerr << g_failures << " agent-registry test(s) failed\n";
        return 1;
    }
    std::cout << "All agent-registry tests passed\n";
    return 0;
}
