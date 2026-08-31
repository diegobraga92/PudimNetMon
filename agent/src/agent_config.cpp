#include "agent_config.h"

#include <iostream>
#include <string>
#include <utility>

#include "config_file.h"
#include "platform/getopt.h"
#include "platform/platform.h"

namespace pudimagent {

namespace {

// Returns true and sets `out` when `s` names a supported log verbosity.
bool ParseLogLevel(const std::string &s, logger::LogLevel &out) {
    if (s == "debug") { out = logger::LogLevel::Debug; return true; }
    if (s == "info") { out = logger::LogLevel::Info; return true; }
    if (s == "warn") { out = logger::LogLevel::Warn; return true; }
    if (s == "error") { out = logger::LogLevel::Error; return true; }
    return false;
}

// Splits a comma-separated value into non-empty entries.
std::vector<std::string> ParseList(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Appends entries of the form host=TYPE:value to `out` (the dns-expected
// option; multiple records for one host accumulate).
void ApplyDnsExpected(
    const std::string &s,
    std::map<std::string, std::vector<std::string>> &out) {
    // Format: host=TYPE:value,host2=TYPE:value
    for (auto &entry : ParseList(s)) {
        auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string host = entry.substr(0, eq);
        std::string rec = entry.substr(eq + 1);
        if (!host.empty() && !rec.empty()) {
            out[host].push_back(rec);
        }
    }
}

} // namespace

ConfigResult LoadAgentConfig(int argc, char **argv, AgentConfig &out) {
    // ------------------------------------------------------------------
    // Layered configuration: built-in defaults < config file < CLI flags.
    // The config file is flat key=value with '#' comments; keys are the
    // long CLI option names (see agent/config/agent.conf.example).
    // ------------------------------------------------------------------
    std::string config_path;
    bool config_explicit = false;
    const std::string kConfigFileFlag = "--config-file=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config-file" && i + 1 < argc) {
            config_path = argv[++i];
            config_explicit = true;
        } else if (arg.rfind(kConfigFileFlag, 0) == 0) {
            config_path = arg.substr(kConfigFileFlag.size());
            config_explicit = true;
        }
    }
    if (config_path.empty()) {
#ifdef _WIN32
        config_path = pudimagent::platform::DefaultStateDir() + "\\agent.conf";
#else
        config_path = "/etc/pudim/agent.conf";
#endif
    }

    std::map<std::string, std::string> file_cfg;
    std::string cfg_error;
    bool cfg_loaded = false;
    // An explicitly requested file must exist and parse; the default path is
    // optional and silently skipped when absent.
    if (config_explicit || config::Exists(config_path)) {
        if (!config::LoadConfigFile(config_path, file_cfg, cfg_error)) {
            std::cerr << "Error loading config file '" << config_path
                      << "': " << cfg_error << "\n";
            return ConfigResult::Error;
        }
        cfg_loaded = true;
    }

    // Typed accessors with built-in defaults as the bottom layer.
    auto get_s = [&file_cfg](const char *key, std::string fallback) -> std::string {
        auto it = file_cfg.find(key);
        return it != file_cfg.end() ? it->second : std::move(fallback);
    };
    auto get_i = [&file_cfg](const char *key, int fallback) -> int {
        auto it = file_cfg.find(key);
        if (it == file_cfg.end()) return fallback;
        try {
            return std::stoi(it->second);
        } catch (...) {
            std::cerr << "Config: invalid integer for '" << key << "': '"
                      << it->second << "' (using " << fallback << ")\n";
            return fallback;
        }
    };
    auto get_b = [&file_cfg](const char *key, bool fallback) -> bool {
        auto it = file_cfg.find(key);
        if (it == file_cfg.end()) return fallback;
        const std::string &v = it->second;
        if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
        if (v == "false" || v == "0" || v == "no" || v == "off") return false;
        std::cerr << "Config: invalid boolean for '" << key << "': '" << v
                  << "' (using " << (fallback ? "true" : "false") << ")\n";
        return fallback;
    };


    // Defaults (config file wins over these; CLI flags win over the file)
    std::string collector_endpoint = get_s("collector-endpoint", "localhost:50051");
    std::string node_id = get_s("node-id", "agent-unknown");
    std::string trace_id = get_s("trace-id", "");
    int interval_ms = get_i("interval", 5000);
    std::string version = get_s("version", "0.1.0");
    bool use_stream_metrics = get_b("stream-metrics", false);

    // Probe targets (comma-separated lists)
    std::vector<std::string> dns_targets;
    std::vector<std::string> tcp_targets;
    std::vector<std::string> tls_targets;
    std::vector<std::string> http_targets;
    std::vector<std::string> ping_targets;
    int ping_count = get_i("ping-count", 4);
    int ping_gap_ms = get_i("ping-gap-ms", 200);  // delay between individual ICMP pings

