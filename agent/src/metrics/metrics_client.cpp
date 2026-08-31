#include <chrono>
#include <string>

#include <grpcpp/grpcpp.h>

#include "logger.h"
#include "metrics_client.h"

using grpc::ClientContext;
using grpc::Status;
using pudimnetmon::MetricsBatch;
using pudimnetmon::MetricsResponse;
using pudimnetmon::MetricsService;

namespace pudimagent {

MetricsClient::MetricsClient(
    const std::string &endpoint,
    std::shared_ptr<grpc::ChannelCredentials> creds)
    : m_stub(MetricsService::NewStub(
          grpc::CreateChannel(endpoint,
                              creds ? creds
                                    : grpc::InsecureChannelCredentials()))) {}

bool MetricsClient::SendBatch(const MetricsBatch &batch,
                              const std::string &traceparent) {
    MetricsResponse resp;
    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    if (!traceparent.empty()) {
        ctx.AddMetadata("traceparent", traceparent);
    }

    Status status = m_stub->SendMetrics(&ctx, batch, &resp);
    m_backpressure = false;
    if (status.ok()) {
        for (const auto &md : ctx.GetServerTrailingMetadata()) {
            if (md.first == "x-overloaded" && md.second == "true") {
                m_backpressure = true;
            }
        }
    }

    if (!status.ok()) {
        LOG_ERROR("SendMetrics failed: " + status.error_message() +
                  " (code=" + std::to_string(status.error_code()) + ")");
        return false;
    }
    return resp.ack();
}

bool MetricsClient::StreamMetrics(
    const std::string &agent_id,
    const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics,
    const std::string &traceparent) {
    MetricsResponse resp;
    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    ctx.AddMetadata("x-agent-id", agent_id);
    if (!traceparent.empty()) {
        ctx.AddMetadata("traceparent", traceparent);
    }

    auto writer = m_stub->StreamMetrics(&ctx, &resp);
    for (const auto &m : metrics) {
        if (!writer->Write(m)) {
            LOG_ERROR("StreamMetrics: write failed (stream cancelled?)");
            break;
        }
    }
    writer->WritesDone();

    Status status = writer->Finish();
    m_backpressure = false;
    if (status.ok()) {
        for (const auto &md : ctx.GetServerTrailingMetadata()) {
            if (md.first == "x-overloaded" && md.second == "true") {
                m_backpressure = true;
            }
        }
    }
    if (!status.ok()) {
        LOG_ERROR("StreamMetrics failed: " + status.error_message() +
                  " (code=" + std::to_string(status.error_code()) + ")");
        return false;
    }
    return resp.ack();
}

} // namespace pudimagent