// pudim-consumer-storage: reads MetricsBatch messages from Kafka and writes
// them to TimescaleDB using the same TimescaleStorage class as the collector's
// direct path. At-least-once: offsets are committed only after a successful
// insert; idempotent writes (ON CONFLICT DO NOTHING) make redelivery harmless.
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "kafka/consumer_common.h"
#include "storage/timescale_storage.h"

using pudimcollector::kafka::BatchHandler;
using pudimcollector::kafka::ComputeTotalLag;
using pudimcollector::kafka::ConsumeLoop;
using pudimcollector::kafka::ConsumerStats;
using pudimcollector::kafka::CreateConsumer;
using pudimcollector::kafka::InstallSignalHandlers;
using pudimcollector::kafka::StartPrometheusEndpoint;
using pudimcollector::StorageConfig;
using pudimcollector::TimescaleStorage;

namespace {

std::string NowMs() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
}

void Log(const std::string &level, const std::string &msg) {
    std::cout << "{\"timestamp\":" << NowMs()
              << ",\"level\":\"" << level << "\""
              << ",\"component\":\"consumer-storage\""
              << ",\"message\":\"" << msg << "\"}" << std::endl;
}

// Returns the value for --flag=VALUE or --flag VALUE, or "".
std::string Opt(const std::string &arg, const std::string &flag, int &i,
                int argc, char **argv) {
    const std::string prefix = flag + "=";
    if (arg.compare(0, prefix.size(), prefix) == 0) {
        return arg.substr(prefix.size());
    }
    if (arg == flag && i + 1 < argc) {
        return argv[++i];
    }
    return "";
}

} // anonymous namespace

int main(int argc, char **argv) {
    std::string brokers;
    std::string topic = "network.metrics";
    std::string group = "storage";
    std::string http_addr = "0.0.0.0:9091";
    StorageConfig db;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string v;
        if ((v = Opt(arg, "--kafka-brokers", i, argc, argv)) != "") {
            brokers = v;
        } else if ((v = Opt(arg, "--topic", i, argc, argv)) != "") {
            topic = v;
        } else if ((v = Opt(arg, "--group", i, argc, argv)) != "") {
            group = v;
        } else if ((v = Opt(arg, "--http-addr", i, argc, argv)) != "") {
            http_addr = v;
        } else if ((v = Opt(arg, "--db-host", i, argc, argv)) != "") {
            db.host = v;
        } else if ((v = Opt(arg, "--db-port", i, argc, argv)) != "") {
            db.port = std::stoi(v);
        } else if ((v = Opt(arg, "--db-name", i, argc, argv)) != "") {
            db.dbname = v;
        } else if ((v = Opt(arg, "--db-user", i, argc, argv)) != "") {
            db.user = v;
        } else if ((v = Opt(arg, "--db-password", i, argc, argv)) != "") {
            db.password = v;
        } else if (arg == "--help") {
            std::cout << "Usage: pudim-consumer-storage [options]\n"
                      << "  --kafka-brokers  Kafka bootstrap servers (e.g. localhost:9092)\n"
                      << "  --topic          Topic to consume (default: network.metrics)\n"
                      << "  --group          Consumer group (default: storage)\n"
                      << "  --http-addr      Prometheus listen address (default: 0.0.0.0:9091)\n"
                      << "  --db-host        TimescaleDB host (default: localhost)\n"
                      << "  --db-port        TimescaleDB port (default: 5432)\n"
                      << "  --db-name        TimescaleDB database (default: pudimnetmon)\n"
                      << "  --db-user        TimescaleDB user (default: pudim)\n"
                      << "  --db-password    TimescaleDB password (default: pudim)\n"
                      << "  --help           Show this help\n";
            return 0;
        }
    }

    if (brokers.empty()) {
        std::cerr << "FATAL: --kafka-brokers is required\n";
        return 1;
    }

    InstallSignalHandlers();
    Log("info", "starting storage consumer");

    auto storage = std::make_shared<TimescaleStorage>(db);
    if (!storage->Connect()) {
        Log("error", "TimescaleDB connection failed; will keep consuming and "
                     "retry inserts (uncommitted offsets)");
    }

    std::string err;
    auto consumer = CreateConsumer(brokers, topic, group, /*earliest=*/false, err);
    if (!consumer) {
        std::cerr << "FATAL: " << err << "\n";
        return 1;
    }
    Log("info", "subscribed to " + topic + " (group=" + group + ")");

    ConsumerStats stats;
    BatchHandler handler = [&](const pudimnetmon::MetricsBatch &batch) {
        return storage->InsertMetrics(batch.agent_id(),
                                      batch.timestamp_unix_ms(),
                                      batch.metrics());
    };

    auto metrics_fn = [&]() -> std::string {
        std::string out;
        out += "# HELP pudim_kafka_messages_received_total Messages received\n";
        out += "# TYPE pudim_kafka_messages_received_total counter\n";
        out += "pudim_kafka_messages_received_total " +
               std::to_string(stats.messages_received.load()) + "\n";
        out += "# HELP pudim_kafka_batches_processed_total Batches stored to DB\n";
        out += "# TYPE pudim_kafka_batches_processed_total counter\n";
        out += "pudim_kafka_batches_processed_total " +
               std::to_string(stats.batches_processed.load()) + "\n";
        out += "# HELP pudim_kafka_handler_errors_total Insert failures\n";
        out += "# TYPE pudim_kafka_handler_errors_total counter\n";
        out += "pudim_kafka_handler_errors_total " +
               std::to_string(stats.handler_errors.load()) + "\n";
        out += "# HELP pudim_kafka_consumer_errors_total Kafka consumer errors\n";
        out += "# TYPE pudim_kafka_consumer_errors_total counter\n";
        out += "pudim_kafka_consumer_errors_total " +
               std::to_string(stats.consumer_errors.load()) + "\n";
        out += "# HELP pudim_kafka_consumer_lag Total consumer lag (messages)\n";
        out += "# TYPE pudim_kafka_consumer_lag gauge\n";
        out += "pudim_kafka_consumer_lag " +
               std::to_string(ComputeTotalLag(consumer.get())) + "\n";
        if (storage->IsHealthy()) {
            out += "# HELP pudim_storage_healthy Storage reachable (1=ok)\n";
            out += "# TYPE pudim_storage_healthy gauge\n";
            out += "pudim_storage_healthy 1\n";
        }
        return out;
    };

    std::thread http_thread = StartPrometheusEndpoint(http_addr, metrics_fn);

    ConsumeLoop(consumer.get(), handler, &stats);

    consumer->close();
    if (http_thread.joinable()) http_thread.join();
    Log("info", "storage consumer stopped (processed=" +
                    std::to_string(stats.batches_processed.load()) + ")");
    return 0;
}
