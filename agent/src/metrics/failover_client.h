#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pudimagent {

// Manages a prioritized list of collector endpoints with automatic failover.
// A send is "failed" via OnSendFailure(); after kMaxStrikes consecutive
// failures the next endpoint is selected and OnSendFailure() returns true so
// the caller can recreate its gRPC clients against the new endpoint.
class FailoverClient {
public:
    explicit FailoverClient(std::vector<std::string> endpoints);

    // Endpoint currently in use.
    const std::string &CurrentEndpoint() const;

    // Records a send failure. Returns true when the endpoint was rotated
    // (caller should recreate clients and log the failover).
    bool OnSendFailure();

    // Records a successful send; resets the consecutive-failure strike count.
    void OnSendSuccess();

    size_t FailoverCount() const { return m_failovers; }
    size_t EndpointCount() const { return m_endpoints.size(); }

    static constexpr int kMaxStrikes = 3;

private:
    std::vector<std::string> m_endpoints;
    size_t m_current = 0;
    int m_strikes = 0;
    uint64_t m_failovers = 0;
};

} // namespace pudimagent