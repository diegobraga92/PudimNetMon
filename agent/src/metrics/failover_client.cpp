#include "failover_client.h"

namespace pudimagent {

FailoverClient::FailoverClient(std::vector<std::string> endpoints)
    : m_endpoints(std::move(endpoints)) {
    if (m_endpoints.empty()) {
        m_endpoints.push_back("localhost:50051");
    }
}

const std::string &FailoverClient::CurrentEndpoint() const {
    return m_endpoints[m_current];
}

bool FailoverClient::OnSendFailure() {
    m_strikes++;
    if (m_strikes < kMaxStrikes) return false;

    // Rotate to the next endpoint (wraps around; the first endpoint stays the
    // primary so a recovered primary is preferred once all others are tried).
    m_strikes = 0;
    m_failovers++;
    m_current = (m_current + 1) % m_endpoints.size();
    return true;
}

void FailoverClient::OnSendSuccess() {
    m_strikes = 0;
}

} // namespace pudimagent
