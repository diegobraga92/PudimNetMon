#pragma once

#include <functional>
#include <string>

namespace pudimagent::platform {

// Windows service integration. On non-Windows platforms all of these are
// safe no-ops so main() can include this header unconditionally.

// True if the given command line requests service install/uninstall.
bool WantsInstallService(int argc, char **argv);
bool WantsUninstallService(int argc, char **argv);

// Installs the agent as the "PudimNetMonAgent" auto-start service. `args` is
// appended to the executable path in ImagePath (the agent's own CLI flags).
bool InstallAgentService(const std::wstring &args);

// Stops (if running) and removes the service.
bool UninstallAgentService();

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
