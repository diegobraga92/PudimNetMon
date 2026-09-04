#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pudimagent {

class FailoverClient {
public:
    explicit FailoverClient(std::vector<std::string> endpoints);

    const std::string &CurrentEndpoint() const;

    bool OnSendFailure();
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