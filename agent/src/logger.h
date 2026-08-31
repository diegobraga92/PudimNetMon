#pragma once

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>

// Structured JSON logger shared by all agent components. Emits one JSON object
// per line to stdout:
//   {"timestamp":<unix_ms>,"level":"info","component":"agent",
//    "message":"...","agent_id":"...","trace_id":"..."}
// The output mutex exists because the probe worker / diagnostic server / gRPC
// client threads may log concurrently.
//
// Header-only (C++17 inline state) so any agent TU can include it without
// adding a .cpp to the build. Level and node/trace context are configured once
// at startup by main via SetLevel/SetNodeId/SetTraceId.
namespace logger {

// Severity threshold: messages at or above this level are emitted.
// Keep the enumerators in increasing severity order — enabled() relies on it.
enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

// Guarded because the probe worker / diagnostic server may log concurrently.
inline std::mutex s_out_mu;

inline LogLevel s_level = LogLevel::Info;

// Optional per-agent context embedded in every line (set once at startup).
inline std::string s_node_id;
inline std::string s_trace_id;

inline void SetLevel(LogLevel level) { s_level = level; }
inline void SetNodeId(const std::string &id) { s_node_id = id; }
inline void SetTraceId(const std::string &id) { s_trace_id = id; }

// True if a message of the given severity passes the configured threshold
// (e.g. Warn emits Warn/Error and hides Debug/Info).
inline bool enabled(LogLevel level) {
    return static_cast<int>(s_level) <= static_cast<int>(level);
}

inline const char *Name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "error";  // unreachable: all enumerators handled above
}

inline std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

inline void write(LogLevel level, const std::string &message) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    std::lock_guard<std::mutex> lock(s_out_mu);
    std::cout << "{"
              << "\"timestamp\":" << now << ","
              << "\"level\":\"" << Name(level) << "\","
              << "\"component\":\"agent\","
              << "\"message\":\"" << escape(message) << "\"";
    if (!s_node_id.empty()) {
        std::cout << ",\"agent_id\":\"" << escape(s_node_id) << "\"";
    }
    if (!s_trace_id.empty()) {
        std::cout << ",\"trace_id\":\"" << escape(s_trace_id) << "\"";
    }
    std::cout << "}" << std::endl;
}

} // namespace logger

#define LOG_DEBUG(msg)                                                        \
    do {                                                                     \
        if (logger::enabled(logger::LogLevel::Debug))                        \
            logger::write(logger::LogLevel::Debug, msg);                     \
    } while (0)
#define LOG_INFO(msg)                                                         \
    do {                                                                     \
        if (logger::enabled(logger::LogLevel::Info))                         \
            logger::write(logger::LogLevel::Info, msg);                      \
    } while (0)
#define LOG_WARN(msg)                                                         \
    do {                                                                     \
        if (logger::enabled(logger::LogLevel::Warn))                         \
            logger::write(logger::LogLevel::Warn, msg);                      \
    } while (0)
#define LOG_ERROR(msg)                                                        \
    do {                                                                     \
        if (logger::enabled(logger::LogLevel::Error))                        \
            logger::write(logger::LogLevel::Error, msg);                     \
    } while (0)
