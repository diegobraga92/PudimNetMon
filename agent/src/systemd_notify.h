#pragma once

#include <string>

namespace pudimagent {

void NotifyReady();
void NotifyWatchdog();
void NotifyStatus(const std::string &status);

void StartWatchdogThread(bool *stop);

} // namespace pudimagent