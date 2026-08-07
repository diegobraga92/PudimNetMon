#include <iostream>
#include <string>

#include <librdkafka/rdkafkacpp.h>

#include "producer.h"

namespace pudimcollector::kafka {

struct KafkaProducer::Impl {
    // Delivery report callback; increments the owning producer's counters.
    class DeliveryCb : public RdKafka::DeliveryReportCb {
    public:
        explicit DeliveryCb(KafkaProducer *owner) : m_owner(owner) {}

        void dr_cb(RdKafka::Message &message) override {
            if (message.err() != RdKafka::ERR_NO_ERROR) {
                std::cerr << "Kafka delivery failed for key '"
                          << (message.key() ? *message.key() : "")
                          << "': " << message.errstr() << "\n";
                m_owner->m_delivery_failures++;
            } else {
                m_owner->m_delivery_successes++;
            }
        }

    private:
        KafkaProducer *m_owner;
    };

    explicit Impl(KafkaProducer *owner) : dr_cb(owner) {}

    std::unique_ptr<RdKafka::Producer> producer;
    DeliveryCb dr_cb;
};

KafkaProducer::KafkaProducer()
    : m_impl(std::make_unique<Impl>(this)) {}

KafkaProducer::~KafkaProducer() {
    if (m_impl->producer) {
        m_impl->producer->flush(5000);
    }
}

bool KafkaProducer::Connect(const std::string &brokers, const std::string &topic,
                            std::string &error) {
    m_topic = topic;

    auto conf = std::unique_ptr<RdKafka::Conf>(
        RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (!conf) {
        error = "failed to create RdKafka global conf";
        return false;
    }

    auto set_conf = [&](const std::string &key, const std::string &value) -> bool {
        std::string err;
        if (conf->set(key, value, err) != RdKafka::Conf::CONF_OK) {
            error = "kafka conf '" + key + "': " + err;
            return false;
        }
        return true;
    };

    if (!set_conf("bootstrap.servers", brokers)) return false;
    if (!set_conf("message.timeout.ms", "15000")) return false;
    // Reconnect on broker loss (helps the collector survive broker restarts).
    if (!set_conf("enable.idempotence", "true")) return false;

    {
        std::string err;
        if (conf->set("dr_cb", &m_impl->dr_cb, err) != RdKafka::Conf::CONF_OK) {
            error = "kafka conf 'dr_cb': " + err;
            return false;
        }
    }

    m_impl->producer.reset(RdKafka::Producer::create(conf.get(), error));
    if (!m_impl->producer) {
        error = "failed to create Kafka producer: " + error;
        return false;
    }

    std::cout << "Kafka producer connected to " << brokers
              << " (topic=" << m_topic << ")\n";
    return true;
}

bool KafkaProducer::Produce(const pudimnetmon::MetricsBatch &batch) {
    if (!m_impl->producer) return false;

    std::string payload;
    if (!batch.SerializeToString(&payload)) {
        std::cerr << "Failed to serialize MetricsBatch for Kafka\n";
        return false;
    }

    // Key by agent_id → consistent partition → per-agent ordering.
    const std::string &key = batch.agent_id();

    RdKafka::ErrorCode rc = m_impl->producer->produce(
        m_topic, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
        payload.data(), payload.size(), key.data(), key.size(), -1, nullptr);

    if (rc != RdKafka::ERR_NO_ERROR) {
        std::cerr << "Kafka produce failed for agent '" << key << "': "
                  << RdKafka::err2str(rc) << "\n";
        m_delivery_failures++;
        return false;
    }

    m_produced_total++;
    // Poll delivery reports promptly so dr_cb updates the counters.
    m_impl->producer->poll(0);
    return true;
}

void KafkaProducer::Flush(int timeout_ms) {
    if (m_impl->producer) {
        m_impl->producer->flush(timeout_ms);
    }
}

} // namespace pudimcollector::kafka
