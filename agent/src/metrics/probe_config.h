#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pudimagent {

// Targets + tuning for a probe cycle. Single source of truth shared by the
// startup AgentConfig (embedded member) and the runtime Reconfigure RPC /
// probe worker, so both config paths write the same shape.
struct ProbeConfig {
    // Targets for each probe type. Empty vector means skipped.
    std::vector<std::string> dns_targets;      // e.g. {"example.com"}
    std::vector<std::string> tcp_targets;      // e.g. {"10.0.0.1:443"}
    std::vector<std::string> tls_targets;      // e.g. {"example.com:443"}
    std::vector<std::string> http_targets;     // e.g. {"https://example.com"}
    std::vector<std::string> ping_targets;     // e.g. {"8.8.8.8"}

    int ping_count = 4;
    int ping_gap_ms = 200;

    // Deep diagnostics (all default-on; disabled via CLI).
    bool tls_cert_check = true;        // TLS_CERTIFICATE metrics for tls_targets
    bool tcp_retransmit_check = true;  // TCP_RETRANSMIT metrics for tcp_targets
    bool tcp_handshake_capture = true; // TCP_HANDSHAKE metrics (libpcap timing)
    // HTTP protocol versions to measure for each http_target; empty = default
    // (curl's negotiated version). Entries: "http1.1", "http2", "http3".
    std::vector<std::string> http_protocols;
    // Expected DNS records keyed by host: "example.com" -> {"A:93.184.216.34"}.
    // A configured record that mismatches the resolved value marks the metric
    // success=false (alarm on mismatch).
    std::map<std::string, std::vector<std::string>> dns_expected;
    // Clock hygiene.
    bool ntp_check = true;             // emit CHECK_TYPE_NTP_OFFSET each cycle
};

// Runtime probe configuration store. The probe worker copies the current
// config each cycle; the collector's Reconfigure RPC swaps in a new one so
// checks can be added/edited without restarting the agent.
struct ProbeConfigStore {
    mutable std::mutex mu;
    ProbeConfig cfg;

    void Set(const ProbeConfig &c) {
        std::lock_guard<std::mutex> lock(mu);
        cfg = c;
    }
    ProbeConfig Get() const {
        std::lock_guard<std::mutex> lock(mu);
        return cfg;
    }
};

} // namespace pudimagent
