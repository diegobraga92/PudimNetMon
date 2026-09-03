#pragma once

#include <functional>
#include <string>

namespace pudimagent::platform {

bool InitNetwork(std::string &error);
void CleanupNetwork();

bool TryRunAsService(int argc, char **argv,
                     std::function<int(int, char **)> run_main,
                     std::function<void()> on_stop);

} // namespace pudimagent::platform
