#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>  // SIO_TCP_INFO / TCP_INFO_v0 (Windows 10+)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "platform/platform.h"
#include "probes.h"
#include "ntp_probe.h"
#include "dns_resolver.h"
#include "tls_context.h"

namespace pudimagent {

namespace {

// Convert a "host:port" or "host" string into host + port for TCP/TLS probes.
bool SplitHostPort(const std::string &host_port, std::string &host,
                   int &port) {
    auto colon = host_port.rfind(':');
    if (colon == std::string::npos) {
        host = host_port;
        port = 80;
        return true;
    }
    host = host_port.substr(0, colon);
    try {
        port = std::stoi(host_port.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return !host.empty();
}

// Returns a Metric with success=false and a detail message.
pudimnetmon::Metric FailureMetric(pudimnetmon::CheckType type,
                                  const std::string &target,
                                  const std::string &detail) {
    pudimnetmon::Metric m;
    m.set_check_type(type);
    m.set_target(target);
    m.set_success(false);
    m.set_detail(detail);
    return m;
}

// Monotone clock in microseconds (portable).
int64_t MonotonicUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- Platform socket helpers (POSIX vs Winsock) ----
#ifdef _WIN32
using Sock = SOCKET;
static const Sock kInvalidSock = INVALID_SOCKET;
static inline bool IsValidSock(Sock s) { return s != INVALID_SOCKET; }
static inline int SockClose(Sock s) { return closesocket(s); }
static inline int SockError() { return WSAGetLastError(); }
static inline std::string SockErrorString() {
    int err = WSAGetLastError();
    if (err == 0) return "no error";
    wchar_t *msg = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(err), 0,
        reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string out;
    if (n > 0 && msg) {
        out = pudimagent::platform::WideToUtf8(std::wstring(msg, n));
        LocalFree(msg);
    } else {
        out = "winsock error " + std::to_string(err);
    }
    return out;
}
static inline bool SetNonBlocking(Sock s, bool nb) {
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}
static inline Sock CreateStreamSocket(int family, int protocol) {
    return socket(family, SOCK_STREAM, protocol);
}
static inline bool IsConnectingError() {
    int e = WSAGetLastError();
    return e == WSAEINPROGRESS || e == WSAEWOULDBLOCK;
}
static inline int SelectWrite(Sock, fd_set *wset, timeval *tv) {
    return select(0, nullptr, wset, nullptr, tv);
}
#else
using Sock = int;
static const Sock kInvalidSock = -1;
static inline bool IsValidSock(Sock s) { return s >= 0; }
static inline int SockClose(Sock s) { return close(s); }
static inline int SockError() { return errno; }
static inline std::string SockErrorString() {
    return std::string(strerror(errno));
}
static inline bool SetNonBlocking(Sock s, bool nb) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}
static inline Sock CreateStreamSocket(int family, int protocol) {
    return socket(family, SOCK_STREAM | SOCK_CLOEXEC, protocol);
}
static inline bool IsConnectingError() { return errno == EINPROGRESS; }
static inline int SelectWrite(Sock s, fd_set *wset, timeval *tv) {
    return select(static_cast<int>(s) + 1, nullptr, wset, nullptr, tv);
}
#endif

// Reads SO_ERROR for a socket, returning the error code (0 = success).
static inline int GetSocketError(Sock s) {
    int so_err = 0;
#ifdef _WIN32
    int len = sizeof(so_err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char *>(&so_err), &len) != 0) {
        return SockError();
    }
#else
    socklen_t len = sizeof(so_err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0) {
        return errno;
    }
#endif
    return so_err;
}

// Resolves host:port and establishes a blocking TCP connection with a 3s
// connect timeout. Returns true + fd on success.
bool ConnectSocket(const std::string &host, int port, Sock &fd,
                   std::string &err) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    LookupResult lr =
        GlobalResolver().Lookup(host, std::to_string(port), hints, 3000);
    if (!lr.ok || !lr.addrs) {
        err = lr.ok ? "no addresses"
                    : (lr.error.empty() ? "resolution failed" : lr.error);
        return false;
    }

