#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>
#include "platform/getopt.h"
#include "platform/platform.h"
#include "platform/win_service.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include "heartbeat.grpc.pb.h"
#include "metrics.pb.h"
#include "metrics/metrics_client.h"
#include "metrics/probes.h"
#include "metrics/ntp_probe.h"
#include "metrics/failover_client.h"
#include "diagnostic_service.h"
#include "systemd_notify.h"
#include "trace_context.h"
#include "disk_buffer.h"
#include "tls_credentials.h"
#include "probe_runner.h"
#include "logger.h"
#include "config_file.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using pudimnetmon::AgentService;
using pudimnetmon::HeartbeatRequest;
using pudimnetmon::HeartbeatResponse;
using pudimnetmon::MetricsBatch;
using pudimagent::MetricsClient;
using pudimagent::ProbeConfig;

static std::atomic<bool> s_running{true};
static std::string s_node_id;
static std::string s_diagnostic_endpoint;

// Map a --log-level string to a LogLevel; returns false if unrecognized.
static bool ParseLogLevel(const std::string &s, logger::LogLevel &out) {
    if (s == "debug") { out = logger::LogLevel::Debug; return true; }
    if (s == "info") { out = logger::LogLevel::Info; return true; }
    if (s == "warn") { out = logger::LogLevel::Warn; return true; }
    if (s == "error") { out = logger::LogLevel::Error; return true; }
    return false;
}


// --------------------------------------------
// Signal handler
// --------------------------------------------
#ifndef _WIN32
// Async-signal-safe raw write (no mutex, no iostream) so the handler never
// deadlocks against a log line being emitted by another thread.
static void SafeWriteStr(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    while (n > 0) {
        ssize_t w = ::write(STDOUT_FILENO, s, n);
        if (w <= 0) break;
        s += w;
        n -= static_cast<size_t>(w);
    }
}
#endif

static void handle_signal(int sig) {
#ifndef _WIN32
    if (sig == SIGHUP) {
        SafeWriteStr(
            "{\"level\":\"info\",\"component\":\"agent\",\"message\":\"SIGHUP "
            "received: config reload not yet implemented (config is passed via "
            "CLI flags); ignoring\"}\n");
        return;
    }
    SafeWriteStr("{\"level\":\"info\",\"component\":\"agent\",\"message\":"
                 "\"Received signal, shutting down...\"}\n");
#else
    const char *sig_name = (sig == SIGTERM) ? "SIGTERM" :
                           (sig == SIGINT)  ? "SIGINT" : "UNKNOWN";
    logger::write(logger::LogLevel::Info,
                  std::string("Received ") + sig_name + ", shutting down...");
#endif
    s_running = false;
}

// --------------------------------------------
// Heartbeat client
// --------------------------------------------
class HeartbeatClient {
public:
    HeartbeatClient(const std::string &endpoint,
                    std::shared_ptr<grpc::ChannelCredentials> creds = nullptr)
        : m_stub(AgentService::NewStub(grpc::CreateChannel(
              endpoint,
              creds ? creds : grpc::InsecureChannelCredentials()))) {
        LOG_INFO("gRPC channel created to " + endpoint);
    }

    bool SendHeartbeat(int interval_ms, const std::string &version,
                       const std::string &traceparent = "") {
        HeartbeatRequest req;
        req.set_agent_id(s_node_id);
        req.set_timestamp_unix_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        req.set_interval_ms(interval_ms);
        req.set_version(version);
        if (!s_diagnostic_endpoint.empty()) {
            req.set_diagnostic_endpoint(s_diagnostic_endpoint);
        }

        HeartbeatResponse resp;
        ClientContext ctx;
        if (!traceparent.empty()) {
            ctx.AddMetadata("traceparent", traceparent);
        }

        // Set a deadline for the RPC
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(10);
        ctx.set_deadline(deadline);

        Status status = m_stub->SendHeartbeat(&ctx, req, &resp);

        if (status.ok()) {
            LOG_INFO("Heartbeat ACK received from collector (ack=" +
                     std::to_string(resp.ack()) + ")");
            return true;
        } else {
            LOG_WARN("Heartbeat failed: " + status.error_message() +
                     " (code=" + std::to_string(status.error_code()) + ")");
            return false;
        }
    }

private:
    std::unique_ptr<AgentService::Stub> m_stub;
};

