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
#include "installer_dist.h"
#include "kafka/producer.h"
#include "logging.h"
#include "metrics_service.h"
#include "release_mirror.h"
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
    std::string installer_dist_dir = "/usr/share/pudim/installers";  // staged installer artifacts
    std::string github_owner;
    std::string github_repo;
    std::string github_api_base = "https://api.github.com";
    std::string github_token;             // optional; from --github-token or env
    int64_t release_sync_seconds = 3600;  // 0 = sync once at startup only

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
    github_owner = get_env("PUDIM_GITHUB_OWNER", github_owner);
    github_repo = get_env("PUDIM_GITHUB_REPO", github_repo);
    github_token = get_env("PUDIM_GITHUB_TOKEN", github_token);
    github_api_base = get_env("PUDIM_GITHUB_API_BASE", github_api_base);
    release_sync_seconds =
        std::stoll(get_env("PUDIM_RELEASE_SYNC_SECONDS",
                           std::to_string(release_sync_seconds)));

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
        } else if ((v = opt(arg, "--installer-dist-dir", i)) != "") {
            installer_dist_dir = v;
        } else if ((v = opt(arg, "--github-owner", i)) != "") {
            github_owner = v;
        } else if ((v = opt(arg, "--github-repo", i)) != "") {
            github_repo = v;
        } else if ((v = opt(arg, "--github-api-base", i)) != "") {
            github_api_base = v;
        } else if ((v = opt(arg, "--github-token", i)) != "") {
            github_token = v;
        } else if ((v = opt(arg, "--release-sync-seconds", i)) != "") {
            release_sync_seconds = std::stoll(v);
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
                      << "  --installer-dist-dir  Directory with staged installer artifacts (Inno setup exe\n"
                      << "                        or Linux .run files) served to the dashboard\n"
                      << "                        (default: /usr/share/pudim/installers)\n"
                      << "  --github-owner        GitHub owner whose latest release is mirrored into\n"
                      << "                        the installer staged dir (default: empty = disabled)\n"
                      << "  --github-repo         GitHub repository name (default: empty)\n"
                      << "  --github-api-base     GitHub API base URL (default: https://api.github.com)\n"
                      << "  --github-token        Optional GitHub token for private repos / rate limits\n"
                      << "  --release-sync-seconds How often to re-check the latest release\n"
                      << "                        (default: 3600; 0 = once at startup)\n"
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

    pudimcollector::InstallerDist installer_dist;
    if (!installer_dist.Scan(installer_dist_dir)) {
        logger::emit("info", "No staged installer artifacts in '" +
                     installer_dist_dir + "'; installer download disabled");
    } else {
        logger::emit("info", "Serving " +
                     std::to_string(installer_dist.Artifacts().size()) +
                     " installer artifact(s) from '" + installer_dist_dir +
                     "' (version " + installer_dist.Version() + ")");
    }

    // HTTP routes live in HttpServer.
    pudimcollector::HttpServer http_server(
        registry, storage, metrics_service, alert_manager, kafka_producer,
        agent_dist, installer_dist, pudimcollector::TlsOptions{tls_ca, tls_cert, tls_key});
    http_server.Start(http_addr);

    std::unique_ptr<pudimcollector::ReleaseMirror> release_mirror;
    if (!github_owner.empty() && !github_repo.empty()) {
        pudimcollector::ReleaseMirrorConfig cfg;
        cfg.owner = github_owner;
        cfg.repo = github_repo;
        cfg.api_base = github_api_base;
        cfg.token = github_token;
        cfg.installer_dir = installer_dist_dir;
        release_mirror = std::make_unique<pudimcollector::ReleaseMirror>(cfg);

        std::string summary;
        if (release_mirror->SyncOnce(summary)) {
            installer_dist.Scan(installer_dist_dir);
            logger::emit("info", "Release mirror: " + summary);
        } else {
            logger::emit("info", "Release mirror: " + summary +
                                 " (will retry on schedule)");
        }
    }

    // Wait for shutdown signal, re-syncing the release mirror on schedule.
    auto last_sync = std::chrono::steady_clock::now();
    while (s_running) {
        if (release_mirror && release_sync_seconds > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - last_sync)
                               .count();
            if (elapsed >= release_sync_seconds) {
                std::string summary;
                if (release_mirror->SyncOnce(summary)) {
                    installer_dist.Scan(installer_dist_dir);
                }
                logger::emit("info", "Release mirror: " + summary);
                last_sync = std::chrono::steady_clock::now();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    logger::emit("info", "Shutting down gRPC server...");
    grpc_server->Shutdown();
    grpc_server->Wait();

    logger::emit("info", "Shutting down HTTP server...");
    http_server.Stop();

    logger::emit("info", "Collector shut down gracefully");
    return 0;
}
