#pragma once

#include <functional>
#include <string>

namespace pudimagent::platform {

// Windows service runtime integration. Installing/removing the service is the
// Inno Setup installer's job (installer\installer-agent.iss drives sc.exe), so
// the agent binary has no --install-service/--uninstall-service verbs and
// exposes only the runtime helpers below. On non-Windows platforms these are
// safe no-ops so main() can include this header unconditionally.

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
