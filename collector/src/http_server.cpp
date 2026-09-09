#include "http_server.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent_dist.h"
#include "agent_registry.h"
#include "alerting/alert_manager.h"
#include "installer_dist.h"
#include "kafka/producer.h"
#include "logging.h"
#include "metrics_service.h"
#include "storage/timescale_storage.h"

// TODO: Check Prometheus usage

namespace pudimcollector {

HttpServer::HttpServer(
    const AgentRegistry &registry, std::shared_ptr<TimescaleStorage> storage,
    std::shared_ptr<MetricsServiceImpl> metrics_service,
    std::shared_ptr<alerting::AlertManager> alert_manager,
    std::shared_ptr<kafka::KafkaProducer> kafka_producer,
    AgentDist &agent_dist, InstallerDist &installer_dist, TlsOptions tls)
    : m_registry(registry),
      m_storage(std::move(storage)),
      m_metrics_service(std::move(metrics_service)),
      m_alert_manager(std::move(alert_manager)),
      m_kafka_producer(std::move(kafka_producer)),
      m_agent_dist(agent_dist),
      m_installer_dist(installer_dist),
      m_tls(std::move(tls)) {
    // Run handlers on a pool so one slow request cannot stall other endpoints.
    m_server.new_task_queue = [] {
        return new httplib::ThreadPool(4);
    };

    // Health.
    auto health_handler = [this](const httplib::Request &,
                                 httplib::Response &resp) {
        bool db_ok = m_storage ? m_storage->IsHealthy() : false;
        std::string status = db_ok ? "ok" : "degraded";
        resp.set_content("{\"status\":\"" + status +
                         "\",\"component\":\"collector\",\"storage\":" +
                         std::string(db_ok ? "true" : "false") + "}",
                         "application/json");
    };
    m_server.Get("/health", health_handler);
    m_server.Get("/api/health", health_handler);

    // Agent registry snapshot.
    auto agents_handler = [this](const httplib::Request &,
                                 httplib::Response &resp) {
        resp.set_content(m_registry.DumpAgents(), "application/json");
    };
    m_server.Get("/agents", agents_handler);
    m_server.Get("/api/agents", agents_handler);

    // Prometheus scrape endpoint (text format, no /api alias).
    m_server.Get("/metrics",
                 [this](const httplib::Request &, httplib::Response &resp) {
        resp.set_content(FormatPrometheusMetrics(),
                         "text/plain; version=0.0.4");
    });

    // Dashboard metrics query.
    m_server.Get("/api/metrics",
                 [this](const httplib::Request &req, httplib::Response &resp) {
        if (!m_storage) {
            resp.status = 503;
            resp.set_content("{\"error\":\"storage not available\"}",
                             "application/json");
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

        resp.set_content(
            m_storage->QueryMetricsJson(agent_id, check_type, window_seconds),
            "application/json");
    });
    // Alerting endpoints for the dashboard.
    auto alerts_handler = [this](const httplib::Request &,
                                 httplib::Response &resp) {
        if (!m_alert_manager || !m_alert_manager->Enabled()) {
            resp.status = 200;
            resp.set_content("[]", "application/json");
            return;
        }
        resp.set_content(m_alert_manager->ActiveAlertsJson(), "application/json");
    };
    m_server.Get("/alerts", alerts_handler);
    m_server.Get("/api/alerts", alerts_handler);

    auto alert_history_handler = [this](const httplib::Request &req,
                                        httplib::Response &resp) {
        if (!m_alert_manager) {
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
        resp.set_content(m_alert_manager->AlertHistoryJson(max_events),
                         "application/json");
    };
    m_server.Get("/alert-history", alert_history_handler);
    m_server.Get("/api/alert-history", alert_history_handler);

    auto alert_rules_handler = [this](const httplib::Request &,
                                      httplib::Response &resp) {
        if (!m_alert_manager) {
            resp.set_content("{\"rules\":[]}", "application/json");
            return;
        }
        resp.set_content(m_alert_manager->RulesJson(), "application/json");
    };
    m_server.Get("/alert-rules", alert_rules_handler);
    m_server.Get("/api/alert-rules", alert_rules_handler);

    // Runs an on-demand diagnostic (traceroute or pcap) on the target agent.
    auto diagnostic_handler = [this](const httplib::Request &req,
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
            resp.set_content("{\"error\":\"agent_id is required\"}",
                             "application/json");
            return;
        }

        // Dials the agent diagnostic service over mTLS when configured.
        auto stub = PrepareAgentCall(agent_id, resp);
        if (!stub) {
            return;
        }

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(30));
        pudimnetmon::DiagnosticRequest dreq;
        dreq.set_agent_id(agent_id);
        dreq.set_trace_target(trace_target);
        dreq.set_pcap_duration_s(pcap_duration_s);
        dreq.set_pcap_filter(pcap_filter);
        pudimnetmon::DiagnosticResponse dresp;
        grpc::Status status = stub->RunDiagnostic(&ctx, dreq, &dresp);
        if (!status.ok()) {
            SendAgentRpcError(resp, status);
            return;
        }
        std::string json = "{\"success\":" +
                           std::string(dresp.success() ? "true" : "false") +
                           ",\"timestamp_unix_ms\":" +
                           std::to_string(dresp.timestamp_unix_ms()) +
                           ",\"result\":\"" + logger::escape(dresp.result()) +
                           "\"}";
        resp.set_content(json, "application/json");
    };
    m_server.Post("/diagnostic", diagnostic_handler);
    m_server.Post("/api/diagnostic", diagnostic_handler);
    // Marks an active alert as acknowledged.
    m_server.Post("/api/alerts/ack",
                  [this](const httplib::Request &req, httplib::Response &resp) {
        if (!m_alert_manager) {
            resp.status = 503;
            resp.set_content("{\"error\":\"alerting disabled\"}",
                             "application/json");
            return;
        }
        std::string rule_id, agent_id, target;
        try {
            auto body = nlohmann::json::parse(req.body);
            rule_id = body.value("rule_id", "");
            agent_id = body.value("agent_id", "");
            target = body.value("target", "");
        } catch (...) {
            resp.status = 400;
            resp.set_content("{\"error\":\"invalid JSON body\"}",
                             "application/json");
            return;
        }
        if (rule_id.empty() || agent_id.empty()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"rule_id and agent_id are required\"}",
                             "application/json");
            return;
        }
        bool acked = m_alert_manager->Ack(rule_id, agent_id, target);
        resp.set_content("{\"acknowledged\":" +
                         std::string(acked ? "true" : "false") +
                         ",\"alerts\":" + m_alert_manager->ActiveAlertsJson() + "}",
                         "application/json");
    });

