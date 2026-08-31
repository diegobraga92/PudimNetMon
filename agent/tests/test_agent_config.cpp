#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "agent_config.h"
#include "platform/getopt.h"

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &name) {
    if (cond) {
        std::cout << "PASS " << name << "\n";
    } else {
        std::cerr << "FAIL " << name << "\n";
        ++g_failures;
    }
}

std::string WriteTempConfig(const std::string &content) {
    auto ns = std::chrono::high_resolution_clock::now()
                  .time_since_epoch()
                  .count();
    auto path = std::filesystem::temp_directory_path() /
                ("pudim_agent_cfg_test_" + std::to_string(ns) + ".conf");
    std::ofstream out(path);
    out << content;
    out.close();
    return path.string();
}

// Owns the storage behind an argv array (argv[0] is always "pudim-agent").
struct Args {
    std::vector<std::string> storage;
    std::vector<char *> argv;

    explicit Args(std::vector<std::string> list) {
        storage.push_back("pudim-agent");
        for (auto &s : list) storage.push_back(s);
        for (auto &s : storage) {
            argv.push_back(const_cast<char *>(s.c_str()));
        }
        argv.push_back(nullptr);
    }

    int argc() const { return static_cast<int>(storage.size()); }
    char **data() { return argv.data(); }
};

// getopt keeps global state; reset it so each call rescans from argv[1].
void ResetGetopt() {
    optind = 1;
    opterr = 0;
}

} // namespace

int main() {
    // 1. Built-in defaults with no config file and no CLI flags.
    {
        Args args({});
        ResetGetopt();
        pudimagent::AgentConfig cfg;
        Check(pudimagent::LoadAgentConfig(args.argc(), args.data(), cfg) ==
                  pudimagent::ConfigResult::Ok,
              "defaults load ok");
        Check(cfg.collector_endpoint == "localhost:50051",
              "defaults collector-endpoint");
        Check(cfg.node_id == "agent-unknown", "defaults node-id");
        Check(cfg.interval_ms == 5000, "defaults interval");
        Check(cfg.version == "0.1.0", "defaults version");
        Check(cfg.ping_count == 4 && cfg.ping_gap_ms == 200, "defaults ping");
        Check(cfg.tls_cert_check && cfg.tcp_retransmit_check &&
                  cfg.tcp_handshake_capture,
              "defaults deep diagnostics on");
        Check(cfg.log_level == logger::LogLevel::Info, "defaults log level");
        Check(!cfg.cfg_loaded, "defaults no config file");
        Check(cfg.config_path == "/etc/pudim/agent.conf", "defaults config path");
    }

    // 2. Config file overrides the built-in defaults.
    {
        std::string path = WriteTempConfig(
            "node-id=file-node\ninterval=7000\ndns-targets=dns1.lan,dns2.lan\n"
            "stream-metrics=true\n");
        Args args({"--config-file", path});
        ResetGetopt();
        pudimagent::AgentConfig cfg;
        Check(pudimagent::LoadAgentConfig(args.argc(), args.data(), cfg) ==
                  pudimagent::ConfigResult::Ok,
              "file load ok");
        Check(cfg.cfg_loaded, "file cfg_loaded");
        Check(cfg.node_id == "file-node", "file node-id");
        Check(cfg.interval_ms == 7000, "file interval");
        Check(cfg.dns_targets.size() == 2 && cfg.dns_targets[0] == "dns1.lan" &&
                  cfg.dns_targets[1] == "dns2.lan",
              "file dns-targets list");
        Check(cfg.use_stream_metrics, "file stream-metrics");
        std::filesystem::remove(path);
    }

    // 3. CLI flags override the config file.
    {
        std::string path = WriteTempConfig("node-id=file-node\ninterval=7000\n");
        Args args({"--config-file", path, "--node-id", "cli-node",
                   "--interval", "9000"});
        ResetGetopt();
        pudimagent::AgentConfig cfg;
        Check(pudimagent::LoadAgentConfig(args.argc(), args.data(), cfg) ==
                  pudimagent::ConfigResult::Ok,
              "cli override ok");
        Check(cfg.node_id == "cli-node", "cli overrides file node-id");
        Check(cfg.interval_ms == 9000, "cli overrides file interval");
        std::filesystem::remove(path);
    }

    // 4. Comma-separated CLI lists and presence flags.
    {
        Args args({"--dns-targets", "a.com,b.com", "--tcp-targets", "h:1,h:2",
                   "--collector-endpoints", "c1:50051,c2:50051",
                   "--stream-metrics", "--no-tls-cert", "--log-level", "debug",
                   "--ping-count", "8"});
        ResetGetopt();
        pudimagent::AgentConfig cfg;
        Check(pudimagent::LoadAgentConfig(args.argc(), args.data(), cfg) ==
                  pudimagent::ConfigResult::Ok,
              "cli lists ok");
        Check(cfg.dns_targets.size() == 2 && cfg.dns_targets[1] == "b.com",
              "cli dns-targets");
        Check(cfg.tcp_targets.size() == 2, "cli tcp-targets");
        Check(cfg.collector_endpoints.size() == 2 &&
                  cfg.collector_endpoints[0] == "c1:50051",
              "cli collector-endpoints");
        Check(cfg.use_stream_metrics, "cli stream-metrics flag");
        Check(!cfg.tls_cert_check, "cli no-tls-cert flag");
        Check(cfg.log_level == logger::LogLevel::Debug, "cli log-level");
        Check(cfg.ping_count == 8, "cli ping-count");
    }

    // 5. dns-expected parses host -> records (multiple records per host).
    {
        Args args({"--dns-expected",
                   "example.com=A:1.2.3.4,example.com=AAAA::1"});
        ResetGetopt();
        pudimagent::AgentConfig cfg;
        Check(pudimagent::LoadAgentConfig(args.argc(), args.data(), cfg) ==
                  pudimagent::ConfigResult::Ok,
              "dns-expected ok");
        auto it = cfg.dns_expected.find("example.com");
        Check(it != cfg.dns_expected.end() && it->second.size() == 2,
              "dns-expected host records");
    }

    // 6. Unknown key in the config file is rejected.
    {
        std::string path = WriteTempConfig("node-id=ok\nbogus-key=value\n");
        Args args({"--config-file", path});
        ResetGetopt();
        pudimagent::AgentConfig cfg;
        Check(pudimagent::LoadAgentConfig(args.argc(), args.data(), cfg) ==
                  pudimagent::ConfigResult::Error,
              "unknown config key rejected");
        std::filesystem::remove(path);
    }

    // 7. Invalid --log-level is rejected.
    {
        Args args({"--log-level", "verbose"});
        ResetGetopt();
        pudimagent::AgentConfig cfg;
        Check(pudimagent::LoadAgentConfig(args.argc(), args.data(), cfg) ==
                  pudimagent::ConfigResult::Error,
              "invalid log level rejected");
    }

    // 8. --help prints usage and asks the caller to exit 0.
    {
        Args args({"--help"});
        ResetGetopt();
        pudimagent::AgentConfig cfg;
        Check(pudimagent::LoadAgentConfig(args.argc(), args.data(), cfg) ==
                  pudimagent::ConfigResult::Help,
              "help returns Help");
    }

    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "All agent-config tests passed\n";
    return 0;
}

