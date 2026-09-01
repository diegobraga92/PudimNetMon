#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include <httplib.h>

#include "consumer_common.h"

namespace pudimcollector::kafka {

std::atomic<bool> g_keep_running{true};

namespace {

void HandleSignal(int sig) {
    const char *name = (sig == SIGTERM) ? "SIGTERM" : "SIGINT";
    std::cout << "{\"level\":\"info\",\"component\":\"consumer\",\"message\":\""
              << "Received " << name << ", shutting down\"}" << std::endl;
    g_keep_running = false;
}

} // anonymous namespace

void InstallSignalHandlers() {
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGINT, HandleSignal);
}

std::unique_ptr<RdKafka::KafkaConsumer> CreateConsumer(
    const std::string &brokers, const std::string &topic,
    const std::string &group, bool earliest, std::string &error) {
    auto conf = std::unique_ptr<RdKafka::Conf>(
        RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (!conf) {
        error = "failed to create RdKafka global conf";
        return nullptr;
    }

    auto set_conf = [&](const std::string &key, const std::string &value) -> bool {
        std::string err;
        if (conf->set(key, value, err) != RdKafka::Conf::CONF_OK) {
            error = "kafka conf '" + key + "': " + err;
            return false;
        }
        return true;
    };

    if (!set_conf("bootstrap.servers", brokers)) return nullptr;
    if (!set_conf("group.id", group)) return nullptr;
    // At-least-once: we commit offsets manually only after successful handling.
    if (!set_conf("enable.auto.commit", "false")) return nullptr;
    if (!set_conf("enable.auto.offset.store", "false")) return nullptr;
    if (!set_conf("auto.offset.reset", earliest ? "earliest" : "largest")) return nullptr;
    // Limit message size to our batch limit (matches collector 4MB cap).
    if (!set_conf("max.partition.fetch.bytes", "4194304")) return nullptr;

    std::unique_ptr<RdKafka::KafkaConsumer> consumer(
        RdKafka::KafkaConsumer::create(conf.get(), error));
    if (!consumer) {
        error = "failed to create Kafka consumer: " + error;
        return nullptr;
    }

    RdKafka::ErrorCode rc = consumer->subscribe({topic});
    if (rc != RdKafka::ERR_NO_ERROR) {
        error = "failed to subscribe to '" + topic + "': " + RdKafka::err2str(rc);
        return nullptr;
    }
    return consumer;
}

void ConsumeLoop(RdKafka::KafkaConsumer *consumer, const BatchHandler &handler,
                 ConsumerStats *stats) {
    constexpr int kPollMs = 100;

    while (g_keep_running) {
        std::unique_ptr<RdKafka::Message> msg(consumer->consume(kPollMs));
        if (!msg) continue;

        switch (msg->err()) {
            case RdKafka::ERR_NO_ERROR: {
                stats->messages_received++;
                if (msg->payload() == nullptr || msg->len() <= 0) {
                    stats->parse_errors++;
                    consumer->commitSync(msg.get());  // skip empty message
                    continue;
                }
                pudimnetmon::MetricsBatch batch;
                if (!batch.ParseFromArray(msg->payload(), msg->len())) {
                    stats->parse_errors++;
                    std::cerr << "Kafka: unparseable MetricsBatch (poison pill), "
                                 "skipping + committing offset\n";
                    consumer->commitSync(msg.get());  // avoid infinite redelivery
                    continue;
                }

                // Forward the W3C trace context (Kafka header) to the
                // handler log so the consumer's work can be correlated with the
                // agent→collector trace.
                std::string traceparent;
                if (const RdKafka::Headers *hdrs = msg->headers()) {
                    for (const auto &h : hdrs->get_all()) {
                        if (h.key() == "traceparent" && h.value()) {
                            traceparent.assign(static_cast<const char *>(h.value()),
                                               h.value_size());
                            break;
                        }
                    }
                }
                if (!traceparent.empty()) {
                    std::cout << "{\"level\":\"info\",\"component\":\"consumer\","
                                 "\"message\":\"consume_batch\",\"agent_id\":\""
                              << batch.agent_id()
                              << "\",\"traceparent\":\"" << traceparent << "\"}"
                              << std::endl;
                }

                bool ok = handler(batch);
                if (ok) {
                    stats->batches_processed++;
                    consumer->commitSync(msg.get());  // at-least-once: commit
                                                      // only after success
                } else {
                    stats->handler_errors++;
                    // Do NOT commit → redelivered after rebalance/restart.
                }
                break;
            }
            case RdKafka::ERR__PARTITION_EOF:
            case RdKafka::ERR__TIMED_OUT:
                break;  // normal no-data conditions
            default:
                stats->consumer_errors++;
                std::cerr << "Kafka consume error: " << msg->errstr() << "\n";
                break;
        }
    }
}

uint64_t ComputeTotalLag(RdKafka::KafkaConsumer *consumer) {
    std::vector<RdKafka::TopicPartition *> partitions;
    if (consumer->assignment(partitions) != RdKafka::ERR_NO_ERROR) {
        return 0;
    }

    uint64_t total = 0;
    for (auto *tp : partitions) {
        int64_t low = 0, high = 0;
        if (consumer->query_watermark_offsets(tp->topic(), tp->partition(),
                                              &low, &high, 3000) ==
            RdKafka::ERR_NO_ERROR) {
            int64_t pos = 0;
            std::vector<RdKafka::TopicPartition *> one = {tp};
            if (consumer->position(one) == RdKafka::ERR_NO_ERROR && tp->offset() >= 0) {
                pos = tp->offset();
            }
            if (high > pos) {
                total += static_cast<uint64_t>(high - pos);
            }
        }
    }
    for (auto *tp : partitions) {
        delete tp;
    }
    return total;
}

std::thread StartPrometheusEndpoint(
    const std::string &addr,
    const std::function<std::string()> &metrics_fn) {
    return std::thread([addr, metrics_fn]() {
        httplib::Server srv;
        srv.Get("/metrics", [&](const httplib::Request &, httplib::Response &resp) {
            resp.set_content(metrics_fn(), "text/plain; version=0.0.4");
        });
        srv.Get("/health", [&](const httplib::Request &, httplib::Response &resp) {
            resp.set_content("{\"status\":\"ok\"}", "application/json");
        });

        auto colon = addr.find_last_of(':');
        std::string host = addr.substr(0, colon);
        int port = std::stoi(addr.substr(colon + 1));
        std::cout << "{\"level\":\"info\",\"component\":\"consumer\","
                     "\"message\":\"prometheus endpoint on " << addr << "\"}"
                  << std::endl;
        srv.listen(host.c_str(), port);
    });
}

} // namespace pudimcollector::kafka