    fd = kInvalidSock;
    for (struct addrinfo *ai = lr.addrs.get(); ai != nullptr;
         ai = ai->ai_next) {
        fd = CreateStreamSocket(ai->ai_family, ai->ai_protocol);
        if (!IsValidSock(fd)) continue;

        if (!SetNonBlocking(fd, true)) {
            SockClose(fd);
            fd = kInvalidSock;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && !IsConnectingError()) {
            SockClose(fd);
            fd = kInvalidSock;
            continue;
        }
        struct timeval tv{3, 0};
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        rc = SelectWrite(fd, &wset, &tv);
        if (rc <= 0) {
            SockClose(fd);
            fd = kInvalidSock;
            continue;
        }
        if (GetSocketError(fd) != 0) {
            SockClose(fd);
            fd = kInvalidSock;
            continue;
        }
        // Back to blocking for the caller.
        SetNonBlocking(fd, false);
        break;
    }

    if (!IsValidSock(fd)) {
        err = "connect failed: " + SockErrorString();
        return false;
    }
    return true;
}

// ------------------------------------------------------------
// DNS resolution probe
// ------------------------------------------------------------
void RunDnsProbe(const std::string &host, pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_DNS_RESOLUTION);
    metric.set_target(host);
    metric.set_monotonic_us(MonotonicUs());

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    auto start = std::chrono::steady_clock::now();
    // Bypass the cache so this probe measures a real resolution. The result
    // back-fills the cache under this key (host + service=""), which is a
    // separate entry from the connection probes' "host:port" key - they still
    // deduplicate among themselves via ConnectSocket().
    LookupResult lr = GlobalResolver().Lookup(host, "", hints, 3000, true);
    auto end = std::chrono::steady_clock::now();

    if (!lr.ok) {
        metric.set_success(false);
        metric.set_detail(lr.error.empty() ? "resolution failed" : lr.error);
        return;
    }

    double latency_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    metric.set_latency_ms(latency_ms);
    metric.set_success(true);
}

// ------------------------------------------------------------
// TCP connect probe
// ------------------------------------------------------------
void RunTcpProbe(const std::string &host_port, pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_TCP_CONNECT);
    metric.set_target(host_port);
    metric.set_monotonic_us(MonotonicUs());

    std::string host;
    int port;
    if (!SplitHostPort(host_port, host, port)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_CONNECT, host_port,
                               "invalid host:port");
        return;
    }

    Sock fd = kInvalidSock;
    std::string err;
    auto start = std::chrono::steady_clock::now();
    if (!ConnectSocket(host, port, fd, err)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_CONNECT, host_port,
                               err);
        return;
    }
    auto end = std::chrono::steady_clock::now();
    SockClose(fd);

    double latency_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    metric.set_latency_ms(latency_ms);
    metric.set_success(true);
}

