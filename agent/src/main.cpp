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
#include <getopt.h>

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

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using pudimnetmon::AgentService;
using pudimnetmon::HeartbeatRequest;
using pudimnetmon::HeartbeatResponse;
using pudimnetmon::MetricsBatch;
using pudimagent::MetricsClient;
using pudimagent::ProbeConfig;

// --------------------------------------------
// Globals (must be before logger macros)
// --------------------------------------------
static std::atomic<bool> s_running{true};
static std::string s_node_id;
static std::string s_trace_id;
static std::string s_diagnostic_endpoint;

// --------------------------------------------
// Logger: simple JSON-structured logging to stdout
// --------------------------------------------
namespace logger {

static inline std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static inline void write(const std::string &level, const std::string &message,
                         const std::string &agent_id,
                         const std::string &trace_id) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    std::cout << "{"
              << "\"timestamp\":" << now << ","
              << "\"level\":\"" << level << "\","
              << "\"component\":\"agent\","
              << "\"message\":\"" << escape(message) << "\"";
    if (!agent_id.empty()) {
        std::cout << ",\"agent_id\":\"" << escape(agent_id) << "\"";
    }
    if (!trace_id.empty()) {
        std::cout << ",\"trace_id\":\"" << escape(trace_id) << "\"";
    }
    std::cout << "}" << std::endl;
}

} // namespace logger

#define LOG_INFO(msg)    logger::write("info", msg, s_node_id, s_trace_id)
#define LOG_WARN(msg)    logger::write("warn", msg, s_node_id, s_trace_id)
#define LOG_ERROR(msg)   logger::write("error", msg, s_node_id, s_trace_id)
#define LOG_DEBUG(msg)   logger::write("debug", msg, s_node_id, s_trace_id)

// --------------------------------------------
// Signal handler
// --------------------------------------------
static void handle_signal(int sig) {
    const char *sig_name = (sig == SIGTERM) ? "SIGTERM" :
                           (sig == SIGINT)  ? "SIGINT" :
                           (sig == SIGHUP)  ? "SIGHUP" : "UNKNOWN";
    if (sig == SIGHUP) {
        logger::write("info",
                      "SIGHUP received: config reload not yet implemented (config "
                      "is passed via CLI flags); ignoring",
                      s_node_id, s_trace_id);
        return;
    }
    logger::write("info", std::string("Received ") + sig_name + ", shutting down...",
               s_node_id, "");
    s_running = false;
}

