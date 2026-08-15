#include "probe_runner.h"

#include <algorithm>
#include <cstdlib>
#include <functional>

#include "metrics/ntp_probe.h"
#include "platform/platform.h"

namespace pudimagent {

namespace {

constexpr size_t kMaxQueuedBatches = 50;

// Minimal fixed-size worker pool used to parallelize independent probes within
// a single collection cycle. Tasks are plain closures; the pool persists
// across cycles so we do not pay thread-creation cost every interval.
class ThreadPool {
public:
    explicit ThreadPool(size_t n) {
        n = std::max<size_t>(1, n);
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }
    ~ThreadPool() { Stop(); }

    void Submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();
    }

private:
    void WorkerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

int DefaultPoolSize() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) return 2;
    return static_cast<int>(std::min<unsigned>(4, hw));
}

}  // namespace

struct ProbeRunner::Pool {
    ThreadPool pool{static_cast<size_t>(DefaultPoolSize())};
};

ProbeRunner::ProbeRunner() : pool_(std::make_unique<Pool>()) {}
ProbeRunner::~ProbeRunner() { Stop(); }

void ProbeRunner::Start(const std::string &agent_id) {
    agent_id_ = agent_id;
    s_running_.store(true);
    worker_ = std::thread(&ProbeRunner::RunLoop, this);
}

void ProbeRunner::Stop() {
    if (s_running_.exchange(false)) {
        if (worker_.joinable()) worker_.join();
    } else if (worker_.joinable()) {
        worker_.join();
    }
    pool_->pool.Stop();
}

void ProbeRunner::SetIntervalMs(int interval_ms) {
    s_interval_ms_.store(std::max(100, interval_ms));
}

void ProbeRunner::SetHandshakeIntervalMs(int interval_ms) {
    s_handshake_interval_ms_.store(std::max(0, interval_ms));
}

void ProbeRunner::SetConfigStore(std::shared_ptr<ProbeConfigStore> store) {
    config_store_ = std::move(store);
}

size_t ProbeRunner::PendingCount() {
    std::lock_guard<std::mutex> lock(queue_mu_);
    return queue_.size();
}

size_t ProbeRunner::Drain(std::vector<pudimnetmon::MetricsBatch> &out,
                          size_t limit) {
    std::lock_guard<std::mutex> lock(queue_mu_);
    size_t n = 0;
    while (n < limit && !queue_.empty()) {
        out.push_back(std::move(queue_.front()));
        queue_.pop_front();
        ++n;
    }
    return n;
}

ProbeRunner::CycleStats ProbeRunner::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mu_);
    return stats_;
}


void ProbeRunner::RunLoop() {
    auto next = std::chrono::steady_clock::now();
    auto last_handshake = std::chrono::steady_clock::now();
    uint64_t handshake_cycles = 0;

    while (s_running_.load()) {
        auto cycle_start = std::chrono::steady_clock::now();

        ProbeConfig cfg;
        if (config_store_) {
            cfg = config_store_->Get();
        }

        // Throttle the (relatively expensive) libpcap handshake capture so it
        // does not have to run on every cycle.
        bool run_handshake = cfg.tcp_handshake_capture;
        int hs_ms = s_handshake_interval_ms_.load();
        if (hs_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_handshake < std::chrono::milliseconds(hs_ms)) {
                run_handshake = false;
            } else {
                last_handshake = now;
                ++handshake_cycles;
            }
        }

        std::vector<pudimnetmon::Metric> metrics;
        auto probe_start = std::chrono::steady_clock::now();
        RunCycle(cfg, metrics, run_handshake);
        auto probe_end = std::chrono::steady_clock::now();

        pudimnetmon::MetricsBatch batch;
        batch.set_agent_id(agent_id_);
        batch.set_timestamp_unix_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        for (auto &m : metrics) {
            m.set_seq(seq_.fetch_add(1));
            *batch.add_metrics() = std::move(m);
        }

        {
            std::lock_guard<std::mutex> lock(queue_mu_);
            if (queue_.size() >= kMaxQueuedBatches) {
                // Sender is falling behind (long collector outage); drop the
                // oldest queued batch rather than grow without bound.
                queue_.pop_front();
            }
            queue_.push_back(std::move(batch));
        }

        auto cycle_end = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(stats_mu_);
            stats_.cycle_duration_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    cycle_end - cycle_start)
                    .count();
            stats_.probe_duration_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    probe_end - probe_start)
                    .count();
            stats_.metric_count = static_cast<int>(metrics.size());
            stats_.last_cycle_timestamp_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            stats_.handshake_probe_count =
                static_cast<int64_t>(handshake_cycles);
        }

        // Fixed-rate scheduling: wait for the next cadence slot. When a cycle
        // overruns, skip missed slots instead of running back-to-back.
        int interval_ms = s_interval_ms_.load();
        if (interval_ms < 100) interval_ms = 100;
        next += std::chrono::milliseconds(interval_ms);
        auto now = std::chrono::steady_clock::now();
        if (now < next) {
            while (s_running_.load() &&
                   std::chrono::steady_clock::now() < next) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            next = now;
        }
    }
}