    // Applies a new probe configuration on the target agent.
    m_server.Post("/api/agents/config",
                  [this](const httplib::Request &req, httplib::Response &resp) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            resp.status = 400;
            resp.set_content("{\"error\":\"invalid JSON body\"}",
                             "application/json");
            return;
        }
        std::string agent_id = body.value("agent_id", "");
        if (agent_id.empty()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"agent_id is required\"}",
                             "application/json");
            return;
        }

        auto stub = PrepareAgentCall(agent_id, resp);
        if (!stub) {
            return;
        }

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(15));

        pudimnetmon::AgentConfigRequest creq;
        creq.set_agent_id(agent_id);
        auto put = [&body](const char *key,
                           google::protobuf::RepeatedPtrField<std::string> *field) {
            if (body.contains(key) && body[key].is_array()) {
                for (const auto &v : body[key]) field->Add(v.get<std::string>());
            }
        };
        put("dns_targets", creq.mutable_dns_targets());
        put("tcp_targets", creq.mutable_tcp_targets());
        put("tls_targets", creq.mutable_tls_targets());
        put("http_targets", creq.mutable_http_targets());
        put("ping_targets", creq.mutable_ping_targets());
        if (body.contains("ping_count")) creq.set_ping_count(body["ping_count"].get<int32_t>());
        if (body.contains("ping_gap_ms")) creq.set_ping_gap_ms(body["ping_gap_ms"].get<int32_t>());
        if (body.contains("tls_cert_check")) creq.set_tls_cert_check(body["tls_cert_check"].get<bool>());
        if (body.contains("tcp_retransmit_check")) creq.set_tcp_retransmit_check(body["tcp_retransmit_check"].get<bool>());
        if (body.contains("tcp_handshake_capture")) creq.set_tcp_handshake_capture(body["tcp_handshake_capture"].get<bool>());
        put("http_protocols", creq.mutable_http_protocols());

        pudimnetmon::AgentConfigResponse cresp;
        grpc::Status status = stub->Reconfigure(&ctx, creq, &cresp);
        if (!status.ok()) {
            SendAgentRpcError(resp, status);
            return;
        }
        std::string json = "{\"success\":" +
                           std::string(cresp.success() ? "true" : "false") +
                           ",\"applied\":\"" + logger::escape(cresp.applied()) +
                           "\",\"error\":\"" + logger::escape(cresp.error()) + "\"}";
        resp.set_content(json, "application/json");
    });

    // Returns the agent's current probe configuration.
    m_server.Get("/api/agents/config",
                 [this](const httplib::Request &req, httplib::Response &resp) {
        std::string agent_id = req.get_param_value("agent_id");
        if (agent_id.empty()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"agent_id is required\"}",
                             "application/json");
            return;
        }

        auto stub = PrepareAgentCall(agent_id, resp);
        if (!stub) {
            return;
        }

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(10));
        pudimnetmon::GetConfigRequest greq;
        greq.set_agent_id(agent_id);
        pudimnetmon::AgentConfigResponse gresp;
        grpc::Status status = stub->GetConfig(&ctx, greq, &gresp);
        if (!status.ok()) {
            SendAgentRpcError(resp, status);
            return;
        }
        std::string json = "{\"success\":" +
                           std::string(gresp.success() ? "true" : "false") +
                           ",\"applied\":\"" + logger::escape(gresp.applied()) +
                           "\",\"error\":\"" + logger::escape(gresp.error()) + "\"}";
        resp.set_content(json, "application/json");
    });
    // Runs the agent's built-in diagnostic commands.
    auto agent_commands_handler = [this](const httplib::Request &req,
                                         httplib::Response &resp) {
        std::string agent_id = req.get_param_value("agent_id");
        if (agent_id.empty()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"agent_id is required\"}",
                             "application/json");
            return;
        }

        auto stub = PrepareAgentCall(agent_id, resp);
        if (!stub) {
            return;
        }

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(10));
        pudimnetmon::ListCommandsRequest lreq;
        lreq.set_agent_id(agent_id);
        pudimnetmon::ListCommandsResponse lresp;
        grpc::Status status = stub->ListCommands(&ctx, lreq, &lresp);
        if (!status.ok()) {
            SendAgentRpcError(resp, status);
            return;
        }
        std::string json = "{\"success\":" +
                           std::string(lresp.success() ? "true" : "false") +
                           ",\"commands\":[";
        for (int i = 0; i < lresp.commands_size(); ++i) {
            const auto &c = lresp.commands(i);
            if (i > 0) json += ",";
            json += "{\"command_id\":\"" + logger::escape(c.command_id()) +
                    "\",\"description\":\"" + logger::escape(c.description()) +
                    "\",\"param_names\":[";
            for (int j = 0; j < c.param_names_size(); ++j) {
                if (j > 0) json += ",";
                json += "\"" + logger::escape(c.param_names(j)) + "\"";
            }
            json += "]}";
        }
        json += "]}";
        resp.set_content(json, "application/json");
    };
    m_server.Get("/api/agents/commands", agent_commands_handler);

    auto run_command_handler = [this](const httplib::Request &req,
                                      httplib::Response &resp) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            resp.status = 400;
            resp.set_content("{\"error\":\"invalid JSON body\"}",
                             "application/json");
            return;
        }
        std::string agent_id = body.value("agent_id", "");
        std::string command_id = body.value("command_id", "");
        if (agent_id.empty() || command_id.empty()) {
            resp.status = 400;
            resp.set_content(
                "{\"error\":\"agent_id and command_id are required\"}",
                "application/json");
            return;
        }

        auto stub = PrepareAgentCall(agent_id, resp);
        if (!stub) {
            return;
        }

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(30));
        pudimnetmon::RunCommandRequest creq;
        creq.set_agent_id(agent_id);
        creq.set_command_id(command_id);
        if (body.contains("params") && body["params"].is_object()) {
            for (auto it = body["params"].begin(); it != body["params"].end();
                 ++it) {
                if (it.value().is_string()) {
                    (*creq.mutable_params())[it.key()] =
                        it.value().get<std::string>();
                }
            }
        }
        pudimnetmon::CommandResponse cresp;
        grpc::Status status = stub->RunCommand(&ctx, creq, &cresp);
        if (!status.ok()) {
            SendAgentRpcError(resp, status);
            return;
        }
        std::string json = "{\"success\":" +
                           std::string(cresp.success() ? "true" : "false") +
                           ",\"command_id\":\"" +
                           logger::escape(cresp.command_id()) +
                           "\",\"error\":\"" + logger::escape(cresp.error()) +
                           "\",\"timestamp_unix_ms\":" +
                           std::to_string(cresp.timestamp_unix_ms()) +
                           ",\"summary\":\"" + logger::escape(cresp.summary()) +
                           "\",\"fields\":{";
        bool first_field = true;
        for (const auto &kv : cresp.fields()) {
            if (!first_field) json += ",";
            first_field = false;
            json += "\"" + logger::escape(kv.first) + "\":\"" +
                    logger::escape(kv.second) + "\"";
        }
        json += "},\"issues\":[";
        for (int i = 0; i < cresp.issues_size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + logger::escape(cresp.issues(i)) + "\"";
        }
        json += "],\"detail\":\"" + logger::escape(cresp.detail()) + "\"}";
        resp.set_content(json, "application/json");
    };
    m_server.Post("/api/agents/command", run_command_handler);

    // Agent binary manifest and download.
    auto agent_versions_handler = [this](const httplib::Request &,
                                         httplib::Response &resp) {
        resp.set_content(m_agent_dist.ManifestJson(), "application/json");
    };
    m_server.Get("/api/agent/versions", agent_versions_handler);
    m_server.Get("/agent/versions", agent_versions_handler);

    auto agent_download_handler = [this](const httplib::Request &req,
                                         httplib::Response &resp) {
        std::string platform = req.get_param_value("platform");
        const pudimcollector::AgentPlatform *p = m_agent_dist.Find(platform);
        if (!p) {
            resp.status = 404;
            resp.set_content(
                "{\"error\":\"no agent binary staged for platform '" +
                    logger::escape(platform) + "'\"}",
                "application/json");
            return;
        }
        std::vector<char> bytes;
        if (!m_agent_dist.LoadBinary(platform, bytes)) {
            resp.status = 500;
            resp.set_content("{\"error\":\"failed to read agent binary\"}",
                             "application/json");
            return;
        }
        resp.set_header("Content-Disposition",
                        "attachment; filename=\"" + p->filename + "\"");
        resp.set_content(bytes.data(), bytes.size(), "application/octet-stream");
    };
    m_server.Get("/api/agent/download", agent_download_handler);
    m_server.Get("/agent/download", agent_download_handler);

    // Installer artifact manifest and download. The `config` query parameter is
    // an optional base64url token embedded into the Content-Disposition filename
    // so the downloaded single-file installer self-configures from its own name.
    auto installer_versions_handler = [this](const httplib::Request &,
                                             httplib::Response &resp) {
        resp.set_content(m_installer_dist.ManifestJson(), "application/json");
    };
    m_server.Get("/api/installers/versions", installer_versions_handler);
    m_server.Get("/installers/versions", installer_versions_handler);

    auto installer_download_handler = [this](const httplib::Request &req,
                                             httplib::Response &resp) {
        std::string platform = req.get_param_value("platform");
        if (!m_installer_dist.Has(platform)) {
            resp.status = 404;
            resp.set_content(
                "{\"error\":\"no installer staged for platform '" +
                    logger::escape(platform) + "'\"}",
                "application/json");
            return;
        }
        std::vector<char> bytes;
        if (!m_installer_dist.Load(platform, bytes)) {
            resp.status = 500;
            resp.set_content("{\"error\":\"failed to read installer\"}",
                             "application/json");
            return;
        }
        std::string config = req.get_param_value("config");
        std::string name = m_installer_dist.DownloadName(platform, config);
        if (name.empty()) {
            resp.status = 404;
            resp.set_content("{\"error\":\"no installer staged for platform '" +
                                 logger::escape(platform) + "'\"}",
                             "application/json");
            return;
        }
        resp.set_header("Content-Disposition",
                        "attachment; filename=\"" + name + "\"");
        resp.set_content(bytes.data(), bytes.size(), "application/octet-stream");
    };
    m_server.Get("/api/installers/download", installer_download_handler);
    m_server.Get("/installers/download", installer_download_handler);

}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::Start(const std::string &addr) {
    m_thread = std::thread([this, addr]() {
        logger::emit("info", "HTTP server starting on " + addr);
        // Split addr into host and port.
        auto colon = addr.find_last_of(':');
        std::string host = addr.substr(0, colon);
        int port = std::stoi(addr.substr(colon + 1));
        if (!m_server.listen(host.c_str(), port)) {
            logger::emit("error", "Failed to start HTTP server on " + addr);
        }
    });
}

