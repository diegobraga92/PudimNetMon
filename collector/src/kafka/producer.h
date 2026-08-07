#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "metrics.pb.h"

namespace pudimcollector::kafka {

// Thin wrapper around librdkafka's producer. Serializes MetricsBatch as
// protobuf binary and keys each message by agent_id so per-agent ordering is
// preserved within a partition. Delivery is asynchronous; Produce() returns
// true when the message is accepted into librdkafka's queue.
class KafkaProducer {
public:
    KafkaProducer();
    ~KafkaProducer();

    KafkaProducer(const KafkaProducer &) = delete;
    KafkaProducer &operator=(const KafkaProducer &) = delete;

    // Configures and connects to the broker cluster. Returns false on failure.
    bool Connect(const std::string &brokers, const std::string &topic,
                 std::string &error);

    // Enqueues a batch for delivery. Returns true if accepted into the queue.
    // `traceparent` (optional W3C header) is attached as a Kafka message header.
    bool Produce(const pudimnetmon::MetricsBatch &batch,
                 const std::string &traceparent = "");

    // Blocks until queued messages are flushed or timeout_ms elapses.
    void Flush(int timeout_ms = 10000);

    uint64_t ProducedTotal() const { return m_produced_total.load(); }
    uint64_t DeliverySuccesses() const { return m_delivery_successes.load(); }
    uint64_t DeliveryFailures() const { return m_delivery_failures.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_topic;
    std::atomic<uint64_t> m_produced_total{0};
    std::atomic<uint64_t> m_delivery_successes{0};
    std::atomic<uint64_t> m_delivery_failures{0};
};

} // namespace pudimcollector::kafka
