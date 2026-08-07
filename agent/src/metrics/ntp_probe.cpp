#include <cstring>
#include <sys/timex.h>
#include <time.h>

#include "ntp_probe.h"

namespace pudimagent {

namespace {

int64_t MonotonicUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
}

} // anonymous namespace

void ProbeNtpOffset(pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_NTP_OFFSET);
    metric.set_target("localhost");
    metric.set_monotonic_us(MonotonicUs());

    struct timex tx {};
    std::memset(&tx, 0, sizeof(tx));
    if (ntp_adjtime(&tx) != 0) {
        metric.set_success(false);
        metric.set_detail("ntp_adjtime() failed");
        return;
    }

    // tx.offset is in microseconds (kernel discipline offset); the clock may be
    // un-synchronised (STA_UNSYNC) in which case the offset is stale but still
    // reported for visibility. Convert to signed milliseconds.
    double offset_ms = static_cast<double>(tx.offset) / 1000.0;
    metric.set_latency_ms(offset_ms);

    // Expose the kernel discipline state for the dashboard/debugging.
    auto attrs = metric.mutable_attributes();
    attrs->insert({"ntp_status", std::to_string(tx.status)});
    attrs->insert({"ntp_maxerror_ms", std::to_string(tx.maxerror / 1000.0)});
    attrs->insert({"ntp_esterror_ms", std::to_string(tx.esterror / 1000.0)});
    if (tx.status & STA_UNSYNC) {
        attrs->insert({"ntp_synchronised", "false"});
    } else {
        attrs->insert({"ntp_synchronised", "true"});
    }
    metric.set_success(true);
}

} // namespace pudimagent
