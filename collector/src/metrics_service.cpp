#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "metrics_service.h"

using grpc::ServerContext;
using grpc::Status;
using pudimnetmon::Metric;
using pudimnetmon::MetricsBatch;
using pudimnetmon::MetricsResponse;
using pudimnetmon::MetricsService;

namespace pudimcollector {

MetricsServiceImpl::MetricsServiceImpl(
    std::shared_ptr<TimescaleStorage> storage,
    std::shared_ptr<alerting::AlertManager> alerts,
    std::shared_ptr<kafka::KafkaProducer> producer,
    StorageMode mode)
    : m_storage(std::move(storage)),
      m_alerts(std::move(alerts)),
      m_producer(std::move(producer)),
      m_mode(mode) {}

bool MetricsServiceImpl::IngestBatch(const MetricsBatch &batch) {
    if (m_mode == StorageMode::Kafka) {
        // Produce to Kafka; storage + alerting are handled by consumers.
        if (!m_producer) return false;
        return m_producer->Produce(batch);
    }
    // Direct mode (Phases 1-2): write to TimescaleDB, then evaluate alerts.
    bool ok = m_storage->InsertMetrics(batch.agent_id(),
                                       batch.timestamp_unix_ms(),
                                       batch.metrics());
    if (ok && m_alerts) {
        m_alerts->Evaluate(batch.agent_id(), batch.metrics());
    }
    return ok;
}

Status MetricsServiceImpl::SendMetrics(
    ServerContext *ctx,
    const MetricsBatch *request,
    MetricsResponse *response) {
    m_batches_received++;

    if (!request || !response) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT,
                      "request and response must not be null");
    }

    if (request->agent_id().empty()) {
        m_rejected_metrics += request->metrics_size();
        response->set_ack(false);
        response->set_accepted_count(0);
        response->set_rejected_count(request->metrics_size());
        response->set_status_message("agent_id is required");
        return Status::OK;
    }

    if (request->metrics_size() == 0) {
        response->set_ack(true);
        response->set_accepted_count(0);
        response->set_rejected_count(0);
        response->set_status_message("empty batch accepted");
        return Status::OK;
    }

    m_received_metrics += request->metrics_size();

    if (!IngestBatch(*request)) {
        m_rejected_metrics += request->metrics_size();
        response->set_ack(false);
        response->set_accepted_count(0);
        response->set_rejected_count(request->metrics_size());
        response->set_status_message(
            m_mode == StorageMode::Kafka ? "kafka produce failed"
                                         : "storage write failed");
        return Status::OK;
    }

    response->set_ack(true);
    response->set_accepted_count(request->metrics_size());
    response->set_rejected_count(0);
    response->set_status_message("ok");

    return Status::OK;
}

Status MetricsServiceImpl::StreamMetrics(
    ServerContext *ctx,
    grpc::ServerReader<Metric> *reader,
    MetricsResponse *response) {
    m_batches_received++;

    if (!response) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT,
                      "response must not be null");
    }

    // The agent_id travels in the gRPC metadata header "x-agent-id"
    // (standard gRPC pattern: metadata for identity/routing, payload for data).
    std::string agent_id;
    const auto &md = ctx->client_metadata();
    auto it = md.find("x-agent-id");
    if (it != md.end()) {
        agent_id.assign(it->second.data(), it->second.size());
    }

    if (agent_id.empty()) {
        // Drain the stream so the client completes cleanly before we error out.
        Metric m;
        int64_t drained = 0;
        while (reader->Read(&m)) {
            m_received_metrics++;
            drained++;
        }
        m_rejected_metrics += drained;
        response->set_ack(false);
        response->set_accepted_count(0);
        response->set_rejected_count(drained);
        response->set_status_message("missing x-agent-id metadata header");
        return Status(grpc::StatusCode::INVALID_ARGUMENT,
                      "missing 'x-agent-id' metadata header");
    }

    // Collector-assigned receive timestamp is the source of truth for storage
    // (agent wall clock is preserved in metric.monotonic_us for debugging).
    int64_t batch_timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    constexpr int kFlushThreshold = 500;

    MetricsBatch batch;
    batch.set_agent_id(agent_id);
    batch.set_timestamp_unix_ms(batch_timestamp_ms);

    int64_t accepted = 0;
    int64_t rejected = 0;

    // Flushes accumulated metrics (to storage in Direct mode, to Kafka in
    // Kafka mode); returns false on failure.
    auto flush = [&]() -> bool {
        if (batch.metrics_size() == 0) return true;
        bool ok = IngestBatch(batch);
        if (ok) {
            accepted += batch.metrics_size();
        } else {
            rejected += batch.metrics_size();
        }
        batch.clear_metrics();
        return ok;
    };

    Metric metric;
    while (reader->Read(&metric)) {
        m_received_metrics++;
        *batch.add_metrics() = metric;

        if (batch.metrics_size() >= kFlushThreshold && !flush()) {
            // Drain the remaining stream so the client doesn't hang, then
            // surface the storage failure in the response.
            while (reader->Read(&metric)) {
                m_received_metrics++;
                rejected++;
            }
            m_rejected_metrics += rejected;
            response->set_ack(false);
            response->set_accepted_count(accepted);
            response->set_rejected_count(rejected);
            response->set_status_message("storage write failed");
            return Status(grpc::StatusCode::UNAVAILABLE, "storage write failed");
        }
    }

    // Flush any trailing metrics at end of stream.
    if (batch.metrics_size() > 0 && !flush()) {
        m_rejected_metrics += rejected;
        response->set_ack(false);
        response->set_accepted_count(accepted);
        response->set_rejected_count(rejected);
        response->set_status_message("storage write failed");
        return Status(grpc::StatusCode::UNAVAILABLE, "storage write failed");
    }

    m_rejected_metrics += rejected;

    response->set_ack(true);
    response->set_accepted_count(accepted);
    response->set_rejected_count(rejected);
    response->set_status_message("ok");

    return Status::OK;
}

} // namespace pudimcollector