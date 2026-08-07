#include "alert_rule.h"

namespace pudimcollector::alerting {

std::string CheckTypeToString(pudimnetmon::CheckType type) {
    switch (type) {
        case pudimnetmon::CHECK_TYPE_DNS_RESOLUTION: return "dns_resolution";
        case pudimnetmon::CHECK_TYPE_TCP_CONNECT:    return "tcp_connect";
        case pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE:  return "tls_handshake";
        case pudimnetmon::CHECK_TYPE_HTTP_REQUEST:   return "http_request";
        case pudimnetmon::CHECK_TYPE_ICMP_PING:      return "icmp_ping";
        case pudimnetmon::CHECK_TYPE_JITTER:         return "jitter";
        default:                                     return "unspecified";
    }
}

bool CheckTypeFromString(const std::string &s, pudimnetmon::CheckType &out) {
    if (s == "dns_resolution")      { out = pudimnetmon::CHECK_TYPE_DNS_RESOLUTION; return true; }
    if (s == "tcp_connect")         { out = pudimnetmon::CHECK_TYPE_TCP_CONNECT;    return true; }
    if (s == "tls_handshake")       { out = pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE;  return true; }
    if (s == "http_request")        { out = pudimnetmon::CHECK_TYPE_HTTP_REQUEST;   return true; }
    if (s == "icmp_ping")           { out = pudimnetmon::CHECK_TYPE_ICMP_PING;      return true; }
    if (s == "jitter")              { out = pudimnetmon::CHECK_TYPE_JITTER;         return true; }
    return false;
}

bool GetMetricValue(const pudimnetmon::Metric &m, const std::string &field,
                    double &out) {
    if (field == "latency_ms" && m.has_latency_ms()) {
        out = m.latency_ms();
        return true;
    }
    if (field == "packet_loss_pct" && m.has_packet_loss_pct()) {
        out = m.packet_loss_pct();
        return true;
    }
    if (field == "jitter_ms" && m.has_jitter_ms()) {
        out = m.jitter_ms();
        return true;
    }
    if (field == "rtt_ms" && m.has_rtt_ms()) {
        out = m.rtt_ms();
        return true;
    }
    if (field == "status_code" && m.has_status_code()) {
        out = static_cast<double>(m.status_code());
        return true;
    }
    return false;
}

} // namespace pudimcollector::alerting
