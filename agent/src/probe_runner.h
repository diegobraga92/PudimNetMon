#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "metrics.pb.h"
#include "metrics/probes.h"

namespace pudimagent {

class ProbeRunner {
public:
    ProbeRunner();
    ~ProbeRunner();

    ProbeRunner(const ProbeRunner &) = delete;
    ProbeRunner &operator=(const ProbeRunner &) = delete;

    void Start(const std::string &agent_id);
    void Stop();

    void SetIntervalMs(int interval_ms);
    int GetIntervalMs() const { return s_interval_ms_.load(); }

    void SetHandshakeIntervalMs(int interval_ms);
    int GetHandshakeIntervalMs() const { return s_handshake_interval_ms_.load(); }

    void SetConfigStore(std::shared_ptr<ProbeConfigStore> store);

    size_t PendingCount();
    size_t Drain(std::vector<pudimnetmon::MetricsBatch> &out, size_t limit);

    struct CycleStats {
        int64_t cycle_duration_ms = 0;
        int64_t probe_duration_ms = 0;
        int metric_count = 0;
        int64_t last_cycle_timestamp_ms = 0;
        int64_t handshake_probe_count = 0;
    };
    CycleStats GetStats() const;

private:
    void RunLoop();
    void RunCycle(const ProbeConfig &cfg, std::vector<pudimnetmon::Metric> &out,
                  bool run_handshake);

    std::thread worker_;
    std::atomic<bool> s_running_{false};
    std::atomic<int> s_interval_ms_{5000};
    std::atomic<int> s_handshake_interval_ms_{0};

    std::mutex queue_mu_;
    std::deque<pudimnetmon::MetricsBatch> queue_;
    std::atomic<uint64_t> seq_{0};
    std::string agent_id_;

    std::shared_ptr<ProbeConfigStore> config_store_;

    mutable std::mutex stats_mu_;
    CycleStats stats_;

    // Internal parallel-execution pool.
    struct Pool;
    std::unique_ptr<Pool> pool_;
};

}  // namespace pudimagent
