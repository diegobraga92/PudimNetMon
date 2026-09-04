#include <algorithm>
#include <atomic>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <csignal>
#include "platform/platform.h"
#include "platform/win_service.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include "metrics.pb.h"
#include "metrics/metrics_client.h"
#include "metrics/probes.h"
#include "metrics/ntp_probe.h"
#include "metrics/failover_client.h"
#include "metrics/heartbeat_client.h"
#include "diagnostic_service.h"
#include "systemd_notify.h"
#include "trace_context.h"
#include "disk_buffer.h"
#include "tls_credentials.h"
#include "probe_runner.h"
#include "logger.h"
#include "agent_config.h"

using pudimnetmon::MetricsBatch;
using pudimagent::HeartbeatClient;
using pudimagent::MetricsClient;

static std::atomic<bool> s_running{true};

#ifndef _WIN32
// Async-signal-safe raw write() so the handler never deadlocks.
static void SafeWriteStr(const char *s) {
    size_t n = 0;
    // strlen should be async-signal-safe, but avoids it to be extra safe
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

int RunAgent(int argc, char **argv) {
    // Initial config
    pudimagent::AgentConfig cfg;
    switch (pudimagent::LoadAgentConfig(argc, argv, cfg)) {
        case pudimagent::ConfigResult::Ok: break;
        case pudimagent::ConfigResult::Help: return 0;
        case pudimagent::ConfigResult::Error: return 1;
    }

    logger::SetNodeId(cfg.node_id);
    logger::SetTraceId(cfg.trace_id);
    logger::SetLevel(cfg.log_level);

    pudimagent::SetNtpServer(cfg.ntp_server);

    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    LOG_INFO("Agent starting up");
    LOG_INFO(cfg.cfg_loaded ? ("Loaded config file: " + cfg.config_path)
                            : ("No config file at " + cfg.config_path +
                               "; using built-in defaults and CLI flags"));

    pudimagent::LogConfigAudit(cfg);
    LOG_INFO("Collector endpoint: " + cfg.collector_endpoint);
    LOG_INFO("Node ID: " + cfg.node_id);
    LOG_INFO("Interval: " + std::to_string(cfg.interval_ms) + "ms");
    LOG_INFO("Version: " + cfg.version);

    // TODO: Check mTLS usage

    // Start diag thread
    auto diag_server_creds =
        pudimagent::MakeServerCredentials(cfg.tls_ca, cfg.tls_cert, cfg.tls_key);
    auto probe_store = std::make_shared<pudimagent::ProbeConfigStore>();
    std::thread diagnostic_thread(
        [port = cfg.diagnostic_port, diag_server_creds, probe_store]() {
            pudimagent::DiagnosticServiceImpl diag_service(probe_store);
            grpc::ServerBuilder builder;
            builder.AddListeningPort("0.0.0.0:" + port, diag_server_creds);
            builder.RegisterService(&diag_service);
            auto server = builder.BuildAndStart();
            if (!server) {
                std::cerr << "Failed to start diagnostic server on port "
                          << port << "\n";
                return;
            }
            LOG_INFO("Diagnostic gRPC server listening on port " + port);
            server->Wait();
        });
    diagnostic_thread.detach();

    std::vector<std::string> endpoints;
    if (!cfg.collector_endpoints.empty()) {
        endpoints = cfg.collector_endpoints;
    } else {
        endpoints.push_back(cfg.collector_endpoint);
    }
    pudimagent::FailoverClient failover(endpoints);

    auto creds = pudimagent::MakeChannelCredentials(cfg.tls_ca, cfg.tls_cert,
                                                    cfg.tls_key);
    LOG_INFO(cfg.tls_ca.empty() ? "gRPC transport: insecure (no --tls-*)"
                                : "gRPC transport: mTLS (client cert " +
                                      cfg.tls_cert + ")");

    // TODO: Check if a pair is the best approach here

    auto reconnect = [&]() {
        const std::string &ep = failover.CurrentEndpoint();
        LOG_INFO("Connecting to collector endpoint: " + ep);
        return std::make_pair(
            std::make_unique<HeartbeatClient>(ep, creds, cfg.node_id,
                                              cfg.diagnostic_address),
            std::make_unique<MetricsClient>(ep, creds));
    };
    auto clients = reconnect();

#ifndef _WIN32
    std::signal(SIGHUP, handle_signal);
#endif
    pudimagent::NotifyReady();
    bool watchdog_stop = false;
    pudimagent::StartWatchdogThread(&watchdog_stop);

    std::vector<std::string> probe_targets;
    for (const auto *vec : {&cfg.probe_cfg.dns_targets, &cfg.probe_cfg.tcp_targets,
                            &cfg.probe_cfg.tls_targets, &cfg.probe_cfg.http_targets,
                            &cfg.probe_cfg.ping_targets}) {
        probe_targets.insert(probe_targets.end(), vec->begin(), vec->end());
    }

    if (probe_targets.empty()) {
        // Default demo targets so a bare `pudim-agent` produces useful output
        cfg.probe_cfg.dns_targets = {"example.com"};
        cfg.probe_cfg.tcp_targets = {"example.com:443"};
        cfg.probe_cfg.tls_targets = {"example.com:443"};
        cfg.probe_cfg.http_targets = {"https://example.com"};
        cfg.probe_cfg.ping_targets = {"1.1.1.1"};
    }
    probe_store->Set(cfg.probe_cfg);

    LOG_INFO("Running metrics probes every " + std::to_string(cfg.interval_ms) +
             "ms");

    pudimagent::DiskBuffer disk_buffer(
        cfg.disk_buffer_path,
        static_cast<uint64_t>(cfg.disk_buffer_max_mb) * 1024 * 1024);
    std::string db_err;
    if (disk_buffer.Open(db_err)) {
        LOG_INFO("Disk buffer ready at " + cfg.disk_buffer_path +
                 " (pending=" + std::to_string(disk_buffer.Size()) + ")");
    } else {
        LOG_WARN("Disk buffer unavailable: " + db_err +
                 " (in-memory buffering only)");
    }

    // Main loop
    uint64_t buffer_drops = 0;
    uint64_t disk_spills = 0;
    uint64_t disk_drained_total = 0;
    int current_interval_ms = cfg.interval_ms;
    bool backpressure = false;

    std::deque<MetricsBatch> buffer;

    pudimagent::ProbeRunner probe_runner;
    probe_runner.SetConfigStore(probe_store);
    probe_runner.SetIntervalMs(cfg.interval_ms);
    if (cfg.tcp_handshake_interval_ms > 0) {
        probe_runner.SetHandshakeIntervalMs(cfg.tcp_handshake_interval_ms);
    }
    probe_runner.Start(cfg.node_id);

    auto last_heartbeat =
        std::chrono::steady_clock::now() -
        std::chrono::milliseconds(cfg.interval_ms);
    auto last_send_attempt =
        std::chrono::steady_clock::now() -
        std::chrono::milliseconds(cfg.interval_ms);
    auto last_self_report = std::chrono::steady_clock::now();


    while (s_running) {
        auto now = std::chrono::steady_clock::now();

        if (now - last_heartbeat >=
            std::chrono::milliseconds(current_interval_ms)) {
            last_heartbeat = now;
            std::string traceparent = pudimagent::GenerateTraceParent();
            bool hb_ok = clients.first->SendHeartbeat(
                cfg.interval_ms, cfg.version, traceparent);
            if (hb_ok) {
                failover.OnSendSuccess();
            } else if (failover.OnSendFailure()) {
                LOG_WARN("Heartbeat failed; failing over to " +
                         failover.CurrentEndpoint());
                clients = reconnect();
            }
        }

        std::vector<MetricsBatch> fresh;
        probe_runner.Drain(fresh, 50);
        for (auto &b : fresh) {
            buffer.push_back(std::move(b));
            while (static_cast<int>(buffer.size()) > cfg.max_buffer_size) {
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
            bool ok = cfg.use_stream_metrics
                          ? clients.second->StreamMetrics(cfg.node_id,
                                                          to_send.metrics(),
                                                          traceparent)
                          : clients.second->SendBatch(to_send, traceparent);
            if (ok) {
                buffer.pop_front();
                LOG_INFO("Metrics batch accepted by collector");
                failover.OnSendSuccess();
                backpressure = clients.second->BackpressureSignalled();

                for (int i = 0; i < 10 && disk_buffer.Size() > 0; i++) {
                    std::vector<std::string> blobs;
                    disk_buffer.Peek(blobs, 1);
                    if (blobs.empty()) break;
                    MetricsBatch pb;
                    if (pb.ParseFromString(blobs[0])) {
                        bool d_ok =
                            cfg.use_stream_metrics
                                ? clients.second->StreamMetrics(pb.agent_id(),
                                                                pb.metrics(),
                                                                traceparent)
                                : clients.second->SendBatch(pb, traceparent);
                        if (!d_ok) break;
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

        if (backpressure) {
            current_interval_ms = std::min(current_interval_ms * 2,
                                           cfg.interval_ms * 10);
            LOG_WARN("Collector signalled overload; backing off to " +
                     std::to_string(current_interval_ms) + "ms");
        } else if (current_interval_ms != cfg.interval_ms) {
            current_interval_ms = cfg.interval_ms;
            LOG_INFO("Backpressure cleared; interval restored to " +
                     std::to_string(cfg.interval_ms) + "ms");
        }
        probe_runner.SetIntervalMs(current_interval_ms);

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

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    probe_runner.Stop();

    watchdog_stop = true;
    if (disk_buffer.Size() > 0) {
        LOG_WARN(std::to_string(disk_buffer.Size()) +
                 " batches remain in disk buffer; will be retried on next start");
    }
    LOG_INFO("Agent shut down gracefully");
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    std::string net_err;
    if (!pudimagent::platform::InitNetwork(net_err)) {
        std::cerr << net_err << "\n";
        return 1;
    }

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

