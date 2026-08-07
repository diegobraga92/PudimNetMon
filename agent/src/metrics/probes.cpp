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
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/ssl.h>

#include "probes.h"

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

void RunHttpProbe(const std::string &url, pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_HTTP_REQUEST);
    metric.set_target(url);
    metric.set_monotonic_us(MonotonicUs());

    static std::once_flag curl_once;
    std::call_once(curl_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL *curl = curl_easy_init();
    if (!curl) {
        metric = FailureMetric(pudimnetmon::CHECK_TYPE_HTTP_REQUEST, url,
                               "curl init failed");
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NullWriteCallback);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Add a User-Agent to avoid 403s from some CDNs
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

} // anonymous namespace

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------
void ProbeDns(const std::string &host, pudimnetmon::Metric &metric) {
    RunDnsProbe(host, metric);
}

void ProbeTcp(const std::string &host_port, pudimnetmon::Metric &metric) {
    RunTcpProbe(host_port, metric);
}

void ProbeTls(const std::string &host_port, pudimnetmon::Metric &metric) {
    RunTlsProbe(host_port, metric);
}

void ProbeHttp(const std::string &url, pudimnetmon::Metric &metric) {
    RunHttpProbe(url, metric);
}

void ProbeIcmp(const std::string &host, int count,
               pudimnetmon::Metric &loss_metric,
               pudimnetmon::Metric &rtt_metric,
               pudimnetmon::Metric &jitter_metric) {
    RunIcmpProbe(host, count, loss_metric, rtt_metric, jitter_metric);
}

void RunAllProbes(const ProbeConfig &config,
                  std::vector<pudimnetmon::Metric> &out_metrics) {
    for (const auto &t : config.dns_targets) {
        pudimnetmon::Metric m;
        RunDnsProbe(t, m);
        out_metrics.push_back(std::move(m));
    }
    for (const auto &t : config.tcp_targets) {
        pudimnetmon::Metric m;
        RunTcpProbe(t, m);
        out_metrics.push_back(std::move(m));
    }
    for (const auto &t : config.tls_targets) {
        pudimnetmon::Metric m;
        RunTlsProbe(t, m);
        out_metrics.push_back(std::move(m));
    }
    for (const auto &t : config.http_targets) {
        pudimnetmon::Metric m;
        RunHttpProbe(t, m);
        out_metrics.push_back(std::move(m));
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