#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <cstdlib>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include <httplib.h>

#include "heartbeat.grpc.pb.h"
#include "metrics.grpc.pb.h"
#include "diagnostic.grpc.pb.h"
#include "metrics_service.h"
#include "storage/timescale_storage.h"
#include "alerting/alert_manager.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using pudimnetmon::AgentService;
using pudimnetmon::HeartbeatRequest;
using pudimnetmon::HeartbeatResponse;

// --------------------------------------------
// Logger: JSON-structured logging to stdout
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

static inline void emit(const std::string &level, const std::string &message,
                        const std::string &agent_id = "",
                        const std::string &trace_id = "") {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    std::cout << "{"
              << "\"timestamp\":" << now << ","
              << "\"level\":\"" << level << "\","
              << "\"component\":\"collector\","
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

// --------------------------------------------
// Globals
// --------------------------------------------
static std::atomic<bool> s_running{true};

static void handle_signal(int sig) {
    const char *sig_name = (sig == SIGTERM) ? "SIGTERM" :
                           (sig == SIGINT)  ? "SIGINT" : "UNKNOWN";
    logger::emit("info", std::string("Received ") + sig_name + ", shutting down...");
    s_running = false;
}

// --------------------------------------------
// In-memory agent registry
// --------------------------------------------
struct AgentEntry {
    std::string agent_id;
    int64_t last_seen_unix_ms;
    int32_t interval_ms;
    std::string version;
    int64_t first_seen_unix_ms;
    std::string diagnostic_endpoint;  // host:port of the agent's diagnostic server
};

class AgentRegistry {
public:
    void RecordHeartbeat(const HeartbeatRequest &req) {
        std::unique_lock lock(m_mutex);
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        auto it = m_agents.find(req.agent_id());
        if (it == m_agents.end()) {
            AgentEntry entry;
            entry.agent_id = req.agent_id();
            entry.last_seen_unix_ms = now;
            entry.interval_ms = req.interval_ms();
            entry.version = req.version();
            entry.first_seen_unix_ms = now;
            entry.diagnostic_endpoint = req.diagnostic_endpoint();
            m_agents[req.agent_id()] = entry;
            logger::emit("info", "New agent registered", req.agent_id());
        } else {
            it->second.last_seen_unix_ms = now;
            it->second.interval_ms = req.interval_ms();
            it->second.version = req.version();
            it->second.diagnostic_endpoint = req.diagnostic_endpoint();
        }
        m_heartbeat_count++;
    }

    size_t ActiveAgentCount(int64_t timeout_ms = 30000) const {
        std::shared_lock lock(m_mutex);
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        size_t count = 0;
        for (const auto &[id, entry] : m_agents) {
            if ((now - entry.last_seen_unix_ms) < timeout_ms) {
                count++;
            }
        }
        return count;
    }

    // Returns the agent's advertised diagnostic endpoint, or "" if unknown.
    std::string GetDiagnosticEndpoint(const std::string &agent_id) const {
        std::shared_lock lock(m_mutex);
        auto it = m_agents.find(agent_id);
        return (it != m_agents.end()) ? it->second.diagnostic_endpoint : "";
    }

    size_t TotalAgentCount() const {
        std::shared_lock lock(m_mutex);
        return m_agents.size();
    }

    uint64_t HeartbeatCount() const {
        std::shared_lock lock(m_mutex);
        return m_heartbeat_count;
    }

    std::string DumpAgents() const {
        std::shared_lock lock(m_mutex);
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        std::string json = "{\"agents\":[";
        bool first = true;
        for (const auto &[id, entry] : m_agents) {
            if (!first) json += ",";
            first = false;
            bool alive = (now - entry.last_seen_unix_ms) < 30000;
            json += "{";
            json += "\"agent_id\":\"" + escape(entry.agent_id) + "\",";
            json += "\"last_seen_unix_ms\":" + std::to_string(entry.last_seen_unix_ms) + ",";
            json += "\"interval_ms\":" + std::to_string(entry.interval_ms) + ",";
            json += "\"version\":\"" + escape(entry.version) + "\",";
            json += "\"diagnostic_endpoint\":\"" + escape(entry.diagnostic_endpoint) + "\",";
            json += "\"first_seen_unix_ms\":" + std::to_string(entry.first_seen_unix_ms) + ",";
            json += "\"alive\":" + std::string(alive ? "true" : "false");
            json += "}";
        }
        json += "]}";
        return json;
    }

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, AgentEntry> m_agents;
    std::atomic<uint64_t> m_heartbeat_count{0};

    static std::string escape(const std::string &s) {
        return logger::escape(s);
    }
};

static AgentRegistry s_registry;

// Storage + metrics service (initialized in main)
static std::shared_ptr<pudimcollector::TimescaleStorage> s_storage;
static std::shared_ptr<pudimcollector::MetricsServiceImpl> s_metrics_service;
static std::shared_ptr<pudimcollector::alerting::AlertManager> s_alert_manager;
static std::shared_ptr<pudimcollector::kafka::KafkaProducer> s_kafka_producer;

// --------------------------------------------
// gRPC service implementation
// --------------------------------------------
class AgentServiceImpl final : public AgentService::Service {
public:
    Status SendHeartbeat(ServerContext *ctx,
                         const HeartbeatRequest *req,
                         HeartbeatResponse *resp) override {
        // Record the heartbeat
        s_registry.RecordHeartbeat(*req);

        logger::emit("info",
                  "Heartbeat received from " + req->agent_id(),
                  req->agent_id());

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

        resp->set_ack(true);
        resp->set_collector_time_unix_ms(now);
        resp->set_status_message("ok");

        return Status::OK;
    }
};

// --------------------------------------------
// Prometheus /metrics helper
// --------------------------------------------
static std::string format_prometheus_metrics() {
    auto hb_count = s_registry.HeartbeatCount();
    auto active_count = s_registry.ActiveAgentCount();
    auto total_count = s_registry.TotalAgentCount();

    std::string out;
    out += "# HELP pudim_heartbeats_received_total Total heartbeats received\n";
    out += "# TYPE pudim_heartbeats_received_total counter\n";
    out += "pudim_heartbeats_received_total " + std::to_string(hb_count) + "\n";
    out += "# HELP pudim_agents_active Current number of active agents\n";
    out += "# TYPE pudim_agents_active gauge\n";
    out += "pudim_agents_active " + std::to_string(active_count) + "\n";
    out += "# HELP pudim_agents_registered Total registered agents\n";
    out += "# TYPE pudim_agents_registered gauge\n";
    out += "pudim_agents_registered " + std::to_string(total_count) + "\n";

    if (s_metrics_service) {
        out += "# HELP pudim_metrics_received_total Total metrics received\n";
        out += "# TYPE pudim_metrics_received_total counter\n";
        out += "pudim_metrics_received_total " +
               std::to_string(s_metrics_service->ReceivedMetrics()) + "\n";
        out += "# HELP pudim_metrics_batches_received_total Total metric batches received\n";
        out += "# TYPE pudim_metrics_batches_received_total counter\n";
        out += "pudim_metrics_batches_received_total " +
               std::to_string(s_metrics_service->BatchesReceived()) + "\n";
        out += "# HELP pudim_metrics_rejected_total Total metrics rejected\n";
        out += "# TYPE pudim_metrics_rejected_total counter\n";
        out += "pudim_metrics_rejected_total " +
               std::to_string(s_metrics_service->RejectedMetrics()) + "\n";
    }

    if (s_alert_manager) {
        out += "# HELP pudim_alerts_firing Currently firing alerts\n";
        out += "# TYPE pudim_alerts_firing gauge\n";
        out += "pudim_alerts_firing " +
               std::to_string(s_alert_manager->ActiveAlertCount()) + "\n";
        out += "# HELP pudim_alert_notifications_total Total alert notifications sent\n";
        out += "# TYPE pudim_alert_notifications_total counter\n";
        out += "pudim_alert_notifications_total " +
               std::to_string(s_alert_manager->TotalAlertsFired()) + "\n";
        out += "# HELP pudim_alert_rules_loaded Number of loaded alert rules\n";
        out += "# TYPE pudim_alert_rules_loaded gauge\n";
        out += "pudim_alert_rules_loaded " +
               std::to_string(s_alert_manager->RuleCount()) + "\n";
    }

    if (s_kafka_producer) {
        out += "# HELP pudim_kafka_produced_total Metrics batches produced to Kafka\n";
        out += "# TYPE pudim_kafka_produced_total counter\n";
        out += "pudim_kafka_produced_total " +
               std::to_string(s_kafka_producer->ProducedTotal()) + "\n";
        out += "# HELP pudim_kafka_delivery_failures_total Kafka delivery failures\n";
        out += "# TYPE pudim_kafka_delivery_failures_total counter\n";
        out += "pudim_kafka_delivery_failures_total " +
               std::to_string(s_kafka_producer->DeliveryFailures()) + "\n";
    }

    if (s_metrics_service) {
        out += "# HELP pudim_clock_skew_warnings_total Clock-skew warnings (Phase 5)\n";
        out += "# TYPE pudim_clock_skew_warnings_total counter\n";
        out += "pudim_clock_skew_warnings_total " +
               std::to_string(s_metrics_service->SkewWarnings()) + "\n";
    }

    if (s_storage) {
        auto stats = s_storage->GetStats();
        out += "# HELP pudim_storage_metrics_written_total Metrics written to storage\n";
        out += "# TYPE pudim_storage_metrics_written_total counter\n";
        out += "pudim_storage_metrics_written_total " +
               std::to_string(stats.metrics_written) + "\n";
        out += "# HELP pudim_storage_batches_written_total Batches written to storage\n";
        out += "# TYPE pudim_storage_batches_written_total counter\n";
        out += "pudim_storage_batches_written_total " +
               std::to_string(stats.batches_written) + "\n";
        out += "# HELP pudim_storage_errors_total Storage errors\n";
        out += "# TYPE pudim_storage_errors_total counter\n";
        out += "pudim_storage_errors_total " +
               std::to_string(stats.errors) + "\n";
        out += "# HELP pudim_storage_insert_latency_total_ms Cumulative storage insert latency\n";
        out += "# TYPE pudim_storage_insert_latency_total_ms counter\n";
        out += "pudim_storage_insert_latency_total_ms " +
               std::to_string(stats.insert_latency_total_ms) + "\n";
        out += "# HELP pudim_storage_healthy Storage health (1=ok, 0=unhealthy)\n";
        out += "# TYPE pudim_storage_healthy gauge\n";
        out += "pudim_storage_healthy " +
               std::string(s_storage->IsHealthy() ? "1" : "0") + "\n";
    }

    return out;
}

// --------------------------------------------
// Main
// --------------------------------------------
int main(int argc, char **argv) {
    std::string grpc_addr = "0.0.0.0:50051";
    std::string http_addr = "0.0.0.0:8080";
    std::string db_host = "localhost";
    int db_port = 5432;
    std::string db_name = "pudimnetmon";
    std::string db_user = "pudim";
    std::string db_password = "pudim";
    std::string alert_rules_path;
    std::string kafka_brokers;   // empty → Direct mode (Phases 1-2 behaviour)
    std::string kafka_topic = "network.metrics";
    int64_t skew_threshold_ms = 5000;  // clock-skew warning threshold (Phase 5)

    auto get_env = [](const char *name, const std::string &def) {
        const char *v = std::getenv(name);
        return v ? std::string(v) : def;
    };

    // Env overrides (Docker Compose friendly)
    db_host = get_env("PUDIM_DB_HOST", db_host);
    db_port = std::stoi(get_env("PUDIM_DB_PORT", std::to_string(db_port)));
    db_name = get_env("PUDIM_DB_NAME", db_name);
    db_user = get_env("PUDIM_DB_USER", db_user);
    db_password = get_env("PUDIM_DB_PASSWORD", db_password);

    // Simple CLI parsing. Supports both "--flag value" and "--flag=value".
    auto opt = [&](const std::string &arg, const std::string &flag,
                   int &i) -> std::string {
        const std::string prefix = flag + "=";
        if (arg.compare(0, prefix.size(), prefix) == 0) {
            return arg.substr(prefix.size());
        }
        if (arg == flag && i + 1 < argc) {
            return argv[++i];
        }
        return "";
    };

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string v;
        if ((v = opt(arg, "--grpc-addr", i)) != "") {
            grpc_addr = v;
        } else if ((v = opt(arg, "--http-addr", i)) != "") {
            http_addr = v;
        } else if ((v = opt(arg, "--db-host", i)) != "") {
            db_host = v;
        } else if ((v = opt(arg, "--db-port", i)) != "") {
            db_port = std::stoi(v);
        } else if ((v = opt(arg, "--db-name", i)) != "") {
            db_name = v;
        } else if ((v = opt(arg, "--db-user", i)) != "") {
            db_user = v;
        } else if ((v = opt(arg, "--db-password", i)) != "") {
            db_password = v;
        } else if ((v = opt(arg, "--alert-rules-path", i)) != "") {
            alert_rules_path = v;
        } else if ((v = opt(arg, "--kafka-brokers", i)) != "") {
            kafka_brokers = v;
        } else if ((v = opt(arg, "--kafka-topic", i)) != "") {
            kafka_topic = v;
        } else if ((v = opt(arg, "--skew-threshold-ms", i)) != "") {
            skew_threshold_ms = std::stoll(v);
        } else if (arg == "--help") {
            std::cout << "Usage: pudim-collector [options]\n"
                      << "  --grpc-addr         gRPC listen address (default: 0.0.0.0:50051)\n"
                      << "  --http-addr         HTTP listen address (default: 0.0.0.0:8080)\n"
                      << "  --db-host           PostgreSQL/TimescaleDB host (default: localhost)\n"
                      << "  --db-port           PostgreSQL/TimescaleDB port (default: 5432)\n"
                      << "  --db-name           Database name (default: pudimnetmon)\n"
                      << "  --db-user           Database user (default: pudim)\n"
                      << "  --db-password       Database password (default: pudim)\n"
                      << "  --alert-rules-path  JSON file with alert rules (default: none; disables alerting)\n"
                      << "  --kafka-brokers     Kafka bootstrap servers; when set, collector produces to\n"
                      << "                      Kafka and consumers own storage + alerting (default: empty)\n"
                      << "  --kafka-topic       Kafka topic (default: network.metrics)\n"
                      << "  --skew-threshold-ms Clock-skew warning threshold (default: 5000)\n"
                      << "  --help              Show this help\n";
            return 0;
        }
    }

    logger::emit("info", "Collector starting up");
    logger::emit("info", "gRPC endpoint: " + grpc_addr);
    logger::emit("info", "HTTP endpoint: " + http_addr);
    logger::emit("info", "DB endpoint: " + db_host + ":" + std::to_string(db_port) +
                 "/" + db_name);

    // Setup signal handlers
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    // Initialize storage
    pudimcollector::StorageConfig storage_cfg;
    storage_cfg.host = db_host;
    storage_cfg.port = db_port;
    storage_cfg.dbname = db_name;
    storage_cfg.user = db_user;
    storage_cfg.password = db_password;

    s_storage = std::make_shared<pudimcollector::TimescaleStorage>(storage_cfg);
    if (!s_storage->Connect()) {
        logger::emit("warn", "Storage connection failed; collector will run "
                     "without persistent storage (metrics will be rejected)");
    } else {
        logger::emit("info", "Storage connected (TimescaleDB)");
    }

    // Determine ingestion mode (ADR 004). Kafka mode is enabled by passing
    // --kafka-brokers; consumers then own storage + alerting.
    pudimcollector::StorageMode storage_mode = pudimcollector::StorageMode::Direct;
    if (!kafka_brokers.empty()) {
        storage_mode = pudimcollector::StorageMode::Kafka;
        s_kafka_producer = std::make_shared<pudimcollector::kafka::KafkaProducer>();
        std::string err;
        if (!s_kafka_producer->Connect(kafka_brokers, kafka_topic, err)) {
            logger::emit("error", "Kafka producer connection failed: " + err);
            return 1;
        }
        logger::emit("info", "Kafka mode enabled (topic=" + kafka_topic + ")");
    }

    // Initialize alert manager (optional; only used in Direct mode. In Kafka
    // mode the alert consumer owns alerting.)
    s_alert_manager = std::make_shared<pudimcollector::alerting::AlertManager>();
    if (storage_mode == pudimcollector::StorageMode::Direct &&
        !alert_rules_path.empty()) {
        std::string err;
        if (s_alert_manager->LoadRulesFromFile(alert_rules_path, err)) {
            logger::emit("info",
                         "Loaded " + std::to_string(s_alert_manager->RuleCount()) +
                         " alert rules from " + alert_rules_path);
        } else {
            logger::emit("warn", "Failed to load alert rules: " + err);
        }
    } else if (storage_mode == pudimcollector::StorageMode::Direct) {
        logger::emit("info",
                     "No alert rules configured (--alert-rules-path unset); alerting disabled");
    }

    s_metrics_service =
        std::make_shared<pudimcollector::MetricsServiceImpl>(s_storage,
                                                             s_alert_manager,
                                                             s_kafka_producer,
                                                             storage_mode,
                                                             skew_threshold_ms);

    // Start gRPC server
    AgentServiceImpl agent_service;
    ServerBuilder builder;
    builder.AddListeningPort(grpc_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&agent_service);
    builder.RegisterService(s_metrics_service.get());
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024); // 4MB

    std::unique_ptr<Server> grpc_server = builder.BuildAndStart();
    if (!grpc_server) {
        logger::emit("error", "Failed to start gRPC server on " + grpc_addr);
        return 1;
    }
    logger::emit("info", "gRPC server listening on " + grpc_addr);

    // Start HTTP server (health + metrics)
    httplib::Server http_server;

    http_server.Get("/health", [](const httplib::Request &, httplib::Response &resp) {
        bool db_ok = s_storage ? s_storage->IsHealthy() : false;
        std::string status = db_ok ? "ok" : "degraded";
        resp.set_content("{\"status\":\"" + status +
                         "\",\"component\":\"collector\",\"storage\":" +
                         std::string(db_ok ? "true" : "false") + "}",
                         "application/json");
    });

    http_server.Get("/agents", [](const httplib::Request &, httplib::Response &resp) {
        resp.set_content(s_registry.DumpAgents(), "application/json");
    });

    http_server.Get("/metrics", [](const httplib::Request &, httplib::Response &resp) {
        resp.set_content(format_prometheus_metrics(), "text/plain; version=0.0.4");
    });

    // Dashboard JSON metrics endpoint: /api/metrics?agent_id=X&check_type=Y&window_seconds=300
    http_server.Get("/api/metrics", [](const httplib::Request &req, httplib::Response &resp) {
        if (!s_storage) {
            resp.status = 503;
            resp.set_content("{\"error\":\"storage not available\"}", "application/json");
            return;
        }

        std::string agent_id;
        std::string check_type;
        int64_t window_seconds = 300;

        if (req.has_param("agent_id")) agent_id = req.get_param_value("agent_id");
        if (req.has_param("check_type")) check_type = req.get_param_value("check_type");
        if (req.has_param("window_seconds")) {
            try {
                window_seconds = std::stoll(req.get_param_value("window_seconds"));
            } catch (...) {
                window_seconds = 300;
            }
        }

        resp.set_content(s_storage->QueryMetricsJson(agent_id, check_type, window_seconds),
                         "application/json");
    });

    // Alerting endpoints for the dashboard.
    http_server.Get("/alerts", [](const httplib::Request &, httplib::Response &resp) {
        if (!s_alert_manager || !s_alert_manager->Enabled()) {
            resp.status = 200;
            resp.set_content("[]", "application/json");
            return;
        }
        resp.set_content(s_alert_manager->ActiveAlertsJson(), "application/json");
    });

    http_server.Get("/alert-history", [](const httplib::Request &req, httplib::Response &resp) {
        if (!s_alert_manager) {
            resp.set_content("[]", "application/json");
            return;
        }
        size_t max_events = 200;
        if (req.has_param("limit")) {
            try {
                max_events = static_cast<size_t>(std::stoll(req.get_param_value("limit")));
            } catch (...) {
                max_events = 200;
            }
        }
        resp.set_content(s_alert_manager->AlertHistoryJson(max_events),
                         "application/json");
    });

    http_server.Get("/alert-rules", [](const httplib::Request &, httplib::Response &resp) {
        if (!s_alert_manager) {
            resp.set_content("{\"rules\":[]}", "application/json");
            return;
        }
        resp.set_content(s_alert_manager->RulesJson(), "application/json");
    });

    // Diagnostic endpoint: forwards a diagnostic request to the target agent's
    // DiagnosticService (traceroute + pcap). Requires the agent to have
    // advertised a diagnostic endpoint in its heartbeat.
    http_server.Post("/diagnostic", [](const httplib::Request &req,
                                       httplib::Response &resp) {
        std::string agent_id = req.get_param_value("agent_id");
        std::string trace_target = req.get_param_value("trace_target");
        int pcap_duration_s = 0;
        if (req.has_param("pcap_duration_s")) {
            try {
                pcap_duration_s = std::stoi(req.get_param_value("pcap_duration_s"));
            } catch (...) { pcap_duration_s = 0; }
        }
        std::string pcap_filter = req.get_param_value("pcap_filter");

        if (agent_id.empty()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"agent_id is required\"}", "application/json");
            return;
        }
        std::string diag_endpoint = s_registry.GetDiagnosticEndpoint(agent_id);
        if (diag_endpoint.empty()) {
            resp.status = 404;
            resp.set_content("{\"error\":\"agent has no advertised diagnostic "
                             "endpoint\"}", "application/json");
            return;
        }

        auto channel = grpc::CreateChannel(diag_endpoint,
                                           grpc::InsecureChannelCredentials());
        auto stub = pudimnetmon::DiagnosticService::NewStub(channel);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
        pudimnetmon::DiagnosticRequest dreq;
        dreq.set_agent_id(agent_id);
        dreq.set_trace_target(trace_target);
        dreq.set_pcap_duration_s(pcap_duration_s);
        dreq.set_pcap_filter(pcap_filter);
        pudimnetmon::DiagnosticResponse dresp;
        grpc::Status status = stub->RunDiagnostic(&ctx, dreq, &dresp);
        if (!status.ok()) {
            resp.status = 502;
            resp.set_content("{\"error\":\"" + logger::escape(status.error_message()) +
                                 "\"}", "application/json");
            return;
        }
        std::string json = "{\"success\":" +
                           std::string(dresp.success() ? "true" : "false") +
                           ",\"timestamp_unix_ms\":" +
                           std::to_string(dresp.timestamp_unix_ms()) +
                           ",\"result\":\"" + logger::escape(dresp.result()) +
                           "\"}";
        resp.set_content(json, "application/json");
    });

    // Run HTTP server in a separate thread
    std::thread http_thread([&http_server, http_addr]() {
        logger::emit("info", "HTTP server starting on " + http_addr);
        // Parse host and port from "host:port" string
        auto colon = http_addr.find_last_of(':');
        std::string host = http_addr.substr(0, colon);
        int port = std::stoi(http_addr.substr(colon + 1));
        if (!http_server.listen(host.c_str(), port)) {
            logger::emit("error", "Failed to start HTTP server on " + http_addr);
        }
    });

    // Wait for shutdown signal
    while (s_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    logger::emit("info", "Shutting down gRPC server...");
    grpc_server->Shutdown();
    grpc_server->Wait();

    logger::emit("info", "Shutting down HTTP server...");
    http_server.stop();
    if (http_thread.joinable()) {
        http_thread.join();
    }

    logger::emit("info", "Collector shut down gracefully");
    return 0;
}