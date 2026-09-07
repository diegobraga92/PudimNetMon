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

// Where validated metrics go next.
enum class StorageMode {
    Direct,  // write to TimescaleDB and run alerting in-process
    Kafka,   // produce to Kafka and leave storage and alerting to consumers
};

// gRPC service for MetricsService. Direct mode writes to TimescaleStorage and
// evaluates alerts in-process. Kafka mode produces every batch to Kafka.
class MetricsServiceImpl final : public pudimnetmon::MetricsService::Service {
public:
    explicit MetricsServiceImpl(
        std::shared_ptr<TimescaleStorage> storage,
        std::shared_ptr<alerting::AlertManager> alerts = nullptr,
        std::shared_ptr<kafka::KafkaProducer> producer = nullptr,
        StorageMode mode = StorageMode::Direct,
        int64_t skew_threshold_ms = 5000,
        int64_t backpressure_threshold_ms = 1000);

    grpc::Status SendMetrics(
        grpc::ServerContext *ctx,
        const pudimnetmon::MetricsBatch *request,
        pudimnetmon::MetricsResponse *response) override;

    grpc::Status StreamMetrics(
        grpc::ServerContext *ctx,
        grpc::ServerReader<pudimnetmon::Metric> *reader,
        pudimnetmon::MetricsResponse *response) override;

    // True in Kafka mode.
    bool KafkaEnabled() const { return m_mode == StorageMode::Kafka; }

    // Clock-skew warnings observed on the unary ingest path.
    uint64_t SkewWarnings() const { return m_skew_warnings.load(); }

    // "x-overloaded" backpressure signals sent to agents.
    uint64_t BackpressureSignalsSent() const {
        return m_backpressure_signals_sent.load();
    }

private:
    // Ingests one batch. Forwards traceparent to Kafka and fills elapsed_ms
    // with the ingest duration.
    bool IngestBatch(const pudimnetmon::MetricsBatch &batch,
                     const std::string &traceparent,
                     int64_t &elapsed_ms);

    std::shared_ptr<TimescaleStorage> m_storage;
    std::shared_ptr<alerting::AlertManager> m_alerts;
    std::shared_ptr<kafka::KafkaProducer> m_producer;
    StorageMode m_mode;
    int64_t m_skew_threshold_ms;
    int64_t m_backpressure_threshold_ms;
    std::atomic<uint64_t> m_received_metrics{0};
    std::atomic<uint64_t> m_rejected_metrics{0};
    std::atomic<uint64_t> m_batches_received{0};
    std::atomic<uint64_t> m_skew_warnings{0};
    std::atomic<uint64_t> m_backpressure_signals_sent{0};

public:
    uint64_t ReceivedMetrics() const { return m_received_metrics.load(); }
    uint64_t RejectedMetrics() const { return m_rejected_metrics.load(); }
    uint64_t BatchesReceived() const { return m_batches_received.load(); }
};

} // namespace pudimcollector