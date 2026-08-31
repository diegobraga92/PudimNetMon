#include <iostream>
#include <map>
#include <string>

#include "commands.h"

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &name) {
    if (cond) {
        std::cout << "PASS " << name << "\n";
    } else {
        std::cerr << "FAIL " << name << "\n";
        ++g_failures;
    }
}

} // namespace

int main() {
    // 1. The catalog is non-empty and exposes the expected commands.
    pudimnetmon::ListCommandsResponse list;
    pudimagent::ListCommands(&list);
    Check(list.success(), "list success");
    Check(list.commands_size() >= 2, "catalog has >= 2 commands");
    bool has_hdd = false;
    bool has_info = false;
    for (const auto &c : list.commands()) {
        Check(!c.command_id().empty(), "command id non-empty");
        Check(!c.description().empty(),
              "description non-empty for " + c.command_id());
        if (c.command_id() == "hdd_check") has_hdd = true;
        if (c.command_id() == "agent_info") has_info = true;
    }
    Check(has_hdd, "catalog has hdd_check");
    Check(has_info, "catalog has agent_info");

    // 2. Unknown commands are rejected, never executed.
    pudimnetmon::CommandResponse resp;
    bool known = pudimagent::RunCommand("no_such_command", {}, &resp);
    Check(!known, "unknown command rejected");
    Check(!resp.success(), "unknown command success=false");
    Check(resp.error().find("unknown") != std::string::npos,
          "unknown command error set");
    Check(resp.timestamp_unix_ms() != 0, "unknown command timestamp set");

    // 3. agent_info runs and reports structured fields.
    resp.Clear();
    known = pudimagent::RunCommand("agent_info", {}, &resp);
    Check(known, "agent_info known");
    Check(resp.success(), "agent_info success");
    Check(resp.fields().count("os") == 1, "agent_info has os field");
    Check(resp.fields().count("hostname") == 1, "agent_info has hostname field");
    Check(resp.fields().count("arch") == 1, "agent_info has arch field");
    Check(!resp.summary().empty(), "agent_info summary");
    Check(resp.timestamp_unix_ms() != 0, "agent_info timestamp set");

    // 4. hdd_check runs and reports disk usage (graceful without smartctl).
    resp.Clear();
    known = pudimagent::RunCommand("hdd_check", {}, &resp);
    Check(known, "hdd_check known");
    Check(resp.success(), "hdd_check success");
    Check(resp.fields().count("disk_usage_percent") == 1,
          "hdd_check has disk_usage_percent");
    Check(resp.fields().count("disk_total_bytes") == 1,
          "hdd_check has disk_total_bytes");
    Check(!resp.summary().empty(), "hdd_check summary");
    Check(resp.timestamp_unix_ms() != 0, "hdd_check timestamp set");

    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "All command tests passed\n";
    return 0;
}
