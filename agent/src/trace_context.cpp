#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

#include "trace_context.h"
#include "platform/platform.h"

namespace pudimagent {

namespace {

// Reads `n` random bytes into `out`. Returns false on failure.
bool RandomBytes(unsigned char *out, size_t n) {
#ifdef _WIN32
    return pudimagent::platform::RandomBytes(out, n);
#else
    FILE *f = std::fopen("/dev/urandom", "rb");
    if (!f) {
        // Fallback: std::random_device (used only if /dev/urandom is blocked).
        std::random_device rd;
        for (size_t i = 0; i < n; i++) {
            out[i] = static_cast<unsigned char>(rd());
        }
        return true;
    }
    size_t got = std::fread(out, 1, n, f);
    std::fclose(f);
    return got == n;
#endif
}

std::string BytesToHex(const unsigned char *bytes, size_t n) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += hex[(bytes[i] >> 4) & 0x0f];
        s += hex[bytes[i] & 0x0f];
    }
    return s;
}

} // anonymous namespace

std::string GenerateTraceParent() {
    unsigned char trace_id[16];
    unsigned char span_id[8];
    if (!RandomBytes(trace_id, sizeof(trace_id)) ||
        !RandomBytes(span_id, sizeof(span_id))) {
        return "00-00000000000000000000000000000000-0000000000000000-01";
    }
    return "00-" + BytesToHex(trace_id, 16) + "-" + BytesToHex(span_id, 8) +
           "-01";
}

std::string NextSpanId(const std::string &traceparent) {
    // Keep the trace-id (first field), regenerate the span-id (second field).
    auto first_dash = traceparent.find('-');
    auto second_dash = first_dash == std::string::npos
                           ? std::string::npos
                           : traceparent.find('-', first_dash + 1);
    auto third_dash = second_dash == std::string::npos
                          ? std::string::npos
                          : traceparent.find('-', second_dash + 1);
    if (first_dash == std::string::npos || second_dash == std::string::npos ||
        third_dash == std::string::npos) {
        return GenerateTraceParent();
    }

    unsigned char span_id[8];
    if (!RandomBytes(span_id, sizeof(span_id))) {
        return traceparent;
    }
    std::string new_span = BytesToHex(span_id, 8);
    std::string flags = traceparent.substr(third_dash + 1);
    if (flags.empty()) flags = "01";
    return traceparent.substr(0, third_dash + 1) + new_span + "-" + flags;
}

std::string TraceIdOf(const std::string &traceparent) {
    auto first_dash = traceparent.find('-');
    if (first_dash == std::string::npos) return "";
    return traceparent.substr(0, first_dash);
}

} // namespace pudimagent