// ------------------------------------------------------------
// TLS handshake probe (via OpenSSL over TCP)
// ------------------------------------------------------------
void RunTlsProbe(const std::string &host_port, pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE);
    metric.set_target(host_port);
    metric.set_monotonic_us(MonotonicUs());

    std::string host;
    int port;
    if (!SplitHostPort(host_port, host, port)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE, host_port,
                               "invalid host:port");
        return;
    }

    Sock fd = kInvalidSock;
    std::string err;
    if (!ConnectSocket(host, port, fd, err)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE, host_port,
                               err);
        return;
    }

    auto start = std::chrono::steady_clock::now();

    // Reuse the process-wide SSL_CTX (no per-call context creation).
    SSL_CTX *ctx = SharedSslCtx();
    if (!ctx) {
        SockClose(fd);
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE, host_port,
                               "SSL_CTX_new failed");
        return;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SockClose(fd);
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE, host_port,
                               "SSL_new failed");
        return;
    }

    SSL_set_fd(ssl, static_cast<int>(fd));
    SSL_set_tlsext_host_name(ssl, host.c_str());

    // Reuse a previously negotiated session so repeated handshakes to the
    // same target use an abbreviated handshake (less CPU, one fewer RTT).
    // Get() returns an owned reference; release it after handing it to SSL.
    SSL_SESSION *cached_session = GlobalSessionCache().Get(host_port);
    if (cached_session) {
        SSL_set_session(ssl, cached_session);
        SSL_SESSION_free(cached_session);
    }

    // Client cert verification is disabled for the handshake probe; full
    // chain validation is done by the TLS certificate probe.
    SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);

    int rc = SSL_connect(ssl);
    bool ok = (rc == 1);
    std::string detail;
    if (!ok) {
        detail = "TLS handshake failed";
    }

    if (ok) {
        // Store the negotiated session for the next cycle (takes ownership of
        // the returned reference).
        SSL_SESSION *new_session = SSL_get1_session(ssl);
        if (new_session) {
            GlobalSessionCache().Put(host_port, new_session);
        }
    }

    auto end = std::chrono::steady_clock::now();
    double latency_ms =
        std::chrono::duration<double, std::milli>(end - start).count();

    SSL_free(ssl);
    SockClose(fd);

    if (!ok) {
        metric.set_success(false);
        metric.set_detail(detail);
        return;
    }
    metric.set_latency_ms(latency_ms);
    metric.set_success(true);
}

// ------------------------------------------------------------
// HTTP probe via libcurl
// ------------------------------------------------------------
size_t NullWriteCallback(char *, size_t size, size_t nmemb, void *) {
    return size * nmemb;
}

// Per-thread persistent curl handle. Reusing the handle across cycles lets
// libcurl keep its connection pool and DNS cache alive, so repeated probes to
// the same target reuse the TCP/TLS connection instead of performing a full
// handshake every interval.
thread_local CURL *t_curl = nullptr;

CURL *GetCurlHandle() {
    static std::once_flag curl_once;
    std::call_once(curl_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (!t_curl) t_curl = curl_easy_init();
    return t_curl;
}

// Defined later in this translation unit.
void RunHttpProbeVersioned(const std::string &url, const std::string &protocol,
                           long curl_version, pudimnetmon::Metric &metric);

void RunHttpProbe(const std::string &url, pudimnetmon::Metric &metric) {
    // Default (curl-negotiated) protocol version.
    RunHttpProbeVersioned(url, "", CURL_HTTP_VERSION_NONE, metric);
}

// ------------------------------------------------------------
// ICMP ping probe (raw sockets on POSIX; ICMP API on Windows)
// ------------------------------------------------------------
#ifndef _WIN32
struct PingResult {
    bool ok = false;
    std::string detail;
    std::vector<double> rtts_ms;
};

PingResult RunSinglePing(int fd, const sockaddr_in &addr, uint16_t id,
                         uint16_t seq) {
    PingResult res;

    char packet[64];
    std::memset(packet, 0, sizeof(packet));

    struct icmphdr *icmp = reinterpret_cast<struct icmphdr *>(packet);
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->un.echo.id = htons(id);
    icmp->un.echo.sequence = htons(seq);

    // Build checksum
    uint32_t sum = 0;
    const uint16_t *ptr = reinterpret_cast<const uint16_t *>(packet);
    for (size_t i = 0; i < sizeof(packet) / 2; i++) {
        sum += ptr[i];
    }
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    icmp->checksum = static_cast<uint16_t>(~sum);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    ssize_t sent = sendto(fd, packet, sizeof(packet), 0,
                          reinterpret_cast<const struct sockaddr *>(&addr),
                          sizeof(addr));
    if (sent < 0) {
        res.detail = "sendto failed: " + std::string(strerror(errno));
        return res;
    }

    // Receive reply with 3s timeout
    struct timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char recv_buf[512];
    for (;;) {
        ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
        if (n < 0) {
            res.detail = (errno == EAGAIN || errno == EWOULDBLOCK)
                             ? "timeout waiting for ICMP reply"
                             : std::string("recv failed: ") + strerror(errno);
            return res;
        }
        // Parse IP header
        struct iphdr *iph = reinterpret_cast<struct iphdr *>(recv_buf);
        if (iph->protocol != IPPROTO_ICMP) continue;
        size_t ip_hdr_len = static_cast<size_t>(iph->ihl) * 4;
        if (static_cast<size_t>(n) < ip_hdr_len + sizeof(struct icmphdr)) continue;

        struct icmphdr *recv_icmp =
            reinterpret_cast<struct icmphdr *>(recv_buf + ip_hdr_len);
        if (recv_icmp->type == ICMP_ECHOREPLY &&
            recv_icmp->un.echo.id == htons(id) &&
            recv_icmp->un.echo.sequence == htons(seq)) {
            struct timespec end;
            clock_gettime(CLOCK_MONOTONIC, &end);
            double rtt_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_nsec - start.tv_nsec) / 1'000'000.0;
            res.ok = true;
            res.rtts_ms.push_back(rtt_ms);
            return res;
        }
    }
}
#endif  // !_WIN32

