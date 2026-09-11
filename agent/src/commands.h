#pragma once

#include <map>
#include <string>

#include "diagnostic.pb.h"

namespace pudimagent {

using CommandParams = std::map<std::string, std::string>;

const std::string &ToolHomeDir();

bool ParseMtrLoss(const std::string &line, double *loss);

double SpeedtestBytesPerSecToMbps(double bytes_per_s);

void ListCommands(pudimnetmon::ListCommandsResponse *resp);

bool RunCommand(const std::string &id, const CommandParams &params,
                pudimnetmon::CommandResponse *resp);

} // namespace pudimagent
