#include "http_server.h"

#include <algorithm>
#include <atomic>
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

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Small monotonic id generator for persisted schedules.
std::string NewScheduleId() {
    static std::atomic<uint64_t> counter{0};
    return "cs_" + std::to_string(NowMs() / 1000) + "_" +
           std::to_string(counter.fetch_add(1));
}

// Adds UI-friendly derived fields (active/expired/next_in_ms).
nlohmann::json ScheduleToJson(const CommandSchedule &s, int64_t now_ms) {
    nlohmann::json doc;
    doc["id"] = s.id;
    doc["label"] = s.label;
    doc["agent_id"] = s.agent_id;
    doc["command_id"] = s.command_id;
    try {
        doc["params"] = nlohmann::json::parse(s.params_json);
    } catch (...) {
        doc["params"] = nlohmann::json::object();
    }
    doc["window_start_unix_ms"] = s.window_start_unix_ms;
    doc["window_end_unix_ms"] = s.window_end_unix_ms;
    doc["interval_sec"] = s.interval_sec;
    doc["next_run_unix_ms"] = s.next_run_unix_ms;
    doc["enabled"] = s.enabled;
    doc["expired"] = s.window_end_unix_ms < now_ms;
    doc["active"] = s.enabled && s.window_start_unix_ms <= now_ms &&
                    s.window_end_unix_ms >= now_ms;
    int64_t next_in = s.next_run_unix_ms - now_ms;
    doc["next_in_ms"] = next_in > 0 ? next_in : 0;
    return doc;
}

