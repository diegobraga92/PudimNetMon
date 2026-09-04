#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pudimagent {

struct ProbeConfig {
    // Empty vector means skipped
    std::vector<std::string> dns_targets;      // e.g. {"example.com"}
    std::vector<std::string> tcp_targets;      // e.g. {"10.0.0.1:443"}
    std::vector<std::string> tls_targets;      // e.g. {"example.com:443"}
    std::vector<std::string> http_targets;     // e.g. {"https://example.com"}
    std::vector<std::string> ping_targets;     // e.g. {"8.8.8.8"}

    int ping_count = 4;
    int ping_gap_ms = 200;

    bool tls_cert_check = true;        // TLS_CERTIFICATE
    bool tcp_retransmit_check = true;  // TCP_RETRANSMIT
    bool tcp_handshake_capture = true; // TCP_HANDSHAKE

    std::vector<std::string> http_protocols;
    std::map<std::string, std::vector<std::string>> dns_expected;

    bool ntp_check = true;
};

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