    // Phase 4 deep-diagnostics configuration
    bool tls_cert_check = !get_b("no-tls-cert", false);
    bool tcp_retransmit_check = !get_b("no-tcp-retransmit", false);
    bool tcp_handshake_capture = !get_b("no-tcp-handshake", false);
    int tcp_handshake_interval_ms = get_i("tcp-handshake-interval", 0);  // 0 = every cycle
    std::vector<std::string> http_protocols;
    std::map<std::string, std::vector<std::string>> dns_expected;
    std::string diagnostic_port = get_s("diagnostic-port", "50052");
    std::string diagnostic_address = get_s("diagnostic-address", "");
    std::string collector_endpoints_input = get_s("collector-endpoints", "");
    int max_buffer_size = get_i("max-buffer-size", 200);  // in-memory batch buffer cap
    std::string disk_buffer_path =
        get_s("disk-buffer-path",
#ifdef _WIN32
              pudimagent::platform::DefaultStateDir() + "\\pending.db"
#else
              "/var/lib/pudim/pending.db"
#endif
        );
    int disk_buffer_max_mb = get_i("disk-buffer-max-mb", 100);
    std::string tls_ca = get_s("tls-ca", "");      // PEM CA used to verify the collector (mTLS)
    std::string tls_cert = get_s("tls-cert", "");  // PEM client certificate
    std::string tls_key = get_s("tls-key", "");    // PEM client private key
    std::string ntp_server = get_s("ntp-server", "pool.ntp.org");
    std::string log_level_str = get_s("log-level", "info");  // debug|info|warn|error

    // Apply list-valued options from the config file (CLI flags override below).
    dns_targets = ParseList(get_s("dns-targets", ""));
    tcp_targets = ParseList(get_s("tcp-targets", ""));
    tls_targets = ParseList(get_s("tls-targets", ""));
    http_targets = ParseList(get_s("http-targets", ""));
    ping_targets = ParseList(get_s("ping-targets", ""));
    http_protocols = ParseList(get_s("http-protocols", ""));
    ApplyDnsExpected(get_s("dns-expected", ""), dns_expected);