void RunIcmpProbe(const std::string &host, int count, int gap_ms,
                  pudimnetmon::Metric &loss_metric,
                  pudimnetmon::Metric &rtt_metric,
                  pudimnetmon::Metric &jitter_metric) {
    loss_metric.set_check_type(pudimnetmon::CHECK_TYPE_ICMP_PING);
    loss_metric.set_target(host);
    loss_metric.set_monotonic_us(MonotonicUs());
    rtt_metric.set_check_type(pudimnetmon::CHECK_TYPE_ICMP_PING);
    rtt_metric.set_target(host);
    rtt_metric.set_monotonic_us(MonotonicUs());
    jitter_metric.set_check_type(pudimnetmon::CHECK_TYPE_JITTER);
    jitter_metric.set_target(host);
    jitter_metric.set_monotonic_us(MonotonicUs());

    if (count <= 0) count = 4;
    if (gap_ms < 0) gap_ms = 0;

    // Resolve host
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4 only for raw ICMP in this version
    hints.ai_socktype = SOCK_RAW;

    LookupResult lr = GlobalResolver().Lookup(host, "", hints, 3000);
    if (!lr.ok || !lr.addrs) {
        std::string d = lr.ok ? "no IPv4 address found"
                              : (lr.error.empty() ? "resolution failed" : lr.error);
        loss_metric.set_success(false);
        loss_metric.set_detail(d);
        rtt_metric.set_success(false);
        rtt_metric.set_detail(d);
        jitter_metric.set_success(false);
        jitter_metric.set_detail(d);
        return;
    }

    sockaddr_in dst{};
    bool found = false;
    for (struct addrinfo *ai = lr.addrs.get(); ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            std::memcpy(&dst, ai->ai_addr, sizeof(dst));
            found = true;
            break;
        }
    }

    if (!found) {
        std::string d = "no IPv4 address found";
        loss_metric.set_success(false);
        loss_metric.set_detail(d);
        rtt_metric.set_success(false);
        rtt_metric.set_detail(d);
        jitter_metric.set_success(false);
        jitter_metric.set_detail(d);
        return;
    }

    std::vector<double> rtts;
    int sent_count = 0;
    int lost_count = 0;

#ifdef _WIN32
    // Windows: use the ICMP API (iphlpapi). No admin/capability needed.
    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) {
        std::string d = "IcmpCreateFile failed";
        loss_metric.set_success(false);
        loss_metric.set_detail(d);
        rtt_metric.set_success(false);
        rtt_metric.set_detail(d);
        jitter_metric.set_success(false);
        jitter_metric.set_detail(d);
        return;
    }

    char send_data[32];
    std::memset(send_data, 'P', sizeof(send_data));

    for (int i = 0; i < count; i++) {
        // Reply buffer: ICMP_ECHO_REPLY header + 8 bytes of reply overhead +
        // the echoed payload.
        unsigned char reply[sizeof(ICMP_ECHO_REPLY) + 8 + sizeof(send_data)];
        DWORD rc = IcmpSendEcho2(icmp, nullptr, nullptr, nullptr,
                                 dst.sin_addr.S_un.S_addr, send_data,
                                 static_cast<WORD>(sizeof(send_data)), nullptr,
                                 reply, static_cast<DWORD>(sizeof(reply)), 3000);
        sent_count++;
        if (rc != 0) {
            PICMP_ECHO_REPLY echo = reinterpret_cast<PICMP_ECHO_REPLY>(reply);
            if (echo->Status == IP_SUCCESS) {
                rtts.push_back(static_cast<double>(echo->RoundTripTime));
            } else {
                lost_count++;
            }
        } else {
            lost_count++;
        }
        // Configurable delay between pings
        if (gap_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
        }
    }
    IcmpCloseHandle(icmp);
