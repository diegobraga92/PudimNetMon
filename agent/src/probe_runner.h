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

// Runs the configured probes on a dedicated worker thread at a fixed cadence
// and hands completed metric batches to the main loop through a bounded FIFO
// queue. Independent probes (DNS/TCP/TLS/HTTP) are parallelized across a small
// worker pool; ICMP raw sockets and the libpcap handshake capture run serially
// on the worker thread because they share global kernel state.
//
// Decoupling probes from the sender means a slow or hung probe (e.g. a
// blackholed target or a slow resolver) no longer delays heartbeats or stalls
// the watchdog path.
class ProbeRunner {
public:
    ProbeRunner();
    ~ProbeRunner();

    ProbeRunner(const ProbeRunner &) = delete;
    ProbeRunner &operator=(const ProbeRunner &) = delete;

    // Starts the probe worker thread. `agent_id` is stamped into produced
    // batches.
    void Start(const std::string &agent_id);

    // Stops the worker and the internal pool, waiting for them to exit.
    void Stop();

    // The probe cadence in milliseconds. May be updated at runtime by the
    // sender loop (backpressure adaptation); the worker picks it up on its
    // next cycle.
    void SetIntervalMs(int interval_ms);
    int GetIntervalMs() const { return s_interval_ms_.load(); }

    // The libpcap TCP-handshake capture cadence in ms. Lower-bounded by the
    // probe interval. 0 disables throttling (run every cycle).
    void SetHandshakeIntervalMs(int interval_ms);
    int GetHandshakeIntervalMs() const { return s_handshake_interval_ms_.load(); }

    // Runtime probe configuration source (the shared ProbeConfigStore updated
    // by the collector's Reconfigure RPC).
    void SetConfigStore(std::shared_ptr<ProbeConfigStore> store);

    // Number of completed batches currently waiting to be sent.
    size_t PendingCount();

    // Pops up to `limit` completed batches (FIFO). Returns the number popped.
    size_t Drain(std::vector<pudimnetmon::MetricsBatch> &out, size_t limit);

    // Per-cycle statistics for the self-observability log.
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