nlohmann::json RunToJson(const CommandRun &r) {
    nlohmann::json doc;
    doc["run_id"] = r.run_id;
    doc["schedule_id"] = r.schedule_id;
    doc["agent_id"] = r.agent_id;
    doc["command_id"] = r.command_id;
    doc["scheduled_unix_ms"] = r.scheduled_unix_ms;
    doc["started_unix_ms"] = r.started_unix_ms;
    doc["finished_unix_ms"] = r.finished_unix_ms;
    doc["running"] = !r.has_result;
    doc["success"] = r.has_result && r.success;
    doc["error"] = r.error;
    doc["summary"] = r.summary;
    try {
        doc["fields"] = nlohmann::json::parse(r.fields_json);
    } catch (...) {
        doc["fields"] = nlohmann::json::object();
    }
    try {
        doc["issues"] = nlohmann::json::parse(r.issues_json);
    } catch (...) {
        doc["issues"] = nlohmann::json::array();
    }
    doc["detail"] = r.detail;
    return doc;
}

} // namespace

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

    // Builds the {success, error, rules, persisted} response used by rule mutations
    auto alert_rules_response = [this](bool success, const std::string &error) {
        nlohmann::json doc;
        if (m_alert_manager) {
            doc = nlohmann::json::parse(m_alert_manager->RulesJson());
        } else {
            doc["rules"] = nlohmann::json::array();
        }
        doc["success"] = success;
        doc["error"] = error;
        doc["persisted"] =
            m_alert_manager && !m_alert_manager->RulesFilePath().empty();
        return doc.dump();
    };

    // Creates or replaces one alert rule. Body: {"rule": {...}}.
    auto upsert_rule_handler = [this, alert_rules_response](
                                   const httplib::Request &req,
                                   httplib::Response &resp) {
        if (!m_alert_manager) {
            resp.status = 503;
            resp.set_content("{\"error\":\"alerting unavailable\"}",
                             "application/json");
            return;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            resp.status = 400;
            resp.set_content("{\"error\":\"invalid JSON body\"}",
                             "application/json");
            return;
        }
        if (!body.is_object() || !body.contains("rule") ||
            body["rule"].is_null() || !body["rule"].is_object()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"a 'rule' object is required\"}",
                             "application/json");
            return;
        }
        std::string err;
        const bool ok = m_alert_manager->UpsertRule(body["rule"].dump(), err);
        resp.status = ok ? 200 : 400;
        resp.set_content(alert_rules_response(ok, err), "application/json");
    };
    m_server.Post("/alert-rules", upsert_rule_handler);
    m_server.Post("/api/alert-rules", upsert_rule_handler);

    // Deletes one rule and its firing state. Body: {"rule_id": "..."}.
    auto delete_rule_handler = [this, alert_rules_response](
                                   const httplib::Request &req,
                                   httplib::Response &resp) {
        if (!m_alert_manager) {
            resp.status = 503;
            resp.set_content("{\"error\":\"alerting unavailable\"}",
                             "application/json");
            return;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            resp.status = 400;
            resp.set_content("{\"error\":\"invalid JSON body\"}",
                             "application/json");
            return;
        }
        if (!body.is_object() ||
            body.value("rule_id", "").empty()) {
            resp.status = 400;
            resp.set_content(
                "{\"error\":\"'rule_id' is required\"}", "application/json");
            return;
        }
        std::string err;
        const bool ok =
            m_alert_manager->DeleteRule(body["rule_id"].get<std::string>(), err);
        resp.status = ok ? 200 : 400;
        resp.set_content(alert_rules_response(ok, err), "application/json");
    };
    m_server.Post("/alert-rules/delete", delete_rule_handler);
    m_server.Post("/api/alert-rules/delete", delete_rule_handler);

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

        // Keep ad-hoc runs in the same history table the scheduler writes to.
        if (m_storage) {
            CommandRun run;
            run.agent_id = agent_id;
            run.command_id = command_id;
            run.scheduled_unix_ms =
                cresp.timestamp_unix_ms() > 0 ? cresp.timestamp_unix_ms()
                                              : NowMs();
            run.started_unix_ms = run.scheduled_unix_ms;
            std::string perr;
            const int64_t rid = m_storage->InsertCommandRun(run, &perr);
            if (rid > 0) {
                run.run_id = rid;
                run.finished_unix_ms = NowMs();
                run.success = cresp.success();
                run.error = cresp.error();
                run.summary = cresp.summary();
                nlohmann::json fields = nlohmann::json::object();
                for (const auto &kv : cresp.fields()) {
                    fields[kv.first] = kv.second;
                }
                nlohmann::json issues = nlohmann::json::array();
                for (int i = 0; i < cresp.issues_size(); ++i) {
                    issues.push_back(cresp.issues(i));
                }
                run.fields_json = fields.dump();
                run.issues_json = issues.dump();
                run.detail = cresp.detail();
                m_storage->FinishCommandRun(run, nullptr);
            }
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

    auto storage_ready = [this](httplib::Response &resp) {
        if (!m_storage || !m_storage->IsHealthy()) {
            resp.status = 503;
            resp.set_content(
                "{\"error\":\"scheduled commands require TimescaleDB storage\"}",
                "application/json");
            return false;
        }
        return true;
    };

    // Lists every persisted command schedule.
    auto list_schedules_handler = [this, storage_ready](const httplib::Request &,
                                                        httplib::Response &resp) {
        if (!storage_ready(resp)) return;
        const int64_t now = NowMs();
        nlohmann::json doc;
        doc["schedules"] = nlohmann::json::array();
        for (const auto &s : m_storage->ListCommandSchedules()) {
            doc["schedules"].push_back(ScheduleToJson(s, now));
        }
        resp.set_content(doc.dump(), "application/json");
    };
    m_server.Get("/command-schedules", list_schedules_handler);
    m_server.Get("/api/command-schedules", list_schedules_handler);

    // Creates a schedule. Body:
    // {agent_id, command_id, params?, label?, window_start_unix_ms?,
    //  window_end_unix_ms, interval_sec}
    auto create_schedule_handler = [this, storage_ready](
                                       const httplib::Request &req,
                                       httplib::Response &resp) {
        if (!storage_ready(resp)) return;
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            resp.status = 400;
            resp.set_content("{\"error\":\"invalid JSON body\"}",
                             "application/json");
            return;
        }
        if (!body.is_object()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"a JSON object is required\"}",
                             "application/json");
            return;
        }
        const std::string agent_id = body.value("agent_id", "");
        const std::string command_id = body.value("command_id", "");
        if (agent_id.empty() || command_id.empty()) {
            resp.status = 400;
            resp.set_content(
                "{\"error\":\"agent_id and command_id are required\"}",
                "application/json");
            return;
        }
        const int64_t now = NowMs();
        int64_t window_start = now;
        if (body.contains("window_start_unix_ms") &&
            body["window_start_unix_ms"].is_number_integer()) {
            window_start = body["window_start_unix_ms"].get<int64_t>();
        }
        if (!body.contains("window_end_unix_ms") ||
            !body["window_end_unix_ms"].is_number_integer()) {
            resp.status = 400;
            resp.set_content(
                "{\"error\":\"window_end_unix_ms is required\"}",
                "application/json");
            return;
        }
        const int64_t window_end =
            body["window_end_unix_ms"].get<int64_t>();
        if (!body.contains("interval_sec") ||
            !body["interval_sec"].is_number_integer()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"interval_sec is required\"}",
                             "application/json");
            return;
        }
        const int64_t interval_sec = body["interval_sec"].get<int64_t>();
        const int64_t kMinIntervalSec = 60;
        if (interval_sec < kMinIntervalSec) {
            resp.status = 400;
            resp.set_content("{\"error\":\"interval_sec must be >= " +
                                 std::to_string(kMinIntervalSec) + "\"}",
                             "application/json");
            return;
        }
        if (window_end <= window_start || window_end < now) {
            resp.status = 400;
            resp.set_content(
                "{\"error\":\"window_end_unix_ms must be after the start and in "
                "the future\"}",
                "application/json");
            return;
        }

        CommandSchedule s;
        s.id = NewScheduleId();
        s.label = body.value("label", "");
        s.agent_id = agent_id;
        s.command_id = command_id;
        s.window_start_unix_ms = window_start;
        s.window_end_unix_ms = window_end;
        s.interval_sec = interval_sec;
        // First run: at the window start when it is still ahead, otherwise now.
        s.next_run_unix_ms = std::max(now, window_start);
        s.enabled = true;
        if (body.contains("params") && body["params"].is_object()) {
            s.params_json = body["params"].dump();
        }
        std::string err;
        if (!m_storage->CreateCommandSchedule(s, &err)) {
            resp.status = 400;
            nlohmann::json doc;
            doc["success"] = false;
            doc["error"] = err;
            resp.set_content(doc.dump(), "application/json");
            return;
        }
        nlohmann::json doc;
        doc["success"] = true;
        doc["error"] = "";
        doc["schedule"] = ScheduleToJson(s, now);
        resp.set_content(doc.dump(), "application/json");
    };
    m_server.Post("/command-schedules", create_schedule_handler);
    m_server.Post("/api/command-schedules", create_schedule_handler);

    // Deletes one schedule (keeps its run history). Body: {"id": "..."}
    auto delete_schedule_handler = [this, storage_ready](
                                       const httplib::Request &req,
                                       httplib::Response &resp) {
        if (!storage_ready(resp)) return;
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            resp.status = 400;
            resp.set_content("{\"error\":\"invalid JSON body\"}",
                             "application/json");
            return;
        }
        const std::string id = body.value("id", "");
        if (id.empty()) {
            resp.status = 400;
            resp.set_content("{\"error\":\"'id' is required\"}",
                             "application/json");
            return;
        }
        std::string err;
        const bool ok = m_storage->DeleteCommandSchedule(id, &err);
        resp.status = ok ? 200 : 400;
        nlohmann::json doc;
        doc["success"] = ok;
        doc["error"] = ok ? "" : err;
        resp.set_content(doc.dump(), "application/json");
    };
    m_server.Post("/command-schedules/delete", delete_schedule_handler);
    m_server.Post("/api/command-schedules/delete", delete_schedule_handler);

    // Enables or pauses one schedule. Body: {"id": "...", "enabled": bool}
    auto enable_schedule_handler = [this, storage_ready](
                                       const httplib::Request &req,
                                       httplib::Response &resp) {
        if (!storage_ready(resp)) return;
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            resp.status = 400;
            resp.set_content("{\"error\":\"invalid JSON body\"}",
                             "application/json");
            return;
        }
        const std::string id = body.value("id", "");
        if (id.empty() || !body.contains("enabled")) {
            resp.status = 400;
            resp.set_content(
                "{\"error\":\"'id' and 'enabled' are required\"}",
                "application/json");
            return;
        }
        std::string err;
        const bool ok = m_storage->SetCommandScheduleEnabled(
            id, body["enabled"].get<bool>(), &err);
        resp.status = ok ? 200 : 400;
        nlohmann::json doc;
        doc["success"] = ok;
        doc["error"] = ok ? "" : err;
        const int64_t now = NowMs();
        for (const auto &s : m_storage->ListCommandSchedules()) {
            if (s.id == id) {
                doc["schedule"] = ScheduleToJson(s, now);
                break;
            }
        }
        resp.set_content(doc.dump(), "application/json");
    };
    m_server.Post("/command-schedules/enable", enable_schedule_handler);
    m_server.Post("/api/command-schedules/enable", enable_schedule_handler);

    // Recent command executions (scheduled + ad-hoc), newest first.
    auto runs_handler = [this, storage_ready](const httplib::Request &req,
                                              httplib::Response &resp) {
        if (!storage_ready(resp)) return;
        std::string agent_id = req.get_param_value("agent_id");
        std::string command_id = req.get_param_value("command_id");
        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
            } catch (...) {
                limit = 50;
            }
        }
        nlohmann::json doc;
        doc["runs"] = nlohmann::json::array();
        for (const auto &r : m_storage->ListCommandRuns(agent_id, command_id,
                                                        limit)) {
            doc["runs"].push_back(RunToJson(r));
        }
        resp.set_content(doc.dump(), "application/json");
    };
    m_server.Get("/command-runs", runs_handler);
    m_server.Get("/api/command-runs", runs_handler);

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
