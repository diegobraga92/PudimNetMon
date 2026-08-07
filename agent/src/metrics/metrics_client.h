#pragma once

#include <memory>
#include <string>

#include "metrics.grpc.pb.h"

namespace pudimagent {

// gRPC client for sending metric batches to the collector.
class MetricsClient {
public:
    explicit MetricsClient(const std::string &endpoint);

    // Sends a batch of metrics. Returns true if the collector ACKed.
    bool SendBatch(const pudimnetmon::MetricsBatch &batch);

    // Streams a batch of metrics to the collector using the client-streaming
    // RPC. The agent_id is transmitted via the "x-agent-id" gRPC metadata
    // header. Returns true if the collector ACKed.
    bool StreamMetrics(
        const std::string &agent_id,
        const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics);

private:
    std::unique_ptr<pudimnetmon::MetricsService::Stub> m_stub;
};

} // namespace pudimagent