#else
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        std::string d = "raw socket requires CAP_NET_RAW: " +
                        std::string(strerror(errno));
        loss_metric.set_success(false);
        loss_metric.set_detail(d);
        rtt_metric.set_success(false);
        rtt_metric.set_detail(d);
        jitter_metric.set_success(false);
        jitter_metric.set_detail(d);
        return;
    }

    uint16_t id = static_cast<uint16_t>(getpid() & 0xFFFF);
    for (int i = 0; i < count; i++) {
        PingResult res = RunSinglePing(fd, dst, id, static_cast<uint16_t>(i));
        sent_count++;
        if (res.ok) {
            rtts.insert(rtts.end(), res.rtts_ms.begin(), res.rtts_ms.end());
        } else {
            lost_count++;
        }
        // Configurable delay between pings
        if (gap_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
        }
    }

    close(fd);
#endif

    double loss_pct = (sent_count > 0) ? (100.0 * lost_count / sent_count) : 100.0;
    loss_metric.set_packet_loss_pct(loss_pct);
    loss_metric.set_success(true);

    rtt_metric.set_success(true);
    if (!rtts.empty()) {
        double avg = 0.0;
        for (double r : rtts) avg += r;
        avg /= rtts.size();
        double mean = avg;
        double var = 0.0;
        for (double r : rtts) var += (r - mean) * (r - mean);
        var /= rtts.size();
        double stddev = std::sqrt(var);

        rtt_metric.set_rtt_ms(avg);
        jitter_metric.set_jitter_ms(stddev);
        jitter_metric.set_success(true);
    } else {
        rtt_metric.set_success(false);
        rtt_metric.set_detail("all pings lost");
        jitter_metric.set_success(false);
        jitter_metric.set_detail("all pings lost");
    }
}

// ------------------------------------------------------------
// DNS record probe (A/AAAA/CNAME + optional expected-value validation)
// ------------------------------------------------------------
void RunDnsRecordProbe(const std::string &host,
                       const std::vector<std::string> &expected,
                       pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_DNS_RECORD);
    metric.set_target(host);
    metric.set_monotonic_us(MonotonicUs());
    metric.set_success(true);

    auto attrs = metric.mutable_attributes();

    // Canonical name (CNAME target) via AI_CANONNAME.
    {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_CANONNAME;
        LookupResult lr = GlobalResolver().Lookup(host, "", hints, 3000);
        if (lr.ok && lr.addrs && lr.addrs->ai_canonname &&
            *lr.addrs->ai_canonname) {
            std::string canon = lr.addrs->ai_canonname;
            if (canon != host) (*attrs)["CNAME"] = canon;
        }
    }

    // A records.
    {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        LookupResult lr = GlobalResolver().Lookup(host, "", hints, 3000);
        if (lr.ok && lr.addrs) {
            std::string a;
            for (auto *ai = lr.addrs.get(); ai; ai = ai->ai_next) {
                char buf[INET_ADDRSTRLEN];
                auto *sin = reinterpret_cast<struct sockaddr_in *>(ai->ai_addr);
                if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
                    if (!a.empty()) a += ",";
                    a += buf;
                }
            }
            if (!a.empty()) (*attrs)["A"] = a;
        }
    }

    // AAAA records.
    {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        LookupResult lr = GlobalResolver().Lookup(host, "", hints, 3000);
        if (lr.ok && lr.addrs) {
            std::string aaaa;
            for (auto *ai = lr.addrs.get(); ai; ai = ai->ai_next) {
                char buf[INET6_ADDRSTRLEN];
                auto *sin6 = reinterpret_cast<struct sockaddr_in6 *>(ai->ai_addr);
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) {
                    if (!aaaa.empty()) aaaa += ",";
                    aaaa += buf;
                }
            }
            if (!aaaa.empty()) (*attrs)["AAAA"] = aaaa;
        }
    }

    // Validate against expected records ("A:1.2.3.4", "CNAME:www.example.com").
    bool mismatch = false;
    std::string detail;
    for (const auto &exp : expected) {
        auto colon = exp.find(':');
        if (colon == std::string::npos) continue;
        std::string type = exp.substr(0, colon);
        std::string value = exp.substr(colon + 1);
        auto it = attrs->find(type);
        std::string actual = (it != attrs->end()) ? it->second : "";
        if (actual != value) {
            mismatch = true;
            detail += "expected " + type + "=" + value + " got " +
                      (actual.empty() ? "none" : actual) + "; ";
        }
    }
    if (mismatch) {
        metric.set_success(false);
        metric.set_detail(detail);
    }
}

