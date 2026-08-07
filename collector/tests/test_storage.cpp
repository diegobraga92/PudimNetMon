#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "metrics.pb.h"
#include "storage/timescale_storage.h"

using pudimnetmon::CheckType;
using pudimnetmon::Metric;
using pudimnetmon::MetricsBatch;
using pudimcollector::StorageConfig;
using pudimcollector::TimescaleStorage;
using pudimcollector::StorageStats;

static bool TestInsertAndQuery() {
    StorageConfig cfg;
    cfg.host = std::getenv("PUDIM_TEST_DB_HOST") ? std::getenv("PUDIM_TEST_DB_HOST") : "localhost";
    cfg.port = 5432;
    cfg.dbname = "pudimnetmon";
    cfg.user = "pudim";
    cfg.password = "pudim";

    TimescaleStorage storage(cfg);
    if (!storage.Connect()) {
        std::cout << "SKIP: TimescaleDB not available, skipping storage tests\n";
        return true;
    }

    // Insert a batch of metrics
    MetricsBatch batch;
    batch.set_agent_id("test-agent-1");
    batch.set_timestamp_unix_ms(1700000000000LL);

    Metric m1;
    m1.set_check_type(CheckType::CHECK_TYPE_DNS_RESOLUTION);
    m1.set_target("example.com");
    m1.set_latency_ms(42.5);
    m1.set_success(true);
    m1.set_seq(1);
    m1.set_monotonic_us(1000);
    *batch.add_metrics() = m1;

    Metric m2;
    m2.set_check_type(CheckType::CHECK_TYPE_TCP_CONNECT);
    m2.set_target("example.com:443");
    m2.set_latency_ms(15.2);
    m2.set_success(true);
    m2.set_seq(2);
    m2.set_monotonic_us(2000);
    *batch.add_metrics() = m2;

    // Storage writes by timestamp, not by explicit time field, so use now
    int64_t now_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    bool ok = storage.InsertMetrics(
        batch.agent_id(), now_ms, batch.metrics());

    assert(ok);
    StorageStats after_insert = storage.GetStats();
    assert(after_insert.metrics_written == 2);
    std::cout << "PASS: Inserted 2 metrics, stats.written="
              << after_insert.metrics_written << "\n";

    // Query back within a 5-minute window
    std::string json = storage.QueryMetricsJson("test-agent-1", "", 300);
    assert(!json.empty());
    assert(json.find("test-agent-1") != std::string::npos);
    std::cout << "PASS: Query returned: " << json.substr(0, 120) << "...\n";

    // Query with check_type filter
    json = storage.QueryMetricsJson("test-agent-1", "tcp_connect", 300);
    assert(json.find("tcp_connect") != std::string::npos);
    assert(json.find("dns_resolution") == std::string::npos);
    std::cout << "PASS: check_type filter works\n";

    return true;
}

int main() {
    // Always run this test; it will skip gracefully if no DB available.
    assert(TestInsertAndQuery());
    std::cout << "ALL COLLECTOR STORAGE TESTS PASSED\n";
    return 0;
}