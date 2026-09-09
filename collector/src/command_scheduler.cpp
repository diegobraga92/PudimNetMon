#include "command_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "logging.h"

namespace pudimcollector {

namespace {

constexpr int kWorkerCount = 4;
constexpr int64_t kTickIntervalMs = 1000;
// Longest a scheduled command may run on an agent.
constexpr int64_t kCommandDeadlineMs = 10LL * 60 * 1000;
// Prune command_runs history older than this.
constexpr int64_t kHistoryRetentionMs = 30LL * 24 * 3600 * 1000;
constexpr int64_t kPruneEveryMs = 6LL * 3600 * 1000;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Next execution slot strictly after `after_ms`, aligned to interval
// boundaries counted from the window start. Guarantees a stable cadence even
// after collector downtime (no catch-up bursts).
int64_t NextSlot(int64_t window_start_ms, int64_t interval_ms,
                 int64_t after_ms) {
    if (interval_ms <= 0) return after_ms + 3600 * 1000;
    if (after_ms <= window_start_ms) return window_start_ms + interval_ms;
    int64_t elapsed = after_ms - window_start_ms;
    int64_t k = elapsed / interval_ms;
    return window_start_ms + (k + 1) * interval_ms;
}

} // namespace

CommandScheduler::CommandScheduler(const AgentRegistry &registry,
                                   std::shared_ptr<TimescaleStorage> storage,
                                   TlsOptions tls)
    : m_registry(registry), m_storage(std::move(storage)), m_tls(std::move(tls)) {}

CommandScheduler::~CommandScheduler() { Stop(); }

void CommandScheduler::Start() {
    {
        std::lock_guard lock(m_mutex);
        if (m_started) return;
        m_started = true;
        m_stop = false;
    }
    m_thread = std::thread([this] { Loop(); });
    for (int i = 0; i < kWorkerCount; ++i) {
        m_workers.emplace_back([this] { WorkerLoop(); });
    }
    logger::emit("info",
                 "command scheduler started (" + std::to_string(kWorkerCount) +
                     " workers)");
}

void CommandScheduler::Stop() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_started) return;
        m_stop = true;
        m_started = false;
    }
    m_cv.notify_all();
    {
        std::lock_guard lock(m_ctx_mutex);
        for (auto *ctx : m_active_ctx) {
            if (ctx) ctx->TryCancel();
        }
    }
    if (m_thread.joinable()) m_thread.join();
    for (auto &w : m_workers) {
        if (w.joinable()) w.join();
    }
    m_workers.clear();
    logger::emit("info", "command scheduler stopped");
}

void CommandScheduler::RegisterActiveContext(grpc::ClientContext *ctx) {
    std::lock_guard lock(m_ctx_mutex);
    m_active_ctx.push_back(ctx);
}

void CommandScheduler::UnregisterActiveContext(grpc::ClientContext *ctx) {
    std::lock_guard lock(m_ctx_mutex);
    auto it = std::find(m_active_ctx.begin(), m_active_ctx.end(), ctx);
    if (it != m_active_ctx.end()) m_active_ctx.erase(it);
}

void CommandScheduler::Loop() {
    auto last_prune = std::chrono::steady_clock::now();
    while (true) {
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(kTickIntervalMs),
                          [this] { return m_stop; });
            if (m_stop) return;
        }
        if (!m_storage) continue;

        const int64_t now = NowMs();

        // Snap missed slots (collector downtime) forward without catch-up runs.
        m_storage->RescheduleStaleSchedules(now);

        auto due = m_storage->ListDueCommandSchedules(now);
        for (const auto &sched : due) {
            const int64_t interval_ms = sched.interval_sec * 1000;
            const int64_t new_next =
                NextSlot(sched.window_start_unix_ms, interval_ms, now);
            // Another scheduler (or an old tick) must not double dispatch this slot.
            if (!m_storage->AdvanceCommandScheduleNextRun(
                    sched.id, sched.next_run_unix_ms, new_next)) {
                continue;
            }
            {
                std::lock_guard lock(m_mutex);
                if (m_stop) return;
                // One heavy command per agent at a time; skip overlapping slots.
                if (m_busy_agents.count(sched.agent_id) > 0) continue;
                m_busy_agents.insert(sched.agent_id);
                m_queue.push_back(sched);
            }
            m_cv.notify_one();
        }

        // Opportunistic history retention.
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - last_prune)
                           .count();
        if (elapsed >= kPruneEveryMs) {
            last_prune = std::chrono::steady_clock::now();
            int64_t removed = 0;
            if (m_storage->PruneCommandRuns(NowMs() - kHistoryRetentionMs,
                                            &removed) &&
                removed > 0) {
                logger::emit("info", "pruned " + std::to_string(removed) +
                                         " old command run history row(s)");
            }
        }
    }
}

