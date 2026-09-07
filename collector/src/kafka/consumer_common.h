#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <librdkafka/rdkafkacpp.h>

#include "metrics.pb.h"

namespace pudimcollector::kafka {

// Shared "keep running" flag. Cleared by InstallSignalHandlers() on SIGINT or
// SIGTERM and read by ConsumeLoop().
extern std::atomic<bool> g_keep_running;

// Registers SIGINT/SIGTERM handlers that set g_keep_running = false.
void InstallSignalHandlers();

// Creates a subscribed consumer with at-least-once settings. When earliest is
// true, auto.offset.reset is set to earliest. Returns nullptr on failure.
std::unique_ptr<RdKafka::KafkaConsumer> CreateConsumer(
    const std::string &brokers, const std::string &topic,
    const std::string &group, bool earliest, std::string &error);

// Per-consumer counters exposed via Prometheus.
struct ConsumerStats {
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> batches_processed{0};
    std::atomic<uint64_t> parse_errors{0};
    std::atomic<uint64_t> handler_errors{0};
    std::atomic<uint64_t> consumer_errors{0};
};

// Handles one deserialized batch. Returning false leaves the offset
// uncommitted so the message is redelivered.
using BatchHandler =
    std::function<bool(const pudimnetmon::MetricsBatch &batch)>;

// Consumes from `consumer` and invokes `handler` per message. Commits the
// offset only on success and stops when g_keep_running turns false.
void ConsumeLoop(RdKafka::KafkaConsumer *consumer, const BatchHandler &handler,
                 ConsumerStats *stats);

// Total consumer lag across all assigned partitions.
uint64_t ComputeTotalLag(RdKafka::KafkaConsumer *consumer);

// Serves Prometheus /metrics on `addr` in a background thread. Returns the
// thread to join.
std::thread StartPrometheusEndpoint(
    const std::string &addr,
    const std::function<std::string()> &metrics_fn);

} // namespace pudimcollector::kafka