// ------------------------------------------------------------
// TCP retransmission probe (getsockopt TCP_INFO)
// ------------------------------------------------------------
void RunTcpRetransmitProbe(const std::string &host_port,
                           pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_TCP_RETRANSMIT);
    metric.set_target(host_port);
    metric.set_monotonic_us(MonotonicUs());

    std::string host;
    int port;
    if (!SplitHostPort(host_port, host, port)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_RETRANSMIT,
                               host_port, "invalid host:port");
        return;
    }
    Sock fd;
    std::string err;
    if (!ConnectSocket(host, port, fd, err)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_RETRANSMIT,
                               host_port, err);
        return;
    }
#ifdef _WIN32
    // Windows 10+: SIO_TCP_INFO returns a TCP_INFO_v0 struct; TotalRetrans is
    // the cumulative retransmission count for the connection.
    TCP_INFO_v0 info{};
    DWORD bytes = 0;
    if (WSAIoctl(fd, SIO_TCP_INFO, nullptr, 0, &info, sizeof(info), &bytes,
                 nullptr, nullptr) == 0) {
        metric.set_status_code(static_cast<int64_t>(info.TotalRetrans));
        metric.set_success(true);
    } else {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_RETRANSMIT,
                               host_port, "WSAIoctl(SIO_TCP_INFO) failed");
    }
#else
    struct tcp_info info;
    std::memset(&info, 0, sizeof(info));
    socklen_t len = sizeof(info);
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
        metric.set_status_code(info.tcpi_total_retrans);
        metric.set_success(true);
    } else {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_RETRANSMIT,
                               host_port, "getsockopt(TCP_INFO) failed");
    }
#endif
    SockClose(fd);
}

