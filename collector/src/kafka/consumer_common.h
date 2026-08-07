#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <librdkafka/rdkafkacpp.h>

#include "metrics.pb.h"

namespace pudimcollector::kafka {

// Shared atomic "keep running" flag. Cleared by InstallSignalHandlers() on
// SIGINT/SIGTERM; consumed by ConsumeLoop().
extern std::atomic<bool> g_keep_running;

// Registers SIGINT/SIGTERM handlers that set g_keep_running = false.
void InstallSignalHandlers();

// Creates a subscribed KafkaConsumer with at-least-once settings
// (enable.auto.commit=false, enable.auto.offset.store=false). When `earliest`
// is true, auto.offset.reset=earliest (used by tests/fresh replays).
// Returns nullptr and sets `error` on failure.
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

// Callback for each deserialized MetricsBatch. Return false to skip committing
// the offset (message will be redelivered → at-least-once).
using BatchHandler =
    std::function<bool(const pudimnetmon::MetricsBatch &batch)>;

// Blocks consuming from `consumer`, deserializing each message and invoking
// `handler`. Commits the offset only on successful handler results. Stops when
// g_keep_running becomes false. `stats` is updated throughout.
void ConsumeLoop(RdKafka::KafkaConsumer *consumer, const BatchHandler &handler,
                 ConsumerStats *stats);

// Total consumer lag across all assigned partitions
// (sum of high-watermark − current position).
uint64_t ComputeTotalLag(RdKafka::KafkaConsumer *consumer);

// Serves a minimal Prometheus /metrics endpoint on `addr` in a background
// thread. `metrics_fn` returns the exposition text. Caller joins the thread.
std::thread StartPrometheusEndpoint(
    const std::string &addr,
    const std::function<std::string()> &metrics_fn);

} // namespace pudimcollector::kafka