// --------------------------------------------
// Main
// --------------------------------------------
int RunAgent(int argc, char **argv) {
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
            return 1;
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

    auto parse_list = [](const std::string &s) {
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
    };

    // Apply list-valued options from the config file (CLI flags override below).
    dns_targets = parse_list(get_s("dns-targets", ""));
    tcp_targets = parse_list(get_s("tcp-targets", ""));
    tls_targets = parse_list(get_s("tls-targets", ""));
    http_targets = parse_list(get_s("http-targets", ""));
    ping_targets = parse_list(get_s("ping-targets", ""));
    http_protocols = parse_list(get_s("http-protocols", ""));

    auto apply_dns_expected = [&](const std::string &s) {
        // Format: host=TYPE:value,host2=TYPE:value
        for (auto &entry : parse_list(s)) {
            auto eq = entry.find('=');
            if (eq == std::string::npos) continue;
            std::string host = entry.substr(0, eq);
            std::string rec = entry.substr(eq + 1);
            if (!host.empty() && !rec.empty()) {
                dns_expected[host].push_back(rec);
            }
        }
    };
    apply_dns_expected(get_s("dns-expected", ""));

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
            return 1;
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
            case 'd': dns_targets = parse_list(optarg); break;
            case 'p': tcp_targets = parse_list(optarg); break;
            case 's': tls_targets = parse_list(optarg); break;
            case 'w': http_targets = parse_list(optarg); break;
            case 'g': ping_targets = parse_list(optarg); break;
            case 'k': ping_count = std::stoi(optarg); break;
            case 'u': ping_gap_ms = std::stoi(optarg); break;
            case 'm': use_stream_metrics = true; break;
            case 'q': tls_cert_check = false; break;
            case 'r': tcp_retransmit_check = false; break;
            case 'e': tcp_handshake_capture = false; break;
            case 1001: tcp_handshake_interval_ms = std::stoi(optarg); break;
            case 1000: log_level_str = optarg; break;
            case 'x': http_protocols = parse_list(optarg); break;
            case 'y': apply_dns_expected(optarg); break;
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
                return 0;
            default:
                std::cerr << "Unknown option. Use --help for usage.\n";
                return 1;
        }
    }

    // Set globals
    s_node_id = node_id;
    s_diagnostic_endpoint = diagnostic_address;

    // Log context for the shared logger (embedded in every log line).
    logger::SetNodeId(node_id);
    logger::SetTraceId(trace_id);

    // Configure the NTP offset probe (used by the SNTP client on Windows).
    pudimagent::SetNtpServer(ntp_server);

    // Apply the log verbosity (unrecognized values are rejected).
    logger::LogLevel parsed_level;
    if (!ParseLogLevel(log_level_str, parsed_level)) {
        std::cerr << "Invalid --log-level '" << log_level_str
                  << "'; expected debug|info|warn|error\n";
        return 1;
    }
    logger::SetLevel(parsed_level);

    // Setup signal handlers
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    LOG_INFO("Agent starting up");
    LOG_INFO(cfg_loaded ? ("Loaded config file: " + config_path)
                        : ("No config file at " + config_path +
                           "; using built-in defaults and CLI flags"));
    // Layered-config audit: log every config-file value that survived, and call
    // out any CLI flag that overrides the file, so the effective configuration
    // (and where each value came from) is traceable in the logs.
    for (const auto &kv : file_cfg) {
        auto cli = cli_options.find(kv.first);
        if (cli != cli_options.end() && cli->second != kv.second) {
            LOG_INFO("CLI --" + kv.first +
                     (cli->second.empty() ? "" : ("=" + cli->second)) +
                     " overrides config file value '" + kv.second + "'");
        } else if (cli == cli_options.end()) {
            LOG_DEBUG("config file: " + kv.first + "=" + kv.second);
        }
    }
    for (const auto &kv : cli_options) {
        if (!file_cfg.count(kv.first)) {
            LOG_DEBUG("CLI --" + kv.first +
                      (kv.second.empty() ? "" : ("=" + kv.second)) +
                      " (not set in config file)");
        }
    }
    LOG_INFO("Collector endpoint: " + collector_endpoint);
    LOG_INFO("Node ID: " + node_id);
    LOG_INFO("Interval: " + std::to_string(interval_ms) + "ms");
    LOG_INFO("Version: " + version);

    // Start the diagnostic gRPC server (collector-triggered traceroute/pcap +
    // Phase 8 runtime reconfiguration).
    // Secured with mTLS when --tls-* flags are provided (the agent's own cert
    // acts as the server cert here; the collector presents its client cert).
    auto diag_server_creds =
        pudimagent::MakeServerCredentials(tls_ca, tls_cert, tls_key);
    auto probe_store = std::make_shared<pudimagent::ProbeConfigStore>();
    std::thread diagnostic_thread(
        [diagnostic_port, diag_server_creds, probe_store]() {
            pudimagent::DiagnosticServiceImpl diag_service(probe_store);
            grpc::ServerBuilder builder;
            builder.AddListeningPort("0.0.0.0:" + diagnostic_port,
                                     diag_server_creds);
            builder.RegisterService(&diag_service);
            auto server = builder.BuildAndStart();
            if (!server) {
                std::cerr << "Failed to start diagnostic server on port "
                          << diagnostic_port << "\n";
                return;
            }
            LOG_INFO("Diagnostic gRPC server listening on port " + diagnostic_port);
            server->Wait();
        });
    diagnostic_thread.detach();

    // Phase 5 service discovery: --collector-endpoints is a comma-separated
    // failover list; --collector-endpoint (single) is kept as a fallback.
    std::vector<std::string> endpoints;
    if (!collector_endpoints_input.empty()) {
        endpoints = parse_list(collector_endpoints_input);
    } else {
        endpoints.push_back(collector_endpoint);
    }
    pudimagent::FailoverClient failover(endpoints);

    // Phase 8 (Security): mutual TLS between agent and collector.
    auto creds = pudimagent::MakeChannelCredentials(tls_ca, tls_cert, tls_key);
    LOG_INFO(tls_ca.empty() ? "gRPC transport: insecure (no --tls-*)"
                            : "gRPC transport: mTLS (client cert " + tls_cert + ")");

    auto reconnect = [&]() {
        const std::string &ep = failover.CurrentEndpoint();
        LOG_INFO("Connecting to collector endpoint: " + ep);
        return std::make_pair(std::make_unique<HeartbeatClient>(ep, creds),
                              std::make_unique<MetricsClient>(ep, creds));
    };
    auto clients = reconnect();

    // Phase 5 daemon hardening: notify systemd (Type=notify) and ping watchdog.
