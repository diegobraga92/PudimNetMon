#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>

#include "diagnostic_service.h"
#include "platform/platform.h"

// POSIX popen is _popen on Windows.
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#include <process.h>
#endif

namespace pudimagent {

namespace {

#ifdef _WIN32
int CurrentPid() { return _getpid(); }
std::string TempDir() { return pudimagent::platform::TempDir(); }
#else
int CurrentPid() { return getpid(); }
std::string TempDir() { return "/tmp"; }
#endif

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

std::string JoinList(const std::vector<std::string> &items) {
    std::ostringstream os;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) os << ",";
        os << items[i];
    }
    return os.str();
}

std::string SummaryOf(const ProbeConfig &c) {
    std::ostringstream summary;
    summary << "dns=" << JoinList(c.dns_targets)
            << " tcp=" << JoinList(c.tcp_targets)
            << " tls=" << JoinList(c.tls_targets)
            << " http=" << JoinList(c.http_targets)
            << " ping=" << JoinList(c.ping_targets)
            << " ping_count=" << c.ping_count
            << " tls_cert=" << (c.tls_cert_check ? "on" : "off")
            << " tcp_retransmit=" << (c.tcp_retransmit_check ? "on" : "off")
            << " tcp_handshake=" << (c.tcp_handshake_capture ? "on" : "off")
            << " http_protocols=" << JoinList(c.http_protocols);
    return summary.str();
}

} // anonymous namespace

grpc::Status DiagnosticServiceImpl::RunDiagnostic(
    grpc::ServerContext *ctx,
    const pudimnetmon::DiagnosticRequest *request,
    pudimnetmon::DiagnosticResponse *response) {
    (void)ctx;
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
#ifdef _WIN32
        std::string cmd = "tracert -d -h 15 -w 1000 " + target;
#else
        std::string cmd = "traceroute -n -m 15 -w 1 " + target;
#endif
        result += "=== traceroute " + target + " ===\n";
        result += RunCommand(cmd);
        result += "\n";
    }

#ifndef _WIN32
    if (request->pcap_duration_s() > 0) {
        std::string cap_file =
            TempDir() + "/pudim_diag_" + std::to_string(CurrentPid()) +
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
#else
    if (request->pcap_duration_s() > 0) {
        result += "=== pcap capture ===\n";
        result += "packet capture is not supported on this platform\n";
        result += "\n";
        ok = false;
    }
#endif

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

grpc::Status DiagnosticServiceImpl::Reconfigure(
    grpc::ServerContext *ctx,
    const pudimnetmon::AgentConfigRequest *request,
    pudimnetmon::AgentConfigResponse *response) {
    (void)ctx;
    if (!request || !response || !m_store) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "request/response/store must not be null");
    }

    // Full replacement semantics: the request is the COMPLETE new config.
    // Empty repeated fields clear the corresponding target list.
    ProbeConfig next;
    next.dns_targets.assign(request->dns_targets().begin(),
                            request->dns_targets().end());
    next.tcp_targets.assign(request->tcp_targets().begin(),
                            request->tcp_targets().end());
    next.tls_targets.assign(request->tls_targets().begin(),
                            request->tls_targets().end());
    next.http_targets.assign(request->http_targets().begin(),
                             request->http_targets().end());
    next.ping_targets.assign(request->ping_targets().begin(),
                             request->ping_targets().end());
    next.ping_count = request->ping_count() > 0 ? request->ping_count()
                                                : ProbeConfig{}.ping_count;
    if (request->has_tls_cert_check()) {
        next.tls_cert_check = request->tls_cert_check();
    }
    if (request->has_tcp_retransmit_check()) {
        next.tcp_retransmit_check = request->tcp_retransmit_check();
    }
    if (request->has_tcp_handshake_capture()) {
        next.tcp_handshake_capture = request->tcp_handshake_capture();
    }
    next.http_protocols.assign(request->http_protocols().begin(),
                               request->http_protocols().end());

    m_store->Set(next);

    response->set_success(true);
    response->set_applied(SummaryOf(next));
    return grpc::Status::OK;
}

grpc::Status DiagnosticServiceImpl::GetConfig(
    grpc::ServerContext *ctx,
    const pudimnetmon::GetConfigRequest *request,
    pudimnetmon::AgentConfigResponse *response) {
    (void)ctx;
    (void)request;
    if (!response || !m_store) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "response/store must not be null");
    }
    response->set_success(true);
    response->set_applied(SummaryOf(m_store->Get()));
    return grpc::Status::OK;
}

} // namespace pudimagent
