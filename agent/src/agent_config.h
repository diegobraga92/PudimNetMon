#pragma once

#include <map>
#include <string>
#include <vector>

#include "logger.h"

namespace pudimagent {

struct AgentConfig {
    std::string config_path;
    bool cfg_loaded = false;

    std::map<std::string, std::string> file_cfg;
    std::map<std::string, std::string> cli_options;

    std::string collector_endpoint;
    std::vector<std::string> collector_endpoints;
    std::string node_id;
    std::string version;
    std::string trace_id;
    int interval_ms = 5000;

    std::string tls_ca;
    std::string tls_cert;
    std::string tls_key;

    std::string diagnostic_port;
    std::string diagnostic_address;

    std::string ntp_server;

    logger::LogLevel log_level = logger::LogLevel::Info;

    std::vector<std::string> dns_targets;
    std::vector<std::string> tcp_targets;
    std::vector<std::string> tls_targets;
    std::vector<std::string> http_targets;
    std::vector<std::string> ping_targets;
    std::vector<std::string> http_protocols;
    std::map<std::string, std::vector<std::string>> dns_expected;
    int ping_count = 4;
    int ping_gap_ms = 200;
    bool tls_cert_check = true;
    bool tcp_retransmit_check = true;
    bool tcp_handshake_capture = true;
    int tcp_handshake_interval_ms = 0;

    bool use_stream_metrics = false;
    int max_buffer_size = 200;
    std::string disk_buffer_path;
    int disk_buffer_max_mb = 100;
};

enum class ConfigResult {
    Ok,
    Help,
    Error,
};

std::string DefaultConfigPath();
std::string DefaultDiskBufferPath();

ConfigResult LoadAgentConfig(int argc, char **argv, AgentConfig &out);

void LogConfigAudit(const AgentConfig &cfg);

} // namespace pudimagent
