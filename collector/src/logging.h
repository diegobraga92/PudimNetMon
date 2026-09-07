#pragma once

#include <chrono>
#include <iostream>
#include <string>

// JSON-structured logging to stdout. Shared by every collector component
// (agent registry, gRPC services, HTTP server). One JSON object per line:
//   {"timestamp":<ms>,"level":"info","component":"collector",
//    "message":"...",["agent_id":"...","trace_id":"..."]}
namespace logger {

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

inline void emit(const std::string &level, const std::string &message,
                 const std::string &agent_id = "",
                 const std::string &trace_id = "") {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    std::cout << "{"
              << "\"timestamp\":" << now << ","
              << "\"level\":\"" << level << "\","
              << "\"component\":\"collector\","
              << "\"message\":\"" << escape(message) << "\"";
    if (!agent_id.empty()) {
        std::cout << ",\"agent_id\":\"" << escape(agent_id) << "\"";
    }
    if (!trace_id.empty()) {
        std::cout << ",\"trace_id\":\"" << escape(trace_id) << "\"";
    }
    std::cout << "}" << std::endl;
}

} // namespace logger
