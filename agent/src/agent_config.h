#pragma once

#include <map>
#include <string>
#include <vector>

#include "logger.h"

namespace pudimagent {

// Fully-resolved agent configuration: built-in defaults < config file < CLI
// flags. Produced by LoadAgentConfig(); see agent_config.cpp for the option
// table, the layered precedence and the --help text.
struct AgentConfig {
    // Where the config file was (or would be) loaded from, and whether one was
    // actually parsed and applied (false = built-in defaults + CLI only).
    std::string config_path;
    bool cfg_loaded = false;

    // Raw file/CLI inputs kept for the startup configuration audit log.
    std::map<std::string, std::string> file_cfg;
    std::map<std::string, std::string> cli_options;

    // Collector / identity.
    std::string collector_endpoint;
    std::vector<std::string> collector_endpoints;  // failover list (empty = single endpoint)
    std::string node_id;
    std::string version;
    std::string trace_id;
    int interval_ms = 5000;

    // Transport security (mTLS).
    std::string tls_ca;
    std::string tls_cert;
    std::string tls_key;

    // Diagnostic server.
    std::string diagnostic_port;
    std::string diagnostic_address;

    // NTP offset probe server.
    std::string ntp_server;

    // Log verbosity (validated at load time).
    logger::LogLevel log_level = logger::LogLevel::Info;

    // Probe targets and tuning.
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
    int tcp_handshake_interval_ms = 0;  // 0 = run every cycle

    // Delivery.
    bool use_stream_metrics = false;
    int max_buffer_size = 200;
    std::string disk_buffer_path;
    int disk_buffer_max_mb = 100;
};

// Result of parsing the command line plus the optional config file.
enum class ConfigResult {
    Ok,    // `out` populated; proceed with startup.
    Help,  // --help was printed; exit 0 without running the agent.
    Error, // A parse/config error was printed to stderr; exit 1.
};

// Resolves the layered configuration (defaults < config file < CLI flags).
// Prints errors to stderr and the --help usage to stdout. On Ok every field of
// `out` is populated.
ConfigResult LoadAgentConfig(int argc, char **argv, AgentConfig &out);

// Startup audit log: echoes the effective configuration and flags any CLI
// value that overrides a config-file value. Must be called after the logger
// level/context are configured.
void LogConfigAudit(const AgentConfig &cfg);

} // namespace pudimagent
