// Kafka round-trip integration test: produces a MetricsBatch via KafkaProducer
// and consumes it back with a KafkaConsumer (consumer_common), verifying the
// serialized batch arrives intact. Skips gracefully when no broker is reachable
// (defaults to localhost:9092, override with PUDIM_TEST_KAFKA_BROKERS).
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

namespace {

// Fast pre-check: can we open a TCP connection to the bootstrap broker? This
// makes the "no Kafka in CI" case skip in ~0s instead of waiting for
// message.timeout.ms. The delivery-report guard below remains the backstop.
bool BrokerReachable(const std::string &brokers, int timeout_ms = 2000) {
    // Use the first host:port of the bootstrap list.
    std::string hostport = brokers.substr(0, brokers.find(','));
    auto colon = hostport.rfind(':');
    if (colon == std::string::npos) return false;
    std::string host = hostport.substr(0, colon);
    int port = std::stoi(hostport.substr(colon + 1));

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return false;
    int family = res->ai_family;
    std::size_t addrlen = res->ai_addrlen;

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return false;
    }

    // Non-blocking connect with a deadline so filtered ports don't hang.
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_storage addr {};
    std::memcpy(&addr, res->ai_addr, addrlen);
    freeaddrinfo(res);
    if (family == AF_INET) {
        reinterpret_cast<struct sockaddr_in *>(&addr)->sin_port = htons(port);
    } else {
        reinterpret_cast<struct sockaddr_in6 *>(&addr)->sin6_port = htons(port);
    }

    bool ok = connect(fd, reinterpret_cast<struct sockaddr *>(&addr),
                      sizeof(addr)) == 0;
    close(fd);
    return ok;
}

} // anonymous namespace

int main() {
    const char *env = std::getenv("PUDIM_TEST_KAFKA_BROKERS");
    std::string brokers = env ? env : "localhost:9092";
    const std::string topic = "network.metrics";
    const std::string group = "pudim-e2e-" + std::to_string(getpid());

    if (!BrokerReachable(brokers)) {
        std::cout << "SKIP: Kafka broker not reachable at " << brokers << "\n";
        return 0;
    }

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
    // Block long enough for message.timeout.ms (15s) to fire delivery reports
    // for an unreachable broker, so the SKIP guard below is reliable.
    producer.Flush(20000);

    // RdKafka::Producer::create() succeeds even with no reachable broker, so
    // detect an unreachable cluster via the delivery report: if the batch was
    // enqueued but never delivered successfully, skip gracefully (e.g. CI has
    // no Kafka broker).
    if (producer.ProducedTotal() > 0 && producer.DeliverySuccesses() == 0) {
        std::cout << "SKIP: Kafka broker unreachable at " << brokers
                  << " (delivery failures=" << producer.DeliveryFailures() << ")\n";
        return 0;
    }

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