    // Parse CLI flags using getopt
    static struct option long_options[] = {
        {"collector-endpoint", required_argument, nullptr, 'c'},
        {"node-id",            required_argument, nullptr, 'n'},
        {"interval",           required_argument, nullptr, 'i'},
        {"trace-id",           required_argument, nullptr, 't'},
        {"version",            required_argument, nullptr, 'v'},
        {"dns-targets",        required_argument, nullptr, 'd'},
        {"tcp-targets",        required_argument, nullptr, 'p'},
        {"tls-targets",        required_argument, nullptr, 's'},
        {"http-targets",       required_argument, nullptr, 'w'},
        {"ping-targets",       required_argument, nullptr, 'g'},
        {"ping-count",         required_argument, nullptr, 'k'},
        {"ping-gap-ms",        required_argument, nullptr, 'u'},
        {"stream-metrics",     no_argument,       nullptr, 'm'},
        {"no-tls-cert",        no_argument,       nullptr, 'q'},
        {"no-tcp-retransmit",  no_argument,       nullptr, 'r'},
        {"no-tcp-handshake",   no_argument,       nullptr, 'e'},
        {"tcp-handshake-interval", required_argument, nullptr, 1001},
        {"log-level",          required_argument, nullptr, 1000},
        {"http-protocols",     required_argument, nullptr, 'x'},
        {"dns-expected",       required_argument, nullptr, 'y'},
        {"diagnostic-port",    required_argument, nullptr, 'z'},
        {"diagnostic-address", required_argument, nullptr, 'a'},
        {"collector-endpoints", required_argument, nullptr, 'b'},
        {"max-buffer-size",     required_argument, nullptr, 'f'},
        {"disk-buffer-path",    required_argument, nullptr, 'j'},
        {"disk-buffer-max-mb",  required_argument, nullptr, 'l'},
        {"tls-ca",              required_argument, nullptr, 'C'},
        {"tls-cert",            required_argument, nullptr, 'E'},
        {"tls-key",             required_argument, nullptr, 'K'},
        {"ntp-server",          required_argument, nullptr, 'o'},
        {"config-file",        required_argument, nullptr, 1002},
        {"help",               no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    // Reject unknown keys from the config file so a typo fails fast instead of
    // silently falling back to defaults (tracking/debugging aid).
    for (const auto &kv : file_cfg) {
        bool known = false;
        for (const auto &o : long_options) {
            if (o.name && kv.first == o.name) {
                known = true;
                break;
            }
        }
        if (!known) {
            std::cerr << "Unknown option in config file: '" << kv.first
                      << "' (see --help for valid options)\n";
            return ConfigResult::Error;
        }
    }

    // Map each option's code back to its long name so the CLI values actually
    // applied can be audited against the config file (see the startup log).
    std::map<int, const char *> name_by_val;
    for (const auto &o : long_options) {
        if (o.name && std::string(o.name) != "config-file") {
            name_by_val[o.val] = o.name;
        }
    }
    std::map<std::string, std::string> cli_options;

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "c:n:i:t:v:d:p:s:w:g:k:u:mo:qrex:y:z:a:b:f:j:l:C:E:K:h", long_options, &option_index)) != -1) {
        // Record what the CLI actually provided (presence flags have no value).
        auto nit = name_by_val.find(opt);
        if (nit != name_by_val.end()) {
            cli_options[nit->second] = optarg ? optarg : "";
        }
        switch (opt) {
            case 'c': collector_endpoint = optarg; break;
            case 'n': node_id = optarg; break;
            case 'i': interval_ms = std::stoi(optarg); break;
            case 't': trace_id = optarg; break;
            case 'v': version = optarg; break;
            case 'd': dns_targets = ParseList(optarg); break;
            case 'p': tcp_targets = ParseList(optarg); break;
            case 's': tls_targets = ParseList(optarg); break;
            case 'w': http_targets = ParseList(optarg); break;
            case 'g': ping_targets = ParseList(optarg); break;
            case 'k': ping_count = std::stoi(optarg); break;
            case 'u': ping_gap_ms = std::stoi(optarg); break;
            case 'm': use_stream_metrics = true; break;
            case 'q': tls_cert_check = false; break;
            case 'r': tcp_retransmit_check = false; break;
            case 'e': tcp_handshake_capture = false; break;
            case 1001: tcp_handshake_interval_ms = std::stoi(optarg); break;
            case 1000: log_level_str = optarg; break;
            case 'x': http_protocols = ParseList(optarg); break;
            case 'y': ApplyDnsExpected(optarg, dns_expected); break;
            case 1002: break;  // --config-file was handled by the pre-scan above
            case 'z': diagnostic_port = optarg; break;
            case 'a': diagnostic_address = optarg; break;
            case 'b': collector_endpoints_input = optarg; break;
            case 'f': max_buffer_size = std::stoi(optarg); break;
            case 'j': disk_buffer_path = optarg; break;
            case 'l': disk_buffer_max_mb = std::stoi(optarg); break;
            case 'C': tls_ca = optarg; break;
            case 'E': tls_cert = optarg; break;
            case 'K': tls_key = optarg; break;
            case 'o': ntp_server = optarg; break;

            case 'h':
                std::cout << "Usage: pudim-agent [options]\n"
#ifdef _WIN32
                          << "      --config-file        Key=value config file (default: <state-dir>\\agent.conf)\n"
#else
                          << "      --config-file        Key=value config file (default: /etc/pudim/agent.conf)\n"
#endif
                          << "  -c, --collector-endpoint  Collector gRPC endpoint (default: localhost:50051)\n"
                          << "  -n, --node-id             Unique node identifier (default: agent-unknown)\n"
                          << "  -i, --interval            Heartbeat interval in ms (default: 5000)\n"
                          << "  -t, --trace-id            Trace ID for request correlation\n"
                          << "  -v, --version             Agent version string\n"
                          << "  -d, --dns-targets         Comma-separated DNS resolution targets\n"
                          << "  -p, --tcp-targets         Comma-separated host:port TCP connect targets\n"
                          << "  -s, --tls-targets         Comma-separated host:port TLS handshake targets\n"
                          << "  -w, --http-targets        Comma-separated HTTP(S) URLs\n"
                          << "  -g, --ping-targets        Comma-separated ICMP ping targets\n"
                          << "  -k, --ping-count          Number of pings per target (default: 4)\n"
                          << "  -u, --ping-gap-ms         Delay between individual pings in ms (default: 200)\n"
                          << "  -m, --stream-metrics      Use client-streaming RPC for metrics (default: unary)\n"
                          << "      --no-tls-cert         Disable TLS certificate validation probe\n"
                          << "      --no-tcp-retransmit   Disable TCP retransmission probe\n"
                          << "      --no-tcp-handshake    Disable TCP handshake capture (libpcap)\n"
                          << "      --tcp-handshake-interval  Run pcap handshake capture at most this often (ms; default: every cycle)\n"
                          << "      --log-level           Log verbosity: debug, info, warn, error (default: info)\n"
                          << "  -x, --http-protocols      HTTP versions to measure: http1.1,http2,http3\n"
                          << "  -y, --dns-expected        Expected DNS records: host=A:1.2.3.4,host2=CNAME:x\n"
                          << "  -z, --diagnostic-port     gRPC diagnostic server port (default: 50052)\n"
                          << "  -a, --diagnostic-address  Advertised diagnostic endpoint, e.g. agent.example.com:50052\n"
                          << "  -b, --collector-endpoints Comma-separated collector endpoints for failover\n"
                          << "  -f, --max-buffer-size     Max queued metric batches before dropping (default: 200)\n"
                          << "  -j, --disk-buffer-path    SQLite path for persistent buffering (default: /var/lib/pudim/pending.db)\n"
                          << "  -l, --disk-buffer-max-mb  Max disk buffer size in MB (default: 100)\n"
                          << "  -C, --tls-ca              PEM CA to verify the collector (mTLS)\n"
                          << "  -E, --tls-cert             PEM client certificate (mTLS)\n"
                          << "  -K, --tls-key              PEM client private key (mTLS)\n"
                          << "  -o, --ntp-server          NTP server for the offset probe (default: pool.ntp.org)\n"
                          << "  -h, --help                Show this help\n";
                return ConfigResult::Help;
            default:
                std::cerr << "Unknown option. Use --help for usage.\n";
                return ConfigResult::Error;
        }
    }