void CommandScheduler::WorkerLoop() {
    for (;;) {
        CommandSchedule sched;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) {
                if (m_stop) return;
                continue;
            }
            sched = m_queue.front();
            m_queue.pop_front();
            // Do not start heavy commands that were still queued during
            // shutdown — let the process exit promptly.
            if (m_stop) return;
        }
        ExecuteRun(sched);
        {
            std::lock_guard lock(m_mutex);
            m_busy_agents.erase(sched.agent_id);
        }
        m_cv.notify_one();
    }
}

void CommandScheduler::ExecuteRun(const CommandSchedule &sched) {
    CommandRun run;
    run.schedule_id = sched.id;
    run.agent_id = sched.agent_id;
    run.command_id = sched.command_id;
    run.scheduled_unix_ms = sched.next_run_unix_ms;
    run.started_unix_ms = NowMs();

    auto record_result = [&]() {
        run.finished_unix_ms = NowMs();
        if (run.summary.empty()) {
            run.summary = run.success ? "ok" : "failed";
        }
        m_storage->FinishCommandRun(run, nullptr);
    };

    std::string err;
    const int64_t run_id = m_storage->InsertCommandRun(run, &err);
    if (run_id < 0) {
        logger::emit("warn",
                     "could not record scheduled command run (agent=" +
                         sched.agent_id + " command=" + sched.command_id +
                         "): " + err);
        return;
    }
    run.run_id = run_id;

    // The window may have ended while the run was queued behind another one.
    if (NowMs() > sched.window_end_unix_ms) {
        run.success = false;
        run.error = "command window ended before the run could start";
        record_result();
        return;
    }

    const std::string endpoint =
        m_registry.GetDiagnosticEndpoint(sched.agent_id);
    if (endpoint.empty() || !m_registry.IsAgentAlive(sched.agent_id)) {
        run.success = false;
        run.error = "agent offline or unknown; run skipped";
        record_result();
        return;
    }

    auto stub = DialAgentDiagnostic(endpoint, m_tls);
    if (!stub) {
        run.success = false;
        run.error = "could not dial agent diagnostic endpoint " + endpoint;
        record_result();
        return;
    }

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(kCommandDeadlineMs));
    RegisterActiveContext(&ctx);

    pudimnetmon::RunCommandRequest req;
    req.set_agent_id(sched.agent_id);
    req.set_command_id(sched.command_id);
    try {
        auto params = nlohmann::json::parse(sched.params_json);
        if (params.is_object()) {
            for (auto &it : params.items()) {
                if (it.value().is_string()) {
                    (*req.mutable_params())[it.key()] =
                        it.value().get<std::string>();
                } else {
                    (*req.mutable_params())[it.key()] = it.value().dump();
                }
            }
        }
    } catch (...) {
        // Invalid stored params JSON
    }

    pudimnetmon::CommandResponse cresp;
    grpc::Status status = stub->RunCommand(&ctx, req, &cresp);
    UnregisterActiveContext(&ctx);

    if (!status.ok()) {
        run.success = false;
        run.error = "agent RPC failed: " + status.error_message();
        record_result();
        return;
    }

    run.success = cresp.success();
    run.error = cresp.error();
    run.summary = cresp.summary();
    nlohmann::json fields = nlohmann::json::object();
    for (const auto &kv : cresp.fields()) {
        fields[kv.first] = kv.second;
    }
    nlohmann::json issues = nlohmann::json::array();
    for (int i = 0; i < cresp.issues_size(); ++i) {
        issues.push_back(cresp.issues(i));
    }
    run.fields_json = fields.dump();
    run.issues_json = issues.dump();
    run.detail = cresp.detail();
    record_result();

    logger::emit("info",
                 "scheduled command finished: agent=" + sched.agent_id +
                     " command=" + sched.command_id +
                     " success=" + std::string(run.success ? "true" : "false"));
}

} // namespace pudimcollector



