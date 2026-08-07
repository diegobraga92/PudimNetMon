#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

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

#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "probes.h"
#include "ntp_probe.h"

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

// Monotone clock in microseconds.
int64_t MonotonicUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
}

// Resolves host:port and establishes a blocking TCP connection with a 3s
// connect timeout. Returns true + fd on success.
bool ConnectSocket(const std::string &host, int port, int &fd,
                   std::string &err) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                    &result) != 0) {
        err = "resolution failed";
        return false;
    }

    fd = -1;
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                    ai->ai_protocol);
        if (fd < 0) continue;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        struct timeval tv{3, 0};
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        rc = select(fd + 1, nullptr, &wset, nullptr, &tv);
        if (rc <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
        if (so_err != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        // Back to blocking for the caller.
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        break;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        err = "connect failed";
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

    struct addrinfo *result = nullptr;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &result);

    if (rc != 0) {
        metric.set_success(false);
        metric.set_detail(gai_strerror(rc));
        return;
    }
    freeaddrinfo(result);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double latency_ms =
        (end.tv_sec - start.tv_sec) * 1000.0 +
        (end.tv_nsec - start.tv_nsec) / 1'000'000.0;
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

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                    &result) != 0) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_CONNECT, host_port,
                               "resolution failed");
        return;
    }

    int fd = -1;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_CONNECT, host_port,
                               "connect failed");
        return;
    }
    close(fd);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double latency_ms =
        (end.tv_sec - start.tv_sec) * 1000.0 +
        (end.tv_nsec - start.tv_nsec) / 1'000'000.0;
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

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                    &result) != 0) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE, host_port,
                               "resolution failed");
        return;
    }

    int fd = -1;
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        // Non-blocking connect with timeout
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        struct timeval tv{3, 0};
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        rc = select(fd + 1, nullptr, &wset, nullptr, &tv);
        if (rc <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
        if (so_err != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        // Back to blocking for SSL
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        break;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE, host_port,
                               "connect failed");
        return;
    }

    static std::once_flag ssl_once;
    std::call_once(ssl_once, []() { SSL_library_init(); });

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        close(fd);
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE, host_port,
                               "SSL_CTX_new failed");
        return;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        close(fd);
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_HANDSHAKE, host_port,
                               "SSL_new failed");
        return;
    }

    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());

    // Client cert verification is disabled for now (Phase 0/1);
    // TLS cert validation is a Phase 4 feature.
    SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);

    int rc = SSL_connect(ssl);
    bool ok = (rc == 1);
    std::string detail;
    if (!ok) {
        detail = "TLS handshake failed";
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double latency_ms =
        (end.tv_sec - start.tv_sec) * 1000.0 +
        (end.tv_nsec - start.tv_nsec) / 1'000'000.0;

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);

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

// Defined later in this translation unit.
void RunHttpProbeVersioned(const std::string &url, const std::string &protocol,
                           long curl_version, pudimnetmon::Metric &metric);

void RunHttpProbe(const std::string &url, pudimnetmon::Metric &metric) {
    // Default (curl-negotiated) protocol version.
    RunHttpProbeVersioned(url, "", CURL_HTTP_VERSION_NONE, metric);
}

// ------------------------------------------------------------
// ICMP ping probe (requires CAP_NET_RAW)
// ------------------------------------------------------------
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

void RunIcmpProbe(const std::string &host, int count,
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

    // Resolve host
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4 only for raw ICMP in this version
    hints.ai_socktype = SOCK_RAW;

    struct addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
        std::string d = "resolution failed";
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
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            std::memcpy(&dst, ai->ai_addr, sizeof(dst));
            found = true;
            break;
        }
    }
    freeaddrinfo(result);

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
    std::vector<double> rtts;
    int sent_count = 0;
    int lost_count = 0;

    for (int i = 0; i < count; i++) {
        PingResult res = RunSinglePing(fd, dst, id, static_cast<uint16_t>(i));
        sent_count++;
        if (res.ok) {
            rtts.insert(rtts.end(), res.rtts_ms.begin(), res.rtts_ms.end());
        } else {
            lost_count++;
        }
        // 200ms between pings
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    close(fd);

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
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
            if (res->ai_canonname && *res->ai_canonname) {
                std::string canon = res->ai_canonname;
                if (canon != host) (*attrs)["CNAME"] = canon;
            }
            freeaddrinfo(res);
        }
    }

    // A records.
    {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0) {
            std::string a;
            for (auto *ai = res; ai; ai = ai->ai_next) {
                char buf[INET_ADDRSTRLEN];
                auto *sin = reinterpret_cast<struct sockaddr_in *>(ai->ai_addr);
                if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
                    if (!a.empty()) a += ",";
                    a += buf;
                }
            }
            freeaddrinfo(res);
            if (!a.empty()) (*attrs)["A"] = a;
        }
    }

    // AAAA records.
    {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0) {
            std::string aaaa;
            for (auto *ai = res; ai; ai = ai->ai_next) {
                char buf[INET6_ADDRSTRLEN];
                auto *sin6 = reinterpret_cast<struct sockaddr_in6 *>(ai->ai_addr);
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) {
                    if (!aaaa.empty()) aaaa += ",";
                    aaaa += buf;
                }
            }
            freeaddrinfo(res);
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
    int fd;
    std::string err;
    if (!ConnectSocket(host, port, fd, err)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TCP_RETRANSMIT,
                               host_port, err);
        return;
    }
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
    close(fd);
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
    int fd;
    std::string err;
    if (!ConnectSocket(host, port, fd, err)) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE,
                               host_port, err);
        return;
    }

    static std::once_flag ssl_once;
    std::call_once(ssl_once, []() { SSL_library_init(); });

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        close(fd);
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE,
                               host_port, "SSL_CTX_new failed");
        return;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    // Full chain validation against the system CA bundle.
    SSL_CTX_set_default_verify_paths(ctx);

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        close(fd);
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_TLS_CERTIFICATE,
                               host_port, "SSL_new failed");
        return;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);

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
    SSL_CTX_free(ctx);
    close(fd);
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

    static std::once_flag curl_once;
    std::call_once(curl_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL *curl = curl_easy_init();
    if (!curl) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_HTTP_REQUEST,
                               metric.target(), "curl init failed");
        return;
    }

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
        curl_easy_cleanup(curl);
        metric.set_success(false);
        metric.set_detail(detail);
        return;
    }

    long status_code = 0;
    double total_ms = 0.0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_ms);

    curl_easy_cleanup(curl);

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

void ProbeIcmp(const std::string &host, int count,
               pudimnetmon::Metric &loss_metric,
               pudimnetmon::Metric &rtt_metric,
               pudimnetmon::Metric &jitter_metric) {
    RunIcmpProbe(host, count, loss_metric, rtt_metric, jitter_metric);
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
        RunIcmpProbe(t, config.ping_count, loss, rtt, jitter);
        out_metrics.push_back(std::move(loss));
        out_metrics.push_back(std::move(rtt));
        out_metrics.push_back(std::move(jitter));
    }
}

} // namespace pudimagent