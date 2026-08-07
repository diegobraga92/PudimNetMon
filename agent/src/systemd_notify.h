#pragma once

#include <string>

namespace pudimagent {

// Notifies systemd that the agent finished starting up (Type=notify units).
// No-op when built without libsystemd or when not running under systemd.
void NotifyReady();

// Sends a watchdog heartbeat ("WATCHDOG=1"). The systemd unit should set
// WatchdogSec; the caller pings periodically (e.g. every WatchdogSec/2).
void NotifyWatchdog();

// Sends a status string to systemd (informational).
void NotifyStatus(const std::string &status);

// Runs a watchdog ping thread in the background. Kills itself when `stop` is
// set. Returns immediately (thread is detached).
void StartWatchdogThread(bool *stop);

} // namespace pudimagent