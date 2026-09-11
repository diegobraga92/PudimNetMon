#include <iostream>
#include <map>
#include <string>

#include "commands.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

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
    const std::string &tool_home = pudimagent::ToolHomeDir();
    Check(!tool_home.empty(), "tool home resolved");
#ifdef _WIN32
    Check(GetFileAttributesA(tool_home.c_str()) != INVALID_FILE_ATTRIBUTES,
          "tool home exists");
#else
    Check(access(tool_home.c_str(), W_OK) == 0, "tool home is writable");
#endif
    Check(pudimagent::ToolHomeDir() == tool_home, "tool home is stable");

    double loss = -1;
    Check(pudimagent::ParseMtrLoss(
              "  3.|-- 201.1.225.15              70.0%    20    2.3   2.3", &loss) &&
              loss == 70.0,
          "mtr loss parsed from percent row");
    loss = -1;
    Check(pudimagent::ParseMtrLoss(
              "  2.|-- ???                       100.0    20    0.0   0.0", &loss) &&
              loss == 100.0,
          "mtr loss parsed for unanswered hop");
    loss = -1;
    Check(pudimagent::ParseMtrLoss(
              "  8.|-- 1.1.1.1                    0.0%    20    4.0   4.0", &loss) &&
              loss == 0.0,
          "mtr loss parsed for healthy hop");
    Check(!pudimagent::ParseMtrLoss("HOST: dexter   Loss%  Snt   Last", &loss),
          "mtr header rejected");

    const double dl_mbps = pudimagent::SpeedtestBytesPerSecToMbps(117003967);
    Check(dl_mbps > 935.9 && dl_mbps < 936.1,
          "ookla download bandwidth converts to ~936 Mbps");
    const double ul_mbps = pudimagent::SpeedtestBytesPerSecToMbps(60145262);
    Check(ul_mbps > 481.1 && ul_mbps < 481.2,
          "ookla upload bandwidth converts to ~481 Mbps");
    Check(pudimagent::SpeedtestBytesPerSecToMbps(0) == 0.0,
          "zero throughput stays zero");

    // 1. The catalog is non-empty and exposes the expected commands.
    pudimnetmon::ListCommandsResponse list;
    pudimagent::ListCommands(&list);
    Check(list.success(), "list success");
    Check(list.commands_size() >= 5, "catalog has >= 5 commands");
    bool has_hdd = false;
    bool has_info = false;
    bool has_speedtest = false;
    bool has_ping_burst = false;
    bool has_route_quality = false;
    bool speedtest_param = false;
    for (const auto &c : list.commands()) {
        Check(!c.command_id().empty(), "command id non-empty");
        Check(!c.description().empty(),
              "description non-empty for " + c.command_id());
        if (c.command_id() == "hdd_check") has_hdd = true;
        if (c.command_id() == "agent_info") has_info = true;
        if (c.command_id() == "speedtest") {
            has_speedtest = true;
            for (const auto &p : c.param_names()) {
                if (p == "server_id") speedtest_param = true;
            }
        }
        if (c.command_id() == "ping_burst") has_ping_burst = true;
        if (c.command_id() == "route_quality") has_route_quality = true;
    }
    Check(has_hdd, "catalog has hdd_check");
    Check(has_info, "catalog has agent_info");
    Check(has_speedtest, "catalog has speedtest");
    Check(speedtest_param, "speedtest advertises the server_id param");
    Check(has_ping_burst, "catalog has ping_burst");
    Check(has_route_quality, "catalog has route_quality");

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
