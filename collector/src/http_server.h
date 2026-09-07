#pragma once

#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <httplib.h>

#include "agent_rpc.h"
#include "diagnostic.grpc.pb.h"

namespace pudimcollector {

class AgentDist;
class AgentRegistry;
class MetricsServiceImpl;
class TimescaleStorage;
namespace alerting { class AlertManager; }
namespace kafka { class KafkaProducer; }

// HTTP API for the dashboard, the Prometheus /metrics scrape endpoint and the
// self-hosted agent download. Every route is registered in the constructor;
// dependencies are injected instead of read from process globals so the
// handlers stay testable.
//
// Every dashboard-facing route is served at BOTH /path and /api/path so it
// works through reverse proxies that strip the /api prefix (legacy) and those
// that pass it through. Prometheus scraping uses /metrics directly.
class HttpServer {
public:
    HttpServer(const AgentRegistry &registry,
               std::shared_ptr<TimescaleStorage> storage,
               std::shared_ptr<MetricsServiceImpl> metrics_service,
               std::shared_ptr<alerting::AlertManager> alert_manager,
               std::shared_ptr<kafka::KafkaProducer> kafka_producer,
               AgentDist &agent_dist,
               TlsOptions tls);

    ~HttpServer();

    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // Binds and serves on `addr` ("host:port") in a background thread until
    // Stop() is called.
    void Start(const std::string &addr);

    // Stops the listener and joins the serving thread.
    void Stop();

private:
    // Prometheus /metrics text payload for this process.
    std::string FormatPrometheusMetrics() const;

    // Looks up the agent's advertised diagnostic endpoint and dials it.
    // Returns nullptr (after writing a 404 JSON body) when the agent has not
    // advertised one.
    std::unique_ptr<pudimnetmon::DiagnosticService::Stub> PrepareAgentCall(
        const std::string &agent_id, httplib::Response &resp) const;

    // Writes the standard 502 JSON body for a failed agent RPC.
    void SendAgentRpcError(httplib::Response &resp,
                           const grpc::Status &status) const;

    httplib::Server m_server;
    std::thread m_thread;

    const AgentRegistry &m_registry;
    std::shared_ptr<TimescaleStorage> m_storage;
    std::shared_ptr<MetricsServiceImpl> m_metrics_service;
    std::shared_ptr<alerting::AlertManager> m_alert_manager;
    std::shared_ptr<kafka::KafkaProducer> m_kafka_producer;
    AgentDist &m_agent_dist;
    TlsOptions m_tls;
};

} // namespace pudimcollector
