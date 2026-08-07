// End-to-end gRPC integration test:
//   in-process collector (MetricsServiceImpl) + real agent MetricsClient,
//   backed by TimescaleStorage. Exercises both the unary SendMetrics path and
//   the client-streaming StreamMetrics path (with the "x-agent-id" metadata
//   header). Skips gracefully when no TimescaleDB is reachable.
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "metrics.grpc.pb.h"
#include "metrics.pb.h"
#include "metrics_client.h"
#include "metrics_service.h"
#include "storage/timescale_storage.h"

using grpc::InsecureChannelCredentials;
using grpc::InsecureServerCredentials;
using grpc::Server;
using grpc::ServerBuilder;
using pudimnetmon::CheckType;
using pudimnetmon::Metric;
using pudimnetmon::MetricsBatch;
using pudimnetmon::MetricsService;
using pudimagent::MetricsClient;
using pudimcollector::MetricsServiceImpl;
using pudimcollector::StorageConfig;
using pudimcollector::TimescaleStorage;

namespace {

Metric MakeMetric(CheckType type, const std::string &target,
                  double latency_ms, uint64_t seq) {
    Metric m;
    m.set_check_type(type);
    m.set_target(target);
    m.set_latency_ms(latency_ms);
    m.set_success(true);
    m.set_seq(seq);
    m.set_monotonic_us(static_cast<int64_t>(seq) * 1000);
    return m;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

StorageConfig ConfigFromEnv() {
    StorageConfig cfg;
    cfg.host = std::getenv("PUDIM_TEST_DB_HOST")
                   ? std::getenv("PUDIM_TEST_DB_HOST")
                   : "localhost";
    cfg.port = 5432;
    cfg.dbname = "pudimnetmon";
    cfg.user = "pudim";
    cfg.password = "pudim";
    return cfg;
}

// Picks an ephemeral free port by binding a socket to port 0. (gRPC <1.60
// does not expose Server::GetPort(), so we reserve the port ourselves.)
int PickFreePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

} // anonymous namespace

int main() {
    auto storage = std::make_shared<TimescaleStorage>(ConfigFromEnv());
    if (!storage->Connect()) {
        std::cout << "SKIP: TimescaleDB not available, skipping E2E gRPC tests\n";
        return 0;
    }

    // Start an in-process collector gRPC server on an ephemeral port.
    MetricsServiceImpl metrics_service(storage);

    int port = PickFreePort();
    assert(port > 0);
    std::string endpoint = "localhost:" + std::to_string(port);
    std::cout << "E2E collector listening on " << endpoint << "\n";

    ServerBuilder builder;
    builder.AddListeningPort(endpoint, InsecureServerCredentials());
    builder.RegisterService(&metrics_service);
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024);
    std::unique_ptr<Server> server = builder.BuildAndStart();
    assert(server != nullptr);

    MetricsClient client(endpoint);

    // --- 1. Unary SendMetrics path ---
    {
        MetricsBatch batch;
        batch.set_agent_id("e2e-unary-agent");
        batch.set_timestamp_unix_ms(NowMs());
        *batch.add_metrics() =
            MakeMetric(CheckType::CHECK_TYPE_DNS_RESOLUTION, "example.com", 12.5, 1);
        *batch.add_metrics() =
            MakeMetric(CheckType::CHECK_TYPE_TCP_CONNECT, "example.com:443", 30.1, 2);

        bool ok = client.SendBatch(batch);
        assert(ok);
        std::cout << "PASS: unary SendMetrics ACKed\n";
    }

    // --- 2. Client-streaming StreamMetrics path (agent_id via metadata) ---
    {
        MetricsBatch batch;
        batch.set_agent_id("e2e-stream-agent");
        batch.set_timestamp_unix_ms(NowMs());
        *batch.add_metrics() =
            MakeMetric(CheckType::CHECK_TYPE_DNS_RESOLUTION, "example.com", 8.2, 1);
        *batch.add_metrics() =
            MakeMetric(CheckType::CHECK_TYPE_HTTP_REQUEST, "https://example.com", 45.0, 2);

        bool ok = client.StreamMetrics(batch.agent_id(), batch.metrics());
        assert(ok);
        std::cout << "PASS: streaming StreamMetrics ACKed\n";
    }

    // --- 3. Verify both paths persisted to storage ---
    {
        std::string json = storage->QueryMetricsJson("e2e-unary-agent", "", 300);
        assert(json.find("e2e-unary-agent") != std::string::npos);
        assert(json.find("dns_resolution") != std::string::npos);
        std::cout << "PASS: unary metrics persisted: " << json.substr(0, 120) << "...\n";

        json = storage->QueryMetricsJson("e2e-stream-agent", "", 300);
        assert(json.find("e2e-stream-agent") != std::string::npos);
        assert(json.find("http_request") != std::string::npos);
        std::cout << "PASS: streamed metrics persisted: " << json.substr(0, 120) << "...\n";

        assert(metrics_service.ReceivedMetrics() >= 4);
        assert(metrics_service.BatchesReceived() >= 2);
        std::cout << "PASS: collector counters — received="
                  << metrics_service.ReceivedMetrics()
                  << ", batches=" << metrics_service.BatchesReceived() << "\n";
    }

    // --- 4. Stream without the x-agent-id header is rejected ---
    {
        auto channel = grpc::CreateChannel(endpoint, InsecureChannelCredentials());
        auto stub = MetricsService::NewStub(channel);
        grpc::ClientContext ctx;
        pudimnetmon::MetricsResponse resp;
        auto writer = stub->StreamMetrics(&ctx, &resp);
        writer->Write(MakeMetric(CheckType::CHECK_TYPE_DNS_RESOLUTION, "x", 1.0, 1));
        writer->WritesDone();
        grpc::Status status = writer->Finish();
        assert(!status.ok());
        assert(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
        std::cout << "PASS: stream without x-agent-id rejected (code="
                  << status.error_code() << ")\n";
    }

    server->Shutdown();
    server->Wait();

    std::cout << "ALL E2E GRPC TESTS PASSED\n";
    return 0;
}
