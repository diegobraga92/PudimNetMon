#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "metrics.grpc.pb.h"

namespace pudimagent {

// gRPC client for sending metric batches to the collector.
class MetricsClient {
public:
    explicit MetricsClient(
        const std::string &endpoint,
        std::shared_ptr<grpc::ChannelCredentials> creds = nullptr);

    // Sends a batch of metrics. Returns true if the collector ACKed.
    bool SendBatch(const pudimnetmon::MetricsBatch &batch,
                   const std::string &traceparent = "");

    // Streams a batch of metrics to the collector using the client-streaming
    // RPC. The agent_id is transmitted via the "x-agent-id" gRPC metadata
    // header. Returns true if the collector ACKed.
    bool StreamMetrics(
        const std::string &agent_id,
        const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics,
        const std::string &traceparent = "");

    // True if the collector signalled overload on the last call
    // (gRPC response metadata "x-overloaded").
    bool BackpressureSignalled() const { return m_backpressure; }

private:
    std::unique_ptr<pudimnetmon::MetricsService::Stub> m_stub;
    mutable std::atomic<bool> m_backpressure{false};
};

} // namespace pudimagent