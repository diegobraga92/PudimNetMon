#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "metrics.grpc.pb.h"

namespace pudimagent {

class MetricsClient {
public:
    explicit MetricsClient(
        const std::string &endpoint,
        std::shared_ptr<grpc::ChannelCredentials> creds = nullptr);

    bool SendBatch(const pudimnetmon::MetricsBatch &batch,
                   const std::string &traceparent = "");

    bool StreamMetrics(
        const std::string &agent_id,
        const google::protobuf::RepeatedPtrField<pudimnetmon::Metric> &metrics,
        const std::string &traceparent = "");

    bool BackpressureSignalled() const { return m_backpressure; }

private:
    std::unique_ptr<pudimnetmon::MetricsService::Stub> m_stub;
    mutable std::atomic<bool> m_backpressure{false};
};

} // namespace pudimagent