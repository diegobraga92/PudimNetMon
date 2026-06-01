#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include <httplib.h>

#include "heartbeat.grpc.pb.h"

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
            m_agents[req.agent_id()] = entry;
            logger::emit("info", "New agent registered", req.agent_id());
        } else {
            it->second.last_seen_unix_ms = now;
            it->second.interval_ms = req.interval_ms();
            it->second.version = req.version();
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
    return out;
}

// --------------------------------------------
// Main
// --------------------------------------------
int main(int argc, char **argv) {
    std::string grpc_addr = "0.0.0.0:50051";
    std::string http_addr = "0.0.0.0:8080";

    // Simple CLI parsing
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--grpc-addr" && i + 1 < argc) {
            grpc_addr = argv[++i];
        } else if (arg == "--http-addr" && i + 1 < argc) {
            http_addr = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Usage: pudim-collector [options]\n"
                      << "  --grpc-addr  gRPC listen address (default: 0.0.0.0:50051)\n"
                      << "  --http-addr  HTTP listen address (default: 0.0.0.0:8080)\n"
                      << "  --help       Show this help\n";
            return 0;
        }
    }

    logger::emit("info", "Collector starting up");
    logger::emit("info", "gRPC endpoint: " + grpc_addr);
    logger::emit("info", "HTTP endpoint: " + http_addr);

    // Setup signal handlers
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    // Start gRPC server
    AgentServiceImpl agent_service;
    ServerBuilder builder;
    builder.AddListeningPort(grpc_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&agent_service);
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
        resp.set_content(R"({"status":"ok","component":"collector"})", "application/json");
    });

    http_server.Get("/agents", [](const httplib::Request &, httplib::Response &resp) {
        resp.set_content(s_registry.DumpAgents(), "application/json");
    });

    http_server.Get("/metrics", [](const httplib::Request &, httplib::Response &resp) {
        resp.set_content(format_prometheus_metrics(), "text/plain; version=0.0.4");
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