// Kafka round-trip integration test: produces a MetricsBatch via KafkaProducer
// and consumes it back with a KafkaConsumer (consumer_common), verifying the
// serialized batch arrives intact. Skips gracefully when no broker is reachable
// (defaults to localhost:9092, override with PUDIM_TEST_KAFKA_BROKERS).
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <unistd.h>

#include "metrics.pb.h"
#include "kafka/consumer_common.h"
#include "kafka/producer.h"

using pudimnetmon::CheckType;
using pudimnetmon::Metric;
using pudimnetmon::MetricsBatch;
using pudimcollector::kafka::ConsumerStats;
using pudimcollector::kafka::CreateConsumer;
using pudimcollector::kafka::KafkaProducer;

int main() {
    const char *env = std::getenv("PUDIM_TEST_KAFKA_BROKERS");
    std::string brokers = env ? env : "localhost:9092";
    const std::string topic = "network.metrics";
    const std::string group = "pudim-e2e-" + std::to_string(getpid());

    // --- 1. Produce a batch ---
    KafkaProducer producer;
    std::string err;
    if (!producer.Connect(brokers, topic, err)) {
        std::cout << "SKIP: Kafka not reachable at " << brokers
                  << " (" << err << ")\n";
        return 0;
    }

    MetricsBatch batch;
    batch.set_agent_id("e2e-kafka-agent");
    batch.set_timestamp_unix_ms(1786000000000LL);

    Metric m1;
    m1.set_check_type(CheckType::CHECK_TYPE_DNS_RESOLUTION);
    m1.set_target("example.com");
    m1.set_latency_ms(12.5);
    m1.set_success(true);
    m1.set_seq(1);
    *batch.add_metrics() = m1;

    Metric m2;
    m2.set_check_type(CheckType::CHECK_TYPE_TCP_CONNECT);
    m2.set_target("example.com:443");
    m2.set_latency_ms(30.1);
    m2.set_success(true);
    m2.set_seq(2);
    *batch.add_metrics() = m2;

    bool ok = producer.Produce(batch);
    assert(ok);
    producer.Flush(10000);  // ensure delivery
    assert(producer.DeliveryFailures() == 0);
    assert(producer.ProducedTotal() == 1);
    std::cout << "PASS: produced batch to " << topic << " (delivery failures="
              << producer.DeliveryFailures() << ")\n";

    // --- 2. Consume it back with a fresh group reading from earliest ---
    auto consumer = CreateConsumer(brokers, topic, group, /*earliest=*/true, err);
    assert(consumer != nullptr);

    std::atomic<bool> matched{false};
    ConsumerStats stats;
    pudimcollector::kafka::BatchHandler handler =
        [&](const MetricsBatch &b) -> bool {
            if (b.agent_id() == "e2e-kafka-agent" &&
                b.metrics_size() == 2 &&
                b.metrics(0).seq() == 1 &&
                b.metrics(0).has_latency_ms() &&
                b.metrics(0).latency_ms() == 12.5) {
                matched = true;
                std::cout << "PASS: consumed batch matches produced payload\n";
            } else {
                std::cout << "WARN: consumed a different batch (agent_id="
                          << b.agent_id() << ")\n";
            }
            return true;
        };

    // Poll until we see our batch or a timeout elapses.
    constexpr int kTimeoutMs = 15000;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kTimeoutMs);
    while (!matched && std::chrono::steady_clock::now() < deadline) {
        std::unique_ptr<RdKafka::Message> msg(consumer->consume(100));
        if (!msg || msg->err() != RdKafka::ERR_NO_ERROR) continue;
        MetricsBatch b;
        if (b.ParseFromArray(msg->payload(), msg->len())) {
            handler(b);
            consumer->commitSync(msg.get());
        }
    }

    assert(matched);
    consumer->close();

    std::cout << "ALL E2E KAFKA TESTS PASSED\n";
    return 0;
}
