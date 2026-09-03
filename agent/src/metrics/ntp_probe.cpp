#include <cstring>
#include <string>

#include "ntp_probe.h"
#include "dns_resolver.h"
#include "platform/platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/timex.h>
#include <time.h>
#endif

namespace pudimagent {

namespace {

std::string g_ntp_server = "pool.ntp.org";

#ifdef _WIN32

uint64_t NtpNow() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    uint64_t ft100ns = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                       ft.dwLowDateTime;
    // FILETIME (1601) -> Unix (1970), then Unix -> NTP (1900).
    uint64_t ntp_secs =
        (ft100ns / 10000000ULL) - 11644473600ULL + 2208988800ULL;
    uint32_t fraction = static_cast<uint32_t>(
        ((ft100ns % 10000000ULL) << 32) / 10000000ULL);
    return (ntp_secs << 32) | fraction;
}

double NtpSecondsFromPacket(const unsigned char *p) {
    uint32_t hi = 0, lo = 0;
    for (int i = 0; i < 4; ++i) hi = (hi << 8) | p[i];
    for (int i = 0; i < 4; ++i) lo = (lo << 8) | p[i + 4];
    return static_cast<double>(hi) + static_cast<double>(lo) / 4294967296.0;
}

// Measures the clock offset against g_ntp_server using a simple RFC 4330 SNTP
// exchange. offset_ms is positive when the local clock is ahead of the server.
bool RunSntpOffset(std::string &detail, double &offset_ms) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        detail = "socket failed";
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    LookupResult lr = GlobalResolver().Lookup(g_ntp_server, "123", hints, 3000);
    if (!lr.ok || !lr.addrs) {
        closesocket(s);
        detail = lr.ok ? "no IPv4 address for " + g_ntp_server
                       : "resolution failed for " + g_ntp_server;
        return false;
    }
    sockaddr_in addr{};
    bool found = false;
    for (struct addrinfo *ai = lr.addrs.get(); ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            addr = *reinterpret_cast<sockaddr_in *>(ai->ai_addr);
            found = true;
            break;
        }
    }
    if (!found) {
        closesocket(s);
        detail = "no IPv4 address for " + g_ntp_server;
        return false;
    }

    // Build the SNTP request: LI=0, VN=3, Mode=3 (client).
    unsigned char packet[48];
    std::memset(packet, 0, sizeof(packet));
    packet[0] = 0x1B;
    uint64_t t1 = NtpNow();
    for (int i = 0; i < 8; ++i) {
        packet[40 + i] = static_cast<unsigned char>((t1 >> (56 - 8 * i)) & 0xFF);
    }

    if (sendto(s, reinterpret_cast<const char *>(packet), sizeof(packet), 0,
               reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        closesocket(s);
        detail = "sendto failed";
        return false;
    }

    int timeout_ms = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));

    unsigned char reply[48];
    int n = recv(s, reinterpret_cast<char *>(reply), sizeof(reply), 0);
    closesocket(s);
    if (n < 48) {
        detail = (n == SOCKET_ERROR) ? "timeout waiting for NTP reply"
                                     : "short NTP reply";
        return false;
    }

    double t2 = NtpSecondsFromPacket(reply + 32);  // server receive time
    double t3 = NtpSecondsFromPacket(reply + 40);  // server transmit time
    double t4 = static_cast<double>(NtpNow()) / 4294967296.0;

    double t1_sec = static_cast<double>(t1) / 4294967296.0;
    double offset_sec = ((t2 - t1_sec) + (t3 - t4)) / 2.0;
    offset_ms = offset_sec * 1000.0;
    return true;
}

#endif  // _WIN32

} // anonymous namespace

void SetNtpServer(const std::string &server) {
    if (!server.empty()) g_ntp_server = server;
}

void ProbeNtpOffset(pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_NTP_OFFSET);
    metric.set_target("localhost");
    metric.set_monotonic_us(platform::MonotonicUs());

#ifdef _WIN32
    double offset_ms = 0.0;
    std::string detail;
    if (!RunSntpOffset(detail, offset_ms)) {
        metric.set_success(false);
        metric.set_detail(detail);
        return;
    }
    metric.set_latency_ms(offset_ms);
    auto attrs = metric.mutable_attributes();
    (*attrs)["ntp_server"] = g_ntp_server;
    (*attrs)["ntp_synchronised"] = "true";
    metric.set_success(true);
#else
    struct timex tx {};
    std::memset(&tx, 0, sizeof(tx));
    if (ntp_adjtime(&tx) != 0) {
        metric.set_success(false);
        metric.set_detail("ntp_adjtime() failed");
        return;
    }

    double offset_ms = static_cast<double>(tx.offset) / 1000.0;
    metric.set_latency_ms(offset_ms);

    auto attrs = metric.mutable_attributes();
    attrs->insert({"ntp_status", std::to_string(tx.status)});
    attrs->insert({"ntp_maxerror_ms", std::to_string(tx.maxerror / 1000.0)});
    attrs->insert({"ntp_esterror_ms", std::to_string(tx.esterror / 1000.0)});
    if (tx.status & STA_UNSYNC) {
        attrs->insert({"ntp_synchronised", "false"});
    } else {
        attrs->insert({"ntp_synchronised", "true"});
    }
    metric.set_success(true);
#endif
}

} // namespace pudimagent
