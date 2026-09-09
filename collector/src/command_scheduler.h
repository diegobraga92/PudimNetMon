#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "agent_registry.h"
#include "agent_rpc.h"
#include "storage/timescale_storage.h"

namespace pudimcollector {

// Background service that runs pre-set agent commands on a schedule.
class CommandScheduler {
public:
    CommandScheduler(const AgentRegistry &registry,
                     std::shared_ptr<TimescaleStorage> storage,
                     TlsOptions tls);
    ~CommandScheduler();

    CommandScheduler(const CommandScheduler &) = delete;
    CommandScheduler &operator=(const CommandScheduler &) = delete;

    void Start();
    void Stop();

private:
    void Loop();
    void WorkerLoop();
    void ExecuteRun(const CommandSchedule &schedule);

    // Registers/cancels a live RPC ClientContext so Stop() can abort it.
    void RegisterActiveContext(grpc::ClientContext *ctx);
    void UnregisterActiveContext(grpc::ClientContext *ctx);

    const AgentRegistry &m_registry;
    std::shared_ptr<TimescaleStorage> m_storage;
    TlsOptions m_tls;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::deque<CommandSchedule> m_queue;
    // Agent ids with a queued or in-flight run (one heavy command per agent).
    std::unordered_set<std::string> m_busy_agents;

    std::mutex m_ctx_mutex;
    std::vector<grpc::ClientContext *> m_active_ctx;

    std::thread m_thread;
    std::vector<std::thread> m_workers;
    bool m_started = false;
};

} // namespace pudimcollector