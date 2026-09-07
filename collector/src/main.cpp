#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include "agent_dist.h"
#include "agent_registry.h"
#include "agent_rpc.h"
#include "agent_service.h"
#include "alerting/alert_manager.h"
#include "http_server.h"
#include "kafka/producer.h"
#include "logging.h"
#include "metrics_service.h"
#include "storage/timescale_storage.h"
#include "tls_credentials.h"

using grpc::Server;
using grpc::ServerBuilder;

// Cleared by the signal handler to request shutdown.
static std::atomic<bool> s_running{true};

static void handle_signal(int sig) {
    const char *sig_name = (sig == SIGTERM) ? "SIGTERM" :
                           (sig == SIGINT)  ? "SIGINT" : "UNKNOWN";
    logger::emit("info", std::string("Received ") + sig_name + ", shutting down...");
    s_running = false;
}

int main(int argc, char **argv) {
    std::string grpc_addr = "0.0.0.0:50051";
    std::string http_addr = "0.0.0.0:8080";
    std::string db_host = "localhost";
    int db_port = 5432;
    std::string db_name = "pudimnetmon";
    std::string db_user = "pudim";
    std::string db_password = "pudim";
    std::string alert_rules_path;
    std::string kafka_brokers;   // empty selects Direct mode
    std::string kafka_topic = "network.metrics";
    int64_t skew_threshold_ms = 5000;  // clock-skew warning threshold
    int64_t backpressure_threshold_ms = 1000;  // ingest latency that triggers x-overloaded
    std::string tls_ca;    // PEM CA used to verify agent client certs (mTLS)
    std::string tls_cert;  // PEM server certificate
    std::string tls_key;   // PEM server private key
    std::string agent_dist_dir = "/usr/share/pudim/agents";  // staged agent binaries for download

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

    // TODO: Check about replacing CLI settings

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
        } else if ((v = opt(arg, "--backpressure-threshold-ms", i)) != "") {
            backpressure_threshold_ms = std::stoll(v);
        } else if ((v = opt(arg, "--tls-ca", i)) != "") {
            tls_ca = v;
        } else if ((v = opt(arg, "--tls-cert", i)) != "") {
            tls_cert = v;
        } else if ((v = opt(arg, "--tls-key", i)) != "") {
            tls_key = v;
        } else if ((v = opt(arg, "--agent-dist-dir", i)) != "") {
            agent_dist_dir = v;
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
                      << "  --backpressure-threshold-ms Ingest latency (ms) that triggers x-overloaded (default: 1000)\n"
                      << "  --tls-ca              PEM CA to verify agent client certs (mTLS)\n"
                      << "  --tls-cert            PEM server certificate (mTLS)\n"
                      << "  --tls-key             PEM server private key (mTLS)\n"
                      << "  --agent-dist-dir      Directory with staged pudim-agent binaries served to the\n"
                      << "                        dashboard (default: /usr/share/pudim/agents)\n"
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

    // Heartbeat registry shared by the gRPC service and the HTTP endpoints.
    pudimcollector::AgentRegistry registry;

    // Initialize storage
    pudimcollector::StorageConfig storage_cfg;
    storage_cfg.host = db_host;
    storage_cfg.port = db_port;
    storage_cfg.dbname = db_name;
    storage_cfg.user = db_user;
    storage_cfg.password = db_password;

    auto storage = std::make_shared<pudimcollector::TimescaleStorage>(storage_cfg);
    if (!storage->Connect()) {
        logger::emit("warn", "Storage connection failed; collector will run "
                     "without persistent storage (metrics will be rejected)");
    } else {
        logger::emit("info", "Storage connected (TimescaleDB)");
    }

    // TODO: Check Kafka usage
    
    // Kafka mode is enabled by passing --kafka-brokers. Consumers then own
    // storage and alerting.
    pudimcollector::StorageMode storage_mode = pudimcollector::StorageMode::Direct;
    std::shared_ptr<pudimcollector::kafka::KafkaProducer> kafka_producer;
    if (!kafka_brokers.empty()) {
        storage_mode = pudimcollector::StorageMode::Kafka;
        kafka_producer = std::make_shared<pudimcollector::kafka::KafkaProducer>();
        std::string err;
        if (!kafka_producer->Connect(kafka_brokers, kafka_topic, err)) {
            logger::emit("error", "Kafka producer connection failed: " + err);
            return 1;
        }
        logger::emit("info", "Kafka mode enabled (topic=" + kafka_topic + ")");
    }

    // Alert manager used in Direct mode.
    auto alert_manager = std::make_shared<pudimcollector::alerting::AlertManager>();
    if (storage_mode == pudimcollector::StorageMode::Direct &&
        !alert_rules_path.empty()) {
        std::string err;
        if (alert_manager->LoadRulesFromFile(alert_rules_path, err)) {
            logger::emit("info",
                         "Loaded " + std::to_string(alert_manager->RuleCount()) +
                         " alert rules from " + alert_rules_path);
        } else {
            logger::emit("warn", "Failed to load alert rules: " + err);
        }
    } else if (storage_mode == pudimcollector::StorageMode::Direct) {
        logger::emit("info",
                     "No alert rules configured (--alert-rules-path unset); alerting disabled");
    }

    auto metrics_service =
        std::make_shared<pudimcollector::MetricsServiceImpl>(storage,
                                                             alert_manager,
                                                             kafka_producer,
                                                             storage_mode,
                                                             skew_threshold_ms,
                                                             backpressure_threshold_ms);

    // Start gRPC server (mTLS when --tls-* flags are provided)
    auto server_creds = pudimagent::MakeServerCredentials(tls_ca, tls_cert, tls_key);
    logger::emit("info", tls_ca.empty() ? "gRPC transport: insecure (no --tls-*)"
                                        : "gRPC transport: mTLS (server cert " + tls_cert + ")");
    pudimcollector::AgentServiceImpl agent_service(registry);
    ServerBuilder builder;
    builder.AddListeningPort(grpc_addr, server_creds);
    builder.RegisterService(&agent_service);
    builder.RegisterService(metrics_service.get());
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024); // 4MB

    std::unique_ptr<Server> grpc_server = builder.BuildAndStart();
    if (!grpc_server) {
        logger::emit("error", "Failed to start gRPC server on " + grpc_addr);
        return 1;
    }
    logger::emit("info", "gRPC server listening on " + grpc_addr);

    // Serves staged agent binaries for self-hosted download.
    pudimcollector::AgentDist agent_dist;
    if (!agent_dist.Scan(agent_dist_dir)) {
        logger::emit("info", "No staged agent binaries in '" + agent_dist_dir +
                     "'; agent download disabled");
    } else {
        logger::emit("info", "Serving " +
                     std::to_string(agent_dist.Platforms().size()) +
                     " agent platform(s) from '" + agent_dist_dir +
                     "' (version " + agent_dist.Version() + ")");
    }

    // HTTP routes live in HttpServer.
    pudimcollector::HttpServer http_server(
        registry, storage, metrics_service, alert_manager, kafka_producer,
        agent_dist, pudimcollector::TlsOptions{tls_ca, tls_cert, tls_key});
    http_server.Start(http_addr);

    // Wait for shutdown signal
    while (s_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    logger::emit("info", "Shutting down gRPC server...");
    grpc_server->Shutdown();
    grpc_server->Wait();

    logger::emit("info", "Shutting down HTTP server...");
    http_server.Stop();

    logger::emit("info", "Collector shut down gracefully");
    return 0;
}
