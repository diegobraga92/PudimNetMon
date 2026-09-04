#pragma once

#include <map>
#include <string>
#include <vector>

#include "logger.h"
#include "metrics/probe_config.h"

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

    ProbeConfig probe_cfg;

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
