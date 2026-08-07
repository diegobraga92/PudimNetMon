#include <chrono>
#include <cstdio>
#include <string>

#include "diagnostic_service.h"

namespace pudimagent {

namespace {

std::string ReadAll(FILE *fp) {
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    return out;
}

// Runs a shell command and returns its combined stdout+stderr.
std::string RunCommand(const std::string &cmd) {
    FILE *fp = popen((cmd + " 2>&1").c_str(), "r");
    if (!fp) return "popen failed\n";
    std::string out = ReadAll(fp);
    pclose(fp);
    return out;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // anonymous namespace

grpc::Status DiagnosticServiceImpl::RunDiagnostic(
    grpc::ServerContext *ctx,
    const pudimnetmon::DiagnosticRequest *request,
    pudimnetmon::DiagnosticResponse *response) {
    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "request and response must not be null");
    }

    std::string result;
    bool ok = true;

    if (!request->trace_target().empty()) {
        // traceroute needs root for ICMP/UDP probes; -n avoids DNS lookups and
        // -w 1 bounds per-hop timeout.
        std::string target = request->trace_target();
        std::string cmd = "traceroute -n -m 15 -w 1 " + target;
        result += "=== traceroute " + target + " ===\n";
        result += RunCommand(cmd);
        result += "\n";
    }

    if (request->pcap_duration_s() > 0) {
        std::string cap_file =
            "/tmp/pudim_diag_" + std::to_string(getpid()) +
            "_" + std::to_string(NowMs()) + ".pcap";
        std::string filter = request->pcap_filter().empty()
                                 ? ""
                                 : request->pcap_filter() + " ";
        std::string cmd = "timeout " + std::to_string(request->pcap_duration_s()) +
                          " tcpdump -i any -nn -c 500 " + filter + "-w " + cap_file;
        result += "=== pcap capture (" + std::to_string(request->pcap_duration_s()) +
                  "s, filter='" + request->pcap_filter() + "') ===\n";
        result += RunCommand(cmd);
        result += "\n";

        // Summarize the captured packets.
        result += "=== pcap summary (first 15 packets) ===\n";
        result += RunCommand("tcpdump -r " + cap_file + " -nn -c 15 2>&1");
        result += "\n";
        result += "total packets: " +
                  RunCommand("tcpdump -r " + cap_file + " -nn 2>/dev/null | wc -l");
    }

    if (request->trace_target().empty() && request->pcap_duration_s() <= 0) {
        ok = false;
        response->set_error("diagnostic request has no work (set trace_target "
                            "and/or pcap_duration_s)");
    }

    response->set_success(ok);
    response->set_result(result);
    response->set_timestamp_unix_ms(NowMs());
    return grpc::Status::OK;
}

} // namespace pudimagent
