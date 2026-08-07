// pudim-consumer-alert: reads MetricsBatch messages from Kafka and evaluates
// them against alert rules using the same AlertManager class the collector used
// in-process (Phases 1-2). At-least-once applies, but evaluation is in-memory
// and non-failing, so offsets are always committed.
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "alerting/alert_manager.h"
#include "kafka/consumer_common.h"

using pudimcollector::alerting::AlertManager;
using pudimcollector::kafka::BatchHandler;
using pudimcollector::kafka::ComputeTotalLag;
using pudimcollector::kafka::ConsumeLoop;
using pudimcollector::kafka::ConsumerStats;
using pudimcollector::kafka::CreateConsumer;
using pudimcollector::kafka::InstallSignalHandlers;
using pudimcollector::kafka::StartPrometheusEndpoint;

namespace {

std::string NowMs() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
}

void Log(const std::string &level, const std::string &msg) {
    std::cout << "{\"timestamp\":" << NowMs()
              << ",\"level\":\"" << level << "\""
              << ",\"component\":\"consumer-alert\""
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
    std::string group = "alert";
    std::string http_addr = "0.0.0.0:9092";
    std::string rules_path;

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
        } else if ((v = Opt(arg, "--alert-rules-path", i, argc, argv)) != "") {
            rules_path = v;
        } else if (arg == "--help") {
            std::cout << "Usage: pudim-consumer-alert [options]\n"
                      << "  --kafka-brokers      Kafka bootstrap servers (e.g. localhost:9092)\n"
                      << "  --topic              Topic to consume (default: network.metrics)\n"
                      << "  --group              Consumer group (default: alert)\n"
                      << "  --http-addr          Prometheus listen address (default: 0.0.0.0:9092)\n"
                      << "  --alert-rules-path   JSON file with alert rules (required)\n"
                      << "  --help               Show this help\n";
            return 0;
        }
    }

    if (brokers.empty() || rules_path.empty()) {
        std::cerr << "FATAL: --kafka-brokers and --alert-rules-path are required\n";
        return 1;
    }

    InstallSignalHandlers();
    Log("info", "starting alert consumer");

    auto alerts = std::make_shared<AlertManager>();
    std::string err;
    if (!alerts->LoadRulesFromFile(rules_path, err)) {
        std::cerr << "FATAL: " << err << "\n";
        return 1;
    }
    Log("info", "loaded " + std::to_string(alerts->RuleCount()) +
                    " alert rules from " + rules_path);

    auto consumer = CreateConsumer(brokers, topic, group, /*earliest=*/false, err);
    if (!consumer) {
        std::cerr << "FATAL: " << err << "\n";
        return 1;
    }
    Log("info", "subscribed to " + topic + " (group=" + group + ")");

    ConsumerStats stats;
    BatchHandler handler = [&](const pudimnetmon::MetricsBatch &batch) {
        alerts->Evaluate(batch.agent_id(), batch.metrics());
        return true;  // evaluation is in-memory and non-failing
    };

    auto metrics_fn = [&]() -> std::string {
        std::string out;
        out += "# HELP pudim_kafka_messages_received_total Messages received\n";
        out += "# TYPE pudim_kafka_messages_received_total counter\n";
        out += "pudim_kafka_messages_received_total " +
               std::to_string(stats.messages_received.load()) + "\n";
        out += "# HELP pudim_kafka_batches_processed_total Batches evaluated\n";
        out += "# TYPE pudim_kafka_batches_processed_total counter\n";
        out += "pudim_kafka_batches_processed_total " +
               std::to_string(stats.batches_processed.load()) + "\n";
        out += "# HELP pudim_alerts_firing Currently firing alerts\n";
        out += "# TYPE pudim_alerts_firing gauge\n";
        out += "pudim_alerts_firing " +
               std::to_string(alerts->ActiveAlertCount()) + "\n";
        out += "# HELP pudim_alert_notifications_total Notifications sent\n";
        out += "# TYPE pudim_alert_notifications_total counter\n";
        out += "pudim_alert_notifications_total " +
               std::to_string(alerts->TotalAlertsFired()) + "\n";
        out += "# HELP pudim_kafka_consumer_lag Total consumer lag (messages)\n";
        out += "# TYPE pudim_kafka_consumer_lag gauge\n";
        out += "pudim_kafka_consumer_lag " +
               std::to_string(ComputeTotalLag(consumer.get())) + "\n";
        return out;
    };

    std::thread http_thread = StartPrometheusEndpoint(http_addr, metrics_fn);

    ConsumeLoop(consumer.get(), handler, &stats);

    consumer->close();
    if (http_thread.joinable()) http_thread.join();
    Log("info", "alert consumer stopped (evaluated=" +
                    std::to_string(stats.batches_processed.load()) + ")");
    return 0;
}