// --------------------------------------------
// Heartbeat client
// --------------------------------------------
class HeartbeatClient {
public:
    HeartbeatClient(const std::string &endpoint)
        : m_stub(AgentService::NewStub(
              grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()))) {
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
int main(int argc, char **argv) {
    // Defaults
    std::string collector_endpoint = "localhost:50051";
    std::string node_id = "agent-unknown";
    std::string trace_id = "";
    int interval_ms = 5000;
    std::string version = "0.1.0";
    bool use_stream_metrics = false;

    // Probe targets (comma-separated lists)
    std::vector<std::string> dns_targets;
    std::vector<std::string> tcp_targets;
    std::vector<std::string> tls_targets;
    std::vector<std::string> http_targets;
    std::vector<std::string> ping_targets;
    int ping_count = 4;

    // Phase 4 deep-diagnostics configuration
    bool tls_cert_check = true;
    bool tcp_retransmit_check = true;
    bool tcp_handshake_capture = true;
    std::vector<std::string> http_protocols;
    std::map<std::string, std::vector<std::string>> dns_expected;
    std::string diagnostic_port = "50052";
    std::string diagnostic_address;  // e.g. "agent.example.com:50052"; empty = not advertised
    std::string collector_endpoints_input;  // comma-separated failover list
    int max_buffer_size = 200;  // in-memory batch buffer cap (overload handling)
    std::string disk_buffer_path = "/var/lib/pudim/pending.db";
    int disk_buffer_max_mb = 100;

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
        {"stream-metrics",     no_argument,       nullptr, 'm'},
        {"no-tls-cert",        no_argument,       nullptr, 'q'},
        {"no-tcp-retransmit",  no_argument,       nullptr, 'r'},
        {"no-tcp-handshake",   no_argument,       nullptr, 'e'},
        {"http-protocols",     required_argument, nullptr, 'x'},
        {"dns-expected",       required_argument, nullptr, 'y'},
        {"diagnostic-port",    required_argument, nullptr, 'z'},
        {"diagnostic-address", required_argument, nullptr, 'a'},
        {"collector-endpoints", required_argument, nullptr, 'b'},
        {"max-buffer-size",     required_argument, nullptr, 'f'},
        {"disk-buffer-path",    required_argument, nullptr, 'j'},
        {"disk-buffer-max-mb",  required_argument, nullptr, 'l'},
        {"help",               no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "c:n:i:t:v:d:p:s:w:g:k:m:qrex:y:z:a:b:f:j:l:h", long_options, &option_index)) != -1) {
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
            case 'm': use_stream_metrics = true; break;
            case 'q': tls_cert_check = false; break;
            case 'r': tcp_retransmit_check = false; break;
            case 'e': tcp_handshake_capture = false; break;
            case 'x': http_protocols = parse_list(optarg); break;
            case 'y': {
                // Format: host=TYPE:value,host2=TYPE:value
                for (auto &entry : parse_list(optarg)) {
                    auto eq = entry.find('=');
                    if (eq == std::string::npos) continue;
                    std::string host = entry.substr(0, eq);
                    std::string rec = entry.substr(eq + 1);
                    if (!host.empty() && !rec.empty()) {
                        dns_expected[host].push_back(rec);
                    }
                }
                break;
            }
            case 'z': diagnostic_port = optarg; break;
            case 'a': diagnostic_address = optarg; break;
            case 'b': collector_endpoints_input = optarg; break;
            case 'f': max_buffer_size = std::stoi(optarg); break;
            case 'j': disk_buffer_path = optarg; break;
            case 'l': disk_buffer_max_mb = std::stoi(optarg); break;
            case 'h':
                std::cout << "Usage: pudim-agent [options]\n"
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
                          << "  -m, --stream-metrics      Use client-streaming RPC for metrics (default: unary)\n"
                          << "      --no-tls-cert         Disable TLS certificate validation probe\n"
                          << "      --no-tcp-retransmit   Disable TCP retransmission probe\n"
                          << "      --no-tcp-handshake    Disable TCP handshake capture (libpcap)\n"
                          << "  -x, --http-protocols      HTTP versions to measure: http1.1,http2,http3\n"
                          << "  -y, --dns-expected        Expected DNS records: host=A:1.2.3.4,host2=CNAME:x\n"
                          << "  -z, --diagnostic-port     gRPC diagnostic server port (default: 50052)\n"
                          << "  -a, --diagnostic-address  Advertised diagnostic endpoint, e.g. agent.example.com:50052\n"
                          << "  -b, --collector-endpoints Comma-separated collector endpoints for failover\n"
                          << "  -f, --max-buffer-size     Max queued metric batches before dropping (default: 200)\n"
                          << "  -j, --disk-buffer-path    SQLite path for persistent buffering (default: /var/lib/pudim/pending.db)\n"
                          << "  -l, --disk-buffer-max-mb  Max disk buffer size in MB (default: 100)\n"
                          << "  -h, --help                Show this help\n";
                return 0;
            default:
                std::cerr << "Unknown option. Use --help for usage.\n";
                return 1;
        }
    }

    // Set globals
    s_node_id = node_id;
    s_trace_id = trace_id;
    s_diagnostic_endpoint = diagnostic_address;

    // Setup signal handlers
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    LOG_INFO("Agent starting up");
    LOG_INFO("Collector endpoint: " + collector_endpoint);
    LOG_INFO("Node ID: " + node_id);
    LOG_INFO("Interval: " + std::to_string(interval_ms) + "ms");
    LOG_INFO("Version: " + version);

    // Start the diagnostic gRPC server (collector-triggered traceroute/pcap)
    std::thread diagnostic_thread([diagnostic_port]() {
        pudimagent::DiagnosticServiceImpl diag_service;
        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:" + diagnostic_port,
                                 grpc::InsecureServerCredentials());
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

    auto reconnect = [&]() {
        const std::string &ep = failover.CurrentEndpoint();
        LOG_INFO("Connecting to collector endpoint: " + ep);
        return std::make_pair(std::make_unique<HeartbeatClient>(ep),
                              std::make_unique<MetricsClient>(ep));
    };
    auto clients = reconnect();

    // Phase 5 daemon hardening: notify systemd (Type=notify) and ping watchdog.
    std::signal(SIGHUP, handle_signal);
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
    // Phase 4 deep diagnostics
    probe_cfg.tls_cert_check = tls_cert_check;
    probe_cfg.tcp_retransmit_check = tcp_retransmit_check;
    probe_cfg.tcp_handshake_capture = tcp_handshake_capture;
    probe_cfg.http_protocols = http_protocols;
    probe_cfg.dns_expected = dns_expected;

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
    uint64_t seq = 0;
    uint64_t buffer_drops = 0;
    uint64_t disk_spills = 0;
    uint64_t disk_drained = 0;
    int current_interval_ms = interval_ms;
    bool backpressure = false;
    // Bounded in-memory buffer (Phase 6 overload handling): oldest dropped
    // first when full; sends drain the buffer FIFO so failed deliveries are
    // retried rather than silently lost.
    std::deque<MetricsBatch> buffer;

    while (s_running) {
        // Phase 6: one W3C traceparent per cycle, shared by the heartbeat and
        // the metric batch so the collector/consumers can correlate them.
        std::string traceparent = pudimagent::GenerateTraceParent();

        bool hb_ok = clients.first->SendHeartbeat(interval_ms, version, traceparent);
        if (hb_ok) {
            failover.OnSendSuccess();
        } else if (failover.OnSendFailure()) {
            LOG_WARN("Heartbeat failed; failing over to " +
                     failover.CurrentEndpoint());
            clients = reconnect();
        }

        // Collect and send metrics
        std::vector<pudimnetmon::Metric> metrics;
        pudimagent::RunAllProbes(probe_cfg, metrics);

        MetricsBatch batch;
        batch.set_agent_id(node_id);
        batch.set_timestamp_unix_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());

        for (auto &m : metrics) {
            m.set_seq(seq++);
            *batch.add_metrics() = std::move(m);
        }

        // Buffer the batch; overflow spills to the disk buffer (Phase 7 DR).
        buffer.push_back(std::move(batch));
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

        // Send the oldest buffered batch (FIFO drain).
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

            // Phase 7: drain persisted batches from the disk buffer (bounded
            // work per cycle; oldest-first).
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
                    if (!d_ok) break;  // stop draining; retry next cycle
                }
                disk_buffer.Pop(1);
                disk_drained++;
            }
            if (disk_drained > 0) {
                LOG_INFO("Drained " + std::to_string(disk_drained) +
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

        // Adaptive interval: back off (double, cap 10x) while the collector
        // signals overload; restore once it clears.
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

        // Sleep in small increments so we can respond to signals promptly
        int slept = 0;
        while (slept < current_interval_ms && s_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            slept += 100;
        }
    }

    watchdog_stop = true;
    if (disk_buffer.Size() > 0) {
        LOG_WARN(std::to_string(disk_buffer.Size()) +
                 " batches remain in disk buffer; will be retried on next start");
    }
    LOG_INFO("Agent shut down gracefully");
    return 0;
}