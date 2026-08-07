#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "metrics.grpc.pb.h"
#include "storage/timescale_storage.h"
#include "alerting/alert_manager.h"

namespace pudimcollector {

// gRPC service implementation for MetricsService, backed by TimescaleStorage.
// Optionally evaluates metrics against alert rules after a successful write.
class MetricsServiceImpl final : public pudimnetmon::MetricsService::Service {
public:
    explicit MetricsServiceImpl(std::shared_ptr<TimescaleStorage> storage,
                                std::shared_ptr<alerting::AlertManager> alerts = nullptr);

    grpc::Status SendMetrics(
        grpc::ServerContext *ctx,
        const pudimnetmon::MetricsBatch *request,
        pudimnetmon::MetricsResponse *response) override;

    grpc::Status StreamMetrics(
        grpc::ServerContext *ctx,
        grpc::ServerReader<pudimnetmon::Metric> *reader,
        pudimnetmon::MetricsResponse *response) override;

private:
    std::shared_ptr<TimescaleStorage> m_storage;
    std::shared_ptr<alerting::AlertManager> m_alerts;
    std::atomic<uint64_t> m_received_metrics{0};
    std::atomic<uint64_t> m_rejected_metrics{0};
    std::atomic<uint64_t> m_batches_received{0};

public:
    uint64_t ReceivedMetrics() const { return m_received_metrics.load(); }
    uint64_t RejectedMetrics() const { return m_rejected_metrics.load(); }
    uint64_t BatchesReceived() const { return m_batches_received.load(); }
};

} // namespace pudimcollector