#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "metrics.grpc.pb.h"
#include "storage/timescale_storage.h"
#include "alerting/alert_manager.h"
#include "kafka/producer.h"

namespace pudimcollector {

// Where ingested metrics go after validation (ADR 004).
enum class StorageMode {
    Direct,  // write to TimescaleDB + in-process alerting (default, Phases 1-2)
    Kafka,   // produce to Kafka; consumers own storage + alerting (Phase 3)
};

// gRPC service implementation for MetricsService. In Direct mode it writes to
// TimescaleStorage and evaluates alerts in-process; in Kafka mode it produces
// every batch to Kafka via KafkaProducer instead.
class MetricsServiceImpl final : public pudimnetmon::MetricsService::Service {
public:
    explicit MetricsServiceImpl(
        std::shared_ptr<TimescaleStorage> storage,
        std::shared_ptr<alerting::AlertManager> alerts = nullptr,
        std::shared_ptr<kafka::KafkaProducer> producer = nullptr,
        StorageMode mode = StorageMode::Direct);

    grpc::Status SendMetrics(
        grpc::ServerContext *ctx,
        const pudimnetmon::MetricsBatch *request,
        pudimnetmon::MetricsResponse *response) override;

    grpc::Status StreamMetrics(
        grpc::ServerContext *ctx,
        grpc::ServerReader<pudimnetmon::Metric> *reader,
        pudimnetmon::MetricsResponse *response) override;

    // True when Kafka mode is active.
    bool KafkaEnabled() const { return m_mode == StorageMode::Kafka; }

private:
    bool IngestBatch(const pudimnetmon::MetricsBatch &batch);

    std::shared_ptr<TimescaleStorage> m_storage;
    std::shared_ptr<alerting::AlertManager> m_alerts;
    std::shared_ptr<kafka::KafkaProducer> m_producer;
    StorageMode m_mode;
    std::atomic<uint64_t> m_received_metrics{0};
    std::atomic<uint64_t> m_rejected_metrics{0};
    std::atomic<uint64_t> m_batches_received{0};

public:
    uint64_t ReceivedMetrics() const { return m_received_metrics.load(); }
    uint64_t RejectedMetrics() const { return m_rejected_metrics.load(); }
    uint64_t BatchesReceived() const { return m_batches_received.load(); }
};

} // namespace pudimcollector