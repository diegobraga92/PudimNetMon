#pragma once

#include <map>
#include <string>

#include "diagnostic.pb.h"

namespace pudimagent {

// Pre-set, whitelisted maintenance commands the dashboard can ask this agent
// to run (e.g. a disk health check). Commands are FIXED, compiled handlers —
// the agent never executes arbitrary shell/terminal input from a requester.
// Adding a command = adding one entry to the catalog table in commands.cpp.
using CommandParams = std::map<std::string, std::string>;

// Fills `resp->commands` with the full catalog (id + description + param names).
void ListCommands(pudimnetmon::ListCommandsResponse *resp);

// Runs the command identified by `id`. Fills `resp` completely (success,
// command_id, timestamp, summary, fields, issues, detail). Returns false when
// `id` is not in the catalog (resp->success=false and resp->error set).
// Never throws.
bool RunCommand(const std::string &id, const CommandParams &params,
                pudimnetmon::CommandResponse *resp);

} // namespace pudimagent