void ProbeRunner::RunCycle(const ProbeConfig &cfg,
                           std::vector<pudimnetmon::Metric> &out,
                           bool run_handshake) {
    // Phase 5: kernel clock offset (cheap syscall; run inline).
    if (cfg.ntp_check) {
        pudimnetmon::Metric ntp;
        ProbeNtpOffset(ntp);
        out.push_back(std::move(ntp));
    }

    // Build the parallel task list. Results go into per-task slots so the
    // final metric order stays deterministic regardless of completion order.
    std::vector<std::vector<pudimnetmon::Metric>> slots;
    std::vector<std::function<void(std::vector<pudimnetmon::Metric> &)>> tasks;

    for (const auto &t : cfg.dns_targets) {
        tasks.push_back([&cfg, t](std::vector<pudimnetmon::Metric> &m) {
            pudimnetmon::Metric a;
            ProbeDns(t, a);
            m.push_back(std::move(a));
            auto it = cfg.dns_expected.find(t);
            std::vector<std::string> expected =
                (it != cfg.dns_expected.end())
                    ? it->second
                    : std::vector<std::string>();
            pudimnetmon::Metric rec;
            ProbeDnsRecord(t, expected, rec);
            m.push_back(std::move(rec));
        });
    }
    for (const auto &t : cfg.tcp_targets) {
        tasks.push_back([&cfg, t](std::vector<pudimnetmon::Metric> &m) {
            pudimnetmon::Metric a;
            ProbeTcp(t, a);
            m.push_back(std::move(a));
            if (cfg.tcp_retransmit_check) {
                pudimnetmon::Metric r;
                ProbeTcpRetransmit(t, r);
                m.push_back(std::move(r));
            }
        });
    }
    for (const auto &t : cfg.tls_targets) {
        tasks.push_back([&cfg, t](std::vector<pudimnetmon::Metric> &m) {
            pudimnetmon::Metric a;
            ProbeTls(t, a);
            m.push_back(std::move(a));
            if (cfg.tls_cert_check) {
                pudimnetmon::Metric c;
                ProbeTlsCert(t, c);
                m.push_back(std::move(c));
            }
        });
    }
    for (const auto &t : cfg.http_targets) {
        if (cfg.http_protocols.empty()) {
            tasks.push_back([t](std::vector<pudimnetmon::Metric> &m) {
                pudimnetmon::Metric h;
                ProbeHttp(t, h);
                m.push_back(std::move(h));
            });
        } else {
            for (const auto &p : cfg.http_protocols) {
                tasks.push_back([t, p](std::vector<pudimnetmon::Metric> &m) {
                    pudimnetmon::Metric h;
                    ProbeHttpProtocol(t, p, h);
                    m.push_back(std::move(h));
                });
            }
        }
    }

    slots.resize(tasks.size());
    if (!tasks.empty()) {
        std::atomic<size_t> remaining{tasks.size()};
        std::mutex wait_mu;
        std::condition_variable wait_cv;
        for (size_t i = 0; i < tasks.size(); ++i) {
            auto *slot = &slots[i];
            auto *task = &tasks[i];
            pool_->pool.Submit([task, slot, &remaining, &wait_cv]() {
                (*task)(*slot);
                if (remaining.fetch_sub(1) == 1) {
                    wait_cv.notify_one();
                }
            });
        }
        std::unique_lock<std::mutex> lock(wait_mu);
        wait_cv.wait(lock, [&remaining] { return remaining.load() == 0; });
        for (auto &slot : slots) {
            for (auto &m : slot) out.push_back(std::move(m));
        }
    }

    // Serial phase: ICMP raw sockets + libpcap handshake capture (shared
    // kernel state).
    for (const auto &t : cfg.ping_targets) {
        pudimnetmon::Metric loss, rtt, jitter;
        ProbeIcmp(t, cfg.ping_count, cfg.ping_gap_ms, loss, rtt, jitter);
        out.push_back(std::move(loss));
        out.push_back(std::move(rtt));
        out.push_back(std::move(jitter));
    }
    if (run_handshake && cfg.tcp_handshake_capture) {
        for (const auto &t : cfg.tcp_targets) {
            pudimnetmon::Metric h;
            ProbeTcpHandshake(t, h);
            out.push_back(std::move(h));
        }
    }
}

}  // namespace pudimagent

