#include <chrono>
#include <thread>

#include "systemd_notify.h"

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

namespace pudimagent {

void NotifyReady() {
#ifdef HAVE_LIBSYSTEMD
    sd_notify(0, "READY=1");
#endif
}

void NotifyWatchdog() {
#ifdef HAVE_LIBSYSTEMD
    sd_notify(0, "WATCHDOG=1");
#endif
}

void NotifyStatus(const std::string &status) {
#ifdef HAVE_LIBSYSTEMD
    sd_notify(0, ("STATUS=" + status).c_str());
#endif
}

void StartWatchdogThread(bool *stop) {
    std::thread t([stop]() {
        constexpr std::chrono::milliseconds kPingInterval{25000};
        while (!*stop) {
            std::this_thread::sleep_for(kPingInterval);
            if (*stop) break;
            NotifyWatchdog();
        }
    });
    t.detach();
}

} // namespace pudimagent