// ------------------------------------------------------------
// TLS certificate validation probe
// ------------------------------------------------------------
void RunTlsCertProbe(const std::string &host_port, pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE);
    metric.set_target(host_port);
    metric.set_monotonic_us(MonotonicUs());
    metric.set_success(true);

    std::string host;
    int port;
    if (!SplitHostPort(host_port, host, port)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE,
                               host_port, "invalid host:port");
        return;
    }
    Sock fd;
    std::string err;
    if (!ConnectSocket(host, port, fd, err)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE,
                               host_port, err);
        return;
    }

    // Reuse the process-wide SSL_CTX (TLS >= 1.2 + system CA bundle loaded
    // once at startup).
    SSL_CTX *ctx = SharedSslCtx();
    if (!ctx) {
        SockClose(fd);
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE,
                               host_port, "SSL_CTX_new failed");
        return;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SockClose(fd);
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE,
                               host_port, "SSL_new failed");
        return;
    }
    SSL_set_fd(ssl, static_cast<int>(fd));
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);

    // NOTE: no TLS session resumption here on purpose - the certificate probe
    // needs a full handshake so the server actually presents its certificate.

    int rc = SSL_connect(ssl);
    long verify_result = SSL_get_verify_result(ssl);
    X509 *cert = SSL_get1_peer_certificate(ssl);

    auto attrs = metric.mutable_attributes();

    if (!cert) {
        metric.set_success(false);
        metric.set_detail(rc == 1 ? "no peer certificate presented"
                                  : "TLS handshake failed");
    } else {
        char *subj = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
        char *issuer = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
        if (subj) {
            (*attrs)["tls_cert_subject"] = subj;
            OPENSSL_free(subj);
        }
        if (issuer) {
            (*attrs)["tls_cert_issuer"] = issuer;
            OPENSSL_free(issuer);
        }

        // Days until expiry (negative if already expired).
        ASN1_TIME *not_after =
            const_cast<ASN1_TIME *>(X509_get0_notAfter(cert));
        int days = 0, secs = 0;
        if (not_after && ASN1_TIME_diff(&days, &secs, nullptr, not_after)) {
            (*attrs)["tls_cert_expiry_days"] = std::to_string(days);
        }

        // Not-yet-valid certificate?
        ASN1_TIME *not_before =
            const_cast<ASN1_TIME *>(X509_get0_notBefore(cert));
        if (not_before && ASN1_TIME_diff(&days, &secs, not_before, nullptr) &&
            days > 0) {
            (*attrs)["tls_cert_not_yet_valid"] = "true";
        }

        int host_ok = X509_check_host(cert, host.c_str(), host.size(), 0, nullptr);
        (*attrs)["tls_cert_hostname_match"] = (host_ok == 1) ? "true" : "false";

        // Validity = chain OK + hostname match.
        if (verify_result != X509_V_OK) {
            metric.set_success(false);
            metric.set_detail(std::string("certificate chain invalid: ") +
                              X509_verify_cert_error_string(verify_result));
        } else if (host_ok != 1) {
            metric.set_success(false);
            metric.set_detail("certificate hostname does not match");
        }
        X509_free(cert);
    }

    SSL_free(ssl);
    SockClose(fd);
}

// ------------------------------------------------------------
// HTTP probe with an explicit protocol version (HTTP/1.1, HTTP/2, HTTP/3)
// ------------------------------------------------------------
void RunHttpProbeVersioned(const std::string &url, const std::string &protocol,
                           long curl_version, pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_HTTP_REQUEST);
    metric.set_monotonic_us(MonotonicUs());
    // Append the protocol to the target so the dashboard can group by it
    // (e.g. "https://example.com;http2").
    metric.set_target(protocol.empty() ? url : url + ";" + protocol);

    CURL *curl = GetCurlHandle();
    if (!curl) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_HTTP_REQUEST,
                               metric.target(), "curl init failed");
        return;
    }
    // Reset per-request options while keeping the handle (and its connection
    // pool + DNS cache) for reuse.
    curl_easy_reset(curl);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NullWriteCallback);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (curl_version != CURL_HTTP_VERSION_NONE) {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, curl_version);
    }

    static const char kUserAgent[] = "PudimNetMon-Agent/0.1.0";
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::string detail = curl_easy_strerror(rc);
        metric.set_success(false);
        metric.set_detail(detail);
        return;
    }

    long status_code = 0;
    double total_ms = 0.0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_ms);

    metric.set_latency_ms(total_ms * 1000.0);
    metric.set_status_code(status_code);
    metric.set_success(true);
}

} // anonymous namespace

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------
void ProbeDns(const std::string &host, pudimnetmon::Metric &metric) {
    RunDnsProbe(host, metric);
}

void ProbeDnsRecord(const std::string &host,
                    const std::vector<std::string> &expected,
                    pudimnetmon::Metric &metric) {
    RunDnsRecordProbe(host, expected, metric);
}

void ProbeTcp(const std::string &host_port, pudimnetmon::Metric &metric) {
    RunTcpProbe(host_port, metric);
}

void ProbeTcpRetransmit(const std::string &host_port, pudimnetmon::Metric &metric) {
    RunTcpRetransmitProbe(host_port, metric);
}

