#include <chrono>
#include <iostream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "metrics_client.h"

using grpc::ClientContext;
using grpc::Status;
using pudimnetmon::MetricsBatch;
using pudimnetmon::MetricsResponse;
using pudimnetmon::MetricsService;

namespace pudimagent {

MetricsClient::MetricsClient(const std::string &endpoint)
    : m_stub(MetricsService::NewStub(
          grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()))) {}

bool MetricsClient::SendBatch(const MetricsBatch &batch) {
    MetricsResponse resp;
    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

    Status status = m_stub->SendMetrics(&ctx, batch, &resp);

    if (!status.ok()) {
        std::cerr << "SendMetrics failed: " << status.error_message()
                  << " (code=" << status.error_code() << ")\n";
        return false;
    }
    return resp.ack();
}

bool MetricsClient::StreamMetrics(
    const std::string &agent_id,
    const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics) {
    MetricsResponse resp;
    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    ctx.AddMetadata("x-agent-id", agent_id);

    auto writer = m_stub->StreamMetrics(&ctx, &resp);
    for (const auto &m : metrics) {
        if (!writer->Write(m)) {
            std::cerr << "StreamMetrics: write failed (stream cancelled?)\n";
            break;
        }
    }
    writer->WritesDone();

    Status status = writer->Finish();
    if (!status.ok()) {
        std::cerr << "StreamMetrics failed: " << status.error_message()
                  << " (code=" << status.error_code() << ")\n";
        return false;
    }
    return resp.ack();
}

} // namespace pudimagent