#ifndef _WIN32
    std::signal(SIGHUP, handle_signal);
#endif
    pudimagent::NotifyReady();
    bool watchdog_stop = false;
    pudimagent::StartWatchdogThread(&watchdog_stop);

    std::vector<std::string> probe_targets;
    for (const auto *vec : {&dns_targets, &tcp_targets, &tls_targets,
                            &http_targets, &ping_targets}) {
        probe_targets.insert(probe_targets.end(), vec->begin(), vec->end());
    }

    if (probe_targets.empty()) {
        // Default demo targets so a bare `pudim-agent` produces useful output
        dns_targets = {"example.com"};
        tcp_targets = {"example.com:443"};
        tls_targets = {"example.com:443"};
        http_targets = {"https://example.com"};
        ping_targets = {"1.1.1.1"};
    }

    ProbeConfig probe_cfg;
    probe_cfg.dns_targets = dns_targets;
    probe_cfg.tcp_targets = tcp_targets;
    probe_cfg.tls_targets = tls_targets;
    probe_cfg.http_targets = http_targets;
    probe_cfg.ping_targets = ping_targets;
    probe_cfg.ping_count = ping_count;
    probe_cfg.ping_gap_ms = ping_gap_ms;
    // Phase 4 deep diagnostics
    probe_cfg.tls_cert_check = tls_cert_check;
    probe_cfg.tcp_retransmit_check = tcp_retransmit_check;
    probe_cfg.tcp_handshake_capture = tcp_handshake_capture;
    probe_cfg.http_protocols = http_protocols;
    probe_cfg.dns_expected = dns_expected;

    // Phase 8: seed the runtime config store; the Reconfigure RPC and the
    // metric loop both share this.
    probe_store->Set(probe_cfg);

    LOG_INFO("Running metrics probes every " + std::to_string(interval_ms) + "ms");

    // Phase 7 DR: persistent disk buffer (SQLite). Metrics overflowed from the
    // in-memory buffer are stored here and drained on reconnect, surviving
    // agent restarts and extended collector downtime.
    pudimagent::DiskBuffer disk_buffer(disk_buffer_path,
        static_cast<uint64_t>(disk_buffer_max_mb) * 1024 * 1024);
    std::string db_err;
    if (disk_buffer.Open(db_err)) {
        LOG_INFO("Disk buffer ready at " + disk_buffer_path +
                 " (pending=" + std::to_string(disk_buffer.Size()) + ")");
    } else {
        LOG_WARN("Disk buffer unavailable: " + db_err +
                 " (in-memory buffering only)");
    }

    // Main loop
    uint64_t buffer_drops = 0;
    uint64_t disk_spills = 0;
    uint64_t disk_drained_total = 0;
    int current_interval_ms = interval_ms;
    bool backpressure = false;

    // Bounded in-memory retry buffer (Phase 6 overload handling): oldest
    // dropped first when full; sends drain the buffer FIFO so failed
    // deliveries are retried rather than silently lost.
    std::deque<MetricsBatch> buffer;

    // Dedicated probe worker thread. Probes run off the main loop so a slow or
    // hung probe (blackholed target, slow resolver) no longer delays
    // heartbeats or stalls the sender.
    pudimagent::ProbeRunner probe_runner;
    probe_runner.SetConfigStore(probe_store);
    probe_runner.SetIntervalMs(interval_ms);
    if (tcp_handshake_interval_ms > 0) {
        probe_runner.SetHandshakeIntervalMs(tcp_handshake_interval_ms);
    }
    probe_runner.Start(node_id);

    auto last_heartbeat =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(interval_ms);
    auto last_send_attempt =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(interval_ms);
    auto last_self_report = std::chrono::steady_clock::now();

    while (s_running) {
        auto now = std::chrono::steady_clock::now();

        // Heartbeat on its own cadence, decoupled from probe latency.
        if (now - last_heartbeat >=
            std::chrono::milliseconds(current_interval_ms)) {
            last_heartbeat = now;
            std::string traceparent = pudimagent::GenerateTraceParent();
            bool hb_ok =
                clients.first->SendHeartbeat(interval_ms, version, traceparent);
            if (hb_ok) {
                failover.OnSendSuccess();
            } else if (failover.OnSendFailure()) {
                LOG_WARN("Heartbeat failed; failing over to " +
                         failover.CurrentEndpoint());
                clients = reconnect();
            }
        }

        // Drain freshly produced batches into the retry buffer.
        std::vector<MetricsBatch> fresh;
        probe_runner.Drain(fresh, 50);
        for (auto &b : fresh) {
            buffer.push_back(std::move(b));
            while (static_cast<int>(buffer.size()) > max_buffer_size) {
                MetricsBatch dropped = std::move(buffer.front());
                buffer.pop_front();
                std::string blob;
                if (disk_buffer.Available() && dropped.SerializeToString(&blob) &&
                    disk_buffer.Push(blob)) {
                    disk_spills++;
                    LOG_WARN("Buffer full; spilled oldest batch to disk buffer (spilled=" +
                             std::to_string(disk_spills) + ", pending=" +
                             std::to_string(disk_buffer.Size()) + ")");
                } else {
                    buffer_drops++;
                    LOG_WARN("Buffer full; dropped oldest metric batch (drops=" +
                             std::to_string(buffer_drops) + ")");
                }
            }
        }

        // Send policy: send when new batches arrived, or retry a failed send
        // at most once per interval.
        bool should_send = false;
        if (!buffer.empty()) {
            if (!fresh.empty()) {
                should_send = true;
            } else if (now - last_send_attempt >=
                       std::chrono::milliseconds(current_interval_ms)) {
                should_send = true;
            }
        }

        if (should_send) {
            last_send_attempt = now;
            std::string traceparent = pudimagent::GenerateTraceParent();
            MetricsBatch &to_send = buffer.front();
            LOG_INFO("Collected " + std::to_string(to_send.metrics_size()) +
                     " metrics, sending to collector");
            bool ok = use_stream_metrics
                          ? clients.second->StreamMetrics(node_id,
                                                          to_send.metrics(),
                                                          traceparent)
                          : clients.second->SendBatch(to_send, traceparent);
            if (ok) {
                buffer.pop_front();
                LOG_INFO("Metrics batch accepted by collector");
                failover.OnSendSuccess();
                backpressure = clients.second->BackpressureSignalled();

                // Phase 7: drain persisted batches from the disk buffer
                // (bounded work per pass; oldest-first).
                for (int i = 0; i < 10 && disk_buffer.Size() > 0; i++) {
                    std::vector<std::string> blobs;
                    disk_buffer.Peek(blobs, 1);
                    if (blobs.empty()) break;
                    MetricsBatch pb;
                    if (pb.ParseFromString(blobs[0])) {
                        bool d_ok =
                            use_stream_metrics
                                ? clients.second->StreamMetrics(pb.agent_id(),
                                                                pb.metrics(),
                                                                traceparent)
                                : clients.second->SendBatch(pb, traceparent);
                        if (!d_ok) break;  // stop draining; retry next pass
                    }
                    disk_buffer.Pop(1);
                    disk_drained_total++;
                }
                if (disk_drained_total > 0) {
                    LOG_INFO("Drained " +
                             std::to_string(disk_drained_total) +
                             " persisted batches from disk buffer (pending=" +
                             std::to_string(disk_buffer.Size()) + ")");
                }
            } else {
                LOG_WARN("Metrics batch rejected or send failed; keeping it buffered");
                if (failover.OnSendFailure()) {
                    LOG_WARN("Failing over to " + failover.CurrentEndpoint());
                    clients = reconnect();
                }
            }
        }

        // Adaptive interval: back off (double, cap 10x) while the collector
        // signals overload; restore once it clears. The probe worker picks the
        // change up on its next cycle.
        if (backpressure) {
            current_interval_ms = std::min(current_interval_ms * 2,
                                           interval_ms * 10);
            LOG_WARN("Collector signalled overload; backing off to " +
                     std::to_string(current_interval_ms) + "ms");
        } else if (current_interval_ms != interval_ms) {
            current_interval_ms = interval_ms;
            LOG_INFO("Backpressure cleared; interval restored to " +
                     std::to_string(interval_ms) + "ms");
        }
        probe_runner.SetIntervalMs(current_interval_ms);

        // Self-observability: report the probe worker's cycle stats once per
        // minute so agent overhead is observable in the logs.
        if (now - last_self_report >= std::chrono::seconds(60)) {
            last_self_report = now;
            auto st = probe_runner.GetStats();
            LOG_INFO("self: cycle=" + std::to_string(st.cycle_duration_ms) +
                     "ms probes=" + std::to_string(st.probe_duration_ms) +
                     "ms metrics=" + std::to_string(st.metric_count) +
                     " queued=" + std::to_string(probe_runner.PendingCount()) +
                     " handshake_runs=" +
                     std::to_string(st.handshake_probe_count));
        }

        // Short responsive sleep; the probe cadence lives in the worker thread.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Stop the probe worker (drains/joins the pool) before reporting shutdown.
    probe_runner.Stop();

    watchdog_stop = true;
    if (disk_buffer.Size() > 0) {
        LOG_WARN(std::to_string(disk_buffer.Size()) +
                 " batches remain in disk buffer; will be retried on next start");
    }
    LOG_INFO("Agent shut down gracefully");
    return 0;
}