void ProbeTls(const std::string &host_port, pudimnetmon::Metric &metric) {
    RunTlsProbe(host_port, metric);
}

void ProbeTlsCert(const std::string &host_port, pudimnetmon::Metric &metric) {
    RunTlsCertProbe(host_port, metric);
}

void ProbeHttp(const std::string &url, pudimnetmon::Metric &metric) {
    RunHttpProbe(url, metric);
}

void ProbeHttpProtocol(const std::string &url, const std::string &protocol,
                       pudimnetmon::Metric &metric) {
    long curl_version = CURL_HTTP_VERSION_NONE;
    if (protocol == "http1.1") {
        curl_version = CURL_HTTP_VERSION_1_1;
    } else if (protocol == "http2") {
        curl_version = CURL_HTTP_VERSION_2_0;
    } else if (protocol == "http3") {
        curl_version = CURL_HTTP_VERSION_3;
    }
    RunHttpProbeVersioned(url, protocol, curl_version, metric);
}

void ProbeIcmp(const std::string &host, int count, int gap_ms,
               pudimnetmon::Metric &loss_metric,
               pudimnetmon::Metric &rtt_metric,
               pudimnetmon::Metric &jitter_metric) {
    RunIcmpProbe(host, count, gap_ms, loss_metric, rtt_metric, jitter_metric);
}

void RunAllProbes(const ProbeConfig &config,
                  std::vector<pudimnetmon::Metric> &out_metrics) {
    // Phase 5: kernel clock offset (always emitted when enabled).
    if (config.ntp_check) {
        pudimnetmon::Metric ntp;
        ProbeNtpOffset(ntp);
        out_metrics.push_back(std::move(ntp));
    }
    for (const auto &t : config.dns_targets) {
        pudimnetmon::Metric m;
        RunDnsProbe(t, m);
        out_metrics.push_back(std::move(m));
        // Phase 4: DNS record lookup + optional expected-value validation.
        auto it = config.dns_expected.find(t);
        std::vector<std::string> expected =
            (it != config.dns_expected.end()) ? it->second
                                              : std::vector<std::string>();
        pudimnetmon::Metric rec;
        RunDnsRecordProbe(t, expected, rec);
        out_metrics.push_back(std::move(rec));
    }
    for (const auto &t : config.tcp_targets) {
        pudimnetmon::Metric m;
        RunTcpProbe(t, m);
        out_metrics.push_back(std::move(m));
        if (config.tcp_retransmit_check) {
            pudimnetmon::Metric r;
            RunTcpRetransmitProbe(t, r);
            out_metrics.push_back(std::move(r));
        }
        if (config.tcp_handshake_capture) {
            pudimnetmon::Metric h;
            ProbeTcpHandshake(t, h);
            out_metrics.push_back(std::move(h));
        }
    }
    for (const auto &t : config.tls_targets) {
        pudimnetmon::Metric m;
        RunTlsProbe(t, m);
        out_metrics.push_back(std::move(m));
        if (config.tls_cert_check) {
            pudimnetmon::Metric c;
            RunTlsCertProbe(t, c);
            out_metrics.push_back(std::move(c));
        }
    }
    for (const auto &t : config.http_targets) {
        if (config.http_protocols.empty()) {
            pudimnetmon::Metric m;
            RunHttpProbe(t, m);
            out_metrics.push_back(std::move(m));
        } else {
            for (const auto &p : config.http_protocols) {
                pudimnetmon::Metric m;
                ProbeHttpProtocol(t, p, m);
                out_metrics.push_back(std::move(m));
            }
        }
    }
    for (const auto &t : config.ping_targets) {
        pudimnetmon::Metric loss, rtt, jitter;
        RunIcmpProbe(t, config.ping_count, config.ping_gap_ms, loss, rtt,
                     jitter);
        out_metrics.push_back(std::move(loss));
        out_metrics.push_back(std::move(rtt));
        out_metrics.push_back(std::move(jitter));
    }
}

} // namespace pudimagent