void HttpServer::Stop() {
    m_server.stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}
std::string HttpServer::FormatPrometheusMetrics() const {
    auto hb_count = m_registry.HeartbeatCount();
    auto active_count = m_registry.ActiveAgentCount();
    auto total_count = m_registry.TotalAgentCount();

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

    if (m_metrics_service) {
        out += "# HELP pudim_metrics_received_total Total metrics received\n";
        out += "# TYPE pudim_metrics_received_total counter\n";
        out += "pudim_metrics_received_total " +
               std::to_string(m_metrics_service->ReceivedMetrics()) + "\n";
        out += "# HELP pudim_metrics_batches_received_total Total metric batches received\n";
        out += "# TYPE pudim_metrics_batches_received_total counter\n";
        out += "pudim_metrics_batches_received_total " +
               std::to_string(m_metrics_service->BatchesReceived()) + "\n";
        out += "# HELP pudim_metrics_rejected_total Total metrics rejected\n";
        out += "# TYPE pudim_metrics_rejected_total counter\n";
        out += "pudim_metrics_rejected_total " +
               std::to_string(m_metrics_service->RejectedMetrics()) + "\n";
    }

    if (m_alert_manager) {
        out += "# HELP pudim_alerts_firing Currently firing alerts\n";
        out += "# TYPE pudim_alerts_firing gauge\n";
        out += "pudim_alerts_firing " +
               std::to_string(m_alert_manager->ActiveAlertCount()) + "\n";
        out += "# HELP pudim_alert_notifications_total Total alert notifications sent\n";
        out += "# TYPE pudim_alert_notifications_total counter\n";
        out += "pudim_alert_notifications_total " +
               std::to_string(m_alert_manager->TotalAlertsFired()) + "\n";
        out += "# HELP pudim_alert_rules_loaded Number of loaded alert rules\n";
        out += "# TYPE pudim_alert_rules_loaded gauge\n";
        out += "pudim_alert_rules_loaded " +
               std::to_string(m_alert_manager->RuleCount()) + "\n";
    }

    if (m_kafka_producer) {
        out += "# HELP pudim_kafka_produced_total Metrics batches produced to Kafka\n";
        out += "# TYPE pudim_kafka_produced_total counter\n";
        out += "pudim_kafka_produced_total " +
               std::to_string(m_kafka_producer->ProducedTotal()) + "\n";
        out += "# HELP pudim_kafka_delivery_failures_total Kafka delivery failures\n";
        out += "# TYPE pudim_kafka_delivery_failures_total counter\n";
        out += "pudim_kafka_delivery_failures_total " +
               std::to_string(m_kafka_producer->DeliveryFailures()) + "\n";
    }

    if (m_metrics_service) {
        out += "# HELP pudim_clock_skew_warnings_total Clock-skew warnings\n";
        out += "# TYPE pudim_clock_skew_warnings_total counter\n";
        out += "pudim_clock_skew_warnings_total " +
               std::to_string(m_metrics_service->SkewWarnings()) + "\n";
        out += "# HELP pudim_backpressure_signals_sent_total x-overloaded signals sent to agents\n";
        out += "# TYPE pudim_backpressure_signals_sent_total counter\n";
        out += "pudim_backpressure_signals_sent_total " +
               std::to_string(m_metrics_service->BackpressureSignalsSent()) + "\n";
    }

    if (m_storage) {
        auto stats = m_storage->GetStats();
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
               std::string(m_storage->IsHealthy() ? "1" : "0") + "\n";
    }

    return out;
}

std::unique_ptr<pudimnetmon::DiagnosticService::Stub>
HttpServer::PrepareAgentCall(const std::string &agent_id,
                             httplib::Response &resp) const {
    // The agent must advertise a diagnostic endpoint in its heartbeat.
    std::string diag_endpoint = m_registry.GetDiagnosticEndpoint(agent_id);
    if (diag_endpoint.empty()) {
        resp.status = 404;
        resp.set_content("{\"error\":\"agent has no advertised diagnostic "
                         "endpoint\"}",
                         "application/json");
        return nullptr;
    }
    // Dials over mTLS when configured.
    return DialAgentDiagnostic(diag_endpoint, m_tls);
}

void HttpServer::SendAgentRpcError(httplib::Response &resp,
                                   const grpc::Status &status) const {
    resp.status = 502;
    resp.set_content("{\"error\":\"" + logger::escape(status.error_message()) +
                         "\"}",
                     "application/json");
}

} // namespace pudimcollector