    // Populate the resolved configuration.
    out.config_path = std::move(config_path);
    out.cfg_loaded = cfg_loaded;
    out.file_cfg = std::move(file_cfg);
    out.cli_options = std::move(cli_options);

    out.collector_endpoint = std::move(collector_endpoint);
    out.collector_endpoints = ParseList(collector_endpoints_input);
    out.node_id = std::move(node_id);
    out.version = std::move(version);
    out.trace_id = std::move(trace_id);
    out.interval_ms = interval_ms;
    out.tls_ca = std::move(tls_ca);
    out.tls_cert = std::move(tls_cert);
    out.tls_key = std::move(tls_key);
    out.diagnostic_port = std::move(diagnostic_port);
    out.diagnostic_address = std::move(diagnostic_address);
    out.ntp_server = std::move(ntp_server);

    out.dns_targets = std::move(dns_targets);
    out.tcp_targets = std::move(tcp_targets);
    out.tls_targets = std::move(tls_targets);
    out.http_targets = std::move(http_targets);
    out.ping_targets = std::move(ping_targets);
    out.http_protocols = std::move(http_protocols);
    out.dns_expected = std::move(dns_expected);
    out.ping_count = ping_count;
    out.ping_gap_ms = ping_gap_ms;
    out.tls_cert_check = tls_cert_check;
    out.tcp_retransmit_check = tcp_retransmit_check;
    out.tcp_handshake_capture = tcp_handshake_capture;
    out.tcp_handshake_interval_ms = tcp_handshake_interval_ms;
    out.use_stream_metrics = use_stream_metrics;
    out.max_buffer_size = max_buffer_size;
    out.disk_buffer_path = std::move(disk_buffer_path);
    out.disk_buffer_max_mb = disk_buffer_max_mb;

    // Apply the log verbosity (unrecognized values are rejected).
    if (!ParseLogLevel(log_level_str, out.log_level)) {
        std::cerr << "Invalid --log-level '" << log_level_str
                  << "'; expected debug|info|warn|error\n";
        return ConfigResult::Error;
    }

    return ConfigResult::Ok;
}

void LogConfigAudit(const AgentConfig &cfg) {
    // Layered-config audit: log every config-file value that survived, and call
    // out any CLI flag that overrides the file, so the effective configuration
    // (and where each value came from) is traceable in the logs.
    for (const auto &kv : cfg.file_cfg) {
        auto cli = cfg.cli_options.find(kv.first);
        if (cli != cfg.cli_options.end() && cli->second != kv.second) {
            LOG_INFO("CLI --" + kv.first +
                     (cli->second.empty() ? "" : ("=" + cli->second)) +
                     " overrides config file value '" + kv.second + "'");
        } else if (cli == cfg.cli_options.end()) {
            LOG_DEBUG("config file: " + kv.first + "=" + kv.second);
        }
    }
    for (const auto &kv : cfg.cli_options) {
        if (!cfg.file_cfg.count(kv.first)) {
            LOG_DEBUG("CLI --" + kv.first +
                      (kv.second.empty() ? "" : ("=" + kv.second)) +
                      " (not set in config file)");
        }
    }
}

} // namespace pudimagent

