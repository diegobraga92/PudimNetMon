#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>
#include <getopt.h>

#include <grpcpp/grpcpp.h>
#include "heartbeat.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using pudimnetmon::AgentService;
using pudimnetmon::HeartbeatRequest;
using pudimnetmon::HeartbeatResponse;

// --------------------------------------------
// Globals (must be before logger macros)
// --------------------------------------------
static std::atomic<bool> s_running{true};
static std::string s_node_id;
static std::string s_trace_id;

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
                           (sig == SIGINT)  ? "SIGINT" : "UNKNOWN";
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

    bool SendHeartbeat(int interval_ms, const std::string &version) {
        HeartbeatRequest req;
        req.set_agent_id(s_node_id);
        req.set_timestamp_unix_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        req.set_interval_ms(interval_ms);
        req.set_version(version);

        HeartbeatResponse resp;
        ClientContext ctx;

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

    // Parse CLI flags using getopt
    static struct option long_options[] = {
        {"collector-endpoint", required_argument, nullptr, 'c'},
        {"node-id",            required_argument, nullptr, 'n'},
        {"interval",           required_argument, nullptr, 'i'},
        {"trace-id",           required_argument, nullptr, 't'},
        {"version",            required_argument, nullptr, 'v'},
        {"help",               no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "c:n:i:t:v:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'c': collector_endpoint = optarg; break;
            case 'n': node_id = optarg; break;
            case 'i': interval_ms = std::stoi(optarg); break;
            case 't': trace_id = optarg; break;
            case 'v': version = optarg; break;
            case 'h':
                std::cout << "Usage: pudim-agent [options]\n"
                          << "  -c, --collector-endpoint  Collector gRPC endpoint (default: localhost:50051)\n"
                          << "  -n, --node-id             Unique node identifier (default: agent-unknown)\n"
                          << "  -i, --interval            Heartbeat interval in ms (default: 5000)\n"
                          << "  -t, --trace-id            Trace ID for request correlation\n"
                          << "  -v, --version             Agent version string\n"
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

    // Setup signal handlers
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    LOG_INFO("Agent starting up");
    LOG_INFO("Collector endpoint: " + collector_endpoint);
    LOG_INFO("Node ID: " + node_id);
    LOG_INFO("Interval: " + std::to_string(interval_ms) + "ms");
    LOG_INFO("Version: " + version);

    HeartbeatClient client(collector_endpoint);

    // Main loop
    while (s_running) {
        client.SendHeartbeat(interval_ms, version);

        // Sleep in small increments so we can respond to signals promptly
        int slept = 0;
        while (slept < interval_ms && s_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            slept += 100;
        }
    }

    LOG_INFO("Agent shut down gracefully");
    return 0;
}