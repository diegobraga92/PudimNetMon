#pragma once

#include <map>
#include <string>

#include "diagnostic.pb.h"

namespace pudimagent {

using CommandParams = std::map<std::string, std::string>;

void ListCommands(pudimnetmon::ListCommandsResponse *resp);

bool RunCommand(const std::string &id, const CommandParams &params,
                pudimnetmon::CommandResponse *resp);

} // namespace pudimagent