// --------------------------------------------
// Entry point: console application or Windows service
// --------------------------------------------
int main(int argc, char **argv) {
#ifdef _WIN32
    // Service lifecycle helpers never start the agent loop.
    if (pudimagent::platform::WantsInstallService(argc, argv)) {
        std::wstring args;
        bool first = true;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--install-service") == 0) continue;
            if (!first) args += L" ";
            first = false;
            args += pudimagent::platform::Utf8ToWide(argv[i]);
        }
        return pudimagent::platform::InstallAgentService(args) ? 0 : 1;
    }
    if (pudimagent::platform::WantsUninstallService(argc, argv)) {
        return pudimagent::platform::UninstallAgentService() ? 0 : 1;
    }

    std::string net_err;
    if (!pudimagent::platform::InitNetwork(net_err)) {
        std::cerr << net_err << "\n";
        return 1;
    }

    // If the Service Control Manager launched us, the dispatcher below runs
    // the whole agent lifecycle inside ServiceMain and returns only after the
    // service has stopped. Otherwise fall through to console mode.
    if (pudimagent::platform::TryRunAsService(
            argc, argv,
            [](int ac, char **av) { return RunAgent(ac, av); },
            []() { s_running = false; })) {
        pudimagent::platform::CleanupNetwork();
        return 0;
    }

    int rc = RunAgent(argc, argv);
    pudimagent::platform::CleanupNetwork();
    return rc;
#else
    return RunAgent(argc, argv);
#endif
}
