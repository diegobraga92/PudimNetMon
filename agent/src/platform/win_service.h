#pragma once

#include <functional>
#include <string>

namespace pudimagent::platform {

// Windows service integration. On non-Windows platforms all of these are
// safe no-ops so main() can include this header unconditionally.

// True if the given command line requests service install/uninstall.
bool WantsInstallService(int argc, char **argv);
bool WantsUninstallService(int argc, char **argv);

// Installs (or, if the service already exists, reconfigures) the agent as the
// "PudimNetMonAgent" auto-start service running as LocalSystem. The service
// ImagePath is `"<this exe>"` followed by every argv token except the
// `--install-service` verb itself; each token is quoted with Windows
// command-line escaping so values containing spaces survive the SCM's re-parse.
//
// Only immutable/identity flags belong here (e.g. --node-id); mutable settings
// (collector endpoints, interval, ...) belong in the agent.conf the installer
// writes, so re-installs/upgrades never leave stale arguments behind. On
// failure `error` holds a human-readable reason (e.g. "access denied").
bool InstallAgentService(int argc, char **argv, std::string &error);

// Stops (if running) and removes the service. Succeeds when the service is
// already gone (idempotent uninstall). On failure `error` holds the reason.
bool UninstallAgentService(std::string &error);

// One-time Winsock initialization / teardown (Windows only).
bool InitNetwork(std::string &error);
void CleanupNetwork();

// When the process was started by the Service Control Manager, dispatches the
// service lifecycle and runs `run_main` inside it. Returns true if the process
// ran as a service (the caller should return without running run_main again).
// Returns false when running as a console application. `on_stop` is invoked on
// STOP/SHUTDOWN so the agent can shut down gracefully.
bool TryRunAsService(int argc, char **argv,
                     std::function<int(int, char **)> run_main,
                     std::function<void()> on_stop);

} // namespace pudimagent::platform
