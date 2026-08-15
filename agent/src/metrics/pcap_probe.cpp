// TCP handshake capture probe using libpcap. Measures per-segment timings of
// the TCP three-way handshake (SYN -> SYN-ACK -> ACK). When libpcap is not
// available in the build, the probe degrades to a failure metric.
#include <chrono>
#include <cstring>
#include <string>

#include "metrics.pb.h"
#include "probes.h"
#include "dns_resolver.h"

#ifdef HAVE_LIBPCAP

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <pcap/pcap.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mutex>

namespace pudimagent {

namespace {

std::string MonotonicUsStr() {
    return std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The capture device rarely changes; enumerating all interfaces via
// pcap_findalldevs() on every probe is comparatively expensive. Cache the
// chosen device and refresh it periodically so interface changes are still
// picked up.
std::string GetCaptureDevice() {
    static std::mutex dev_mu;
    static std::string device;
    static std::chrono::steady_clock::time_point refreshed{};
    constexpr auto kRefreshInterval = std::chrono::minutes(5);

    std::lock_guard<std::mutex> lock(dev_mu);
    auto now = std::chrono::steady_clock::now();
    if (!device.empty() && (now - refreshed) < kRefreshInterval) {
        return device;
    }

    device.clear();
    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_if_t *alldevs = nullptr;
    if (pcap_findalldevs(&alldevs, errbuf) < 0 || !alldevs) {
        refreshed = now;
        return "";
    }
    for (pcap_if_t *d = alldevs; d; d = d->next) {
        std::string name = d->name ? d->name : "";
        if (name == "lo" || name.rfind("lo", 0) == 0) continue;
        device = name;
        break;
    }
    pcap_freealldevs(alldevs);
    refreshed = now;
    return device;
}

} // anonymous namespace

void ProbeTcpHandshake(const std::string &host_port, pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_TCP_HANDSHAKE);
    metric.set_target(host_port);
    metric.set_monotonic_us(std::stoll(MonotonicUsStr()));

    auto fail = [&](const std::string &detail) {
        metric.set_check_type(pudimnetmon::CHECK_TYPE_TCP_HANDSHAKE);
        metric.set_target(host_port);
        metric.set_success(false);
        metric.set_detail(detail);
    };

    // Parse host:port
    auto colon = host_port.rfind(':');
    if (colon == std::string::npos) { fail("invalid host:port"); return; }
    std::string host = host_port.substr(0, colon);
    int port = 0;
    try {
        port = std::stoi(host_port.substr(colon + 1));
    } catch (...) { fail("invalid port"); return; }

    // Resolve target as IPv4 (keeps the pcap parsing simple). Uses the shared
    // time-bounded resolver so a slow DNS answer cannot stall the probe loop.
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    LookupResult lr =
        GlobalResolver().Lookup(host, std::to_string(port), hints, 3000);
    if (!lr.ok || !lr.addrs) {
        fail(lr.ok ? "no IPv4 address found"
                   : (lr.error.empty() ? "resolution failed" : lr.error));
        return;
    }
    auto *sin = reinterpret_cast<struct sockaddr_in *>(lr.addrs->ai_addr);
    uint32_t target_ip = sin->sin_addr.s_addr;

    // Pick a non-loopback capture device (cached; refreshed periodically).
    std::string dev = GetCaptureDevice();
    if (dev.empty()) { fail("no non-loopback capture device"); return; }

    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_t *handle = pcap_open_live(dev.c_str(), 65535, 1, 50, errbuf);
    if (!handle) { fail(std::string("pcap_open_live: ") + errbuf); return; }

    struct bpf_program fp {};
    std::string filter = "tcp port " + std::to_string(port);
    if (pcap_compile(handle, &fp, filter.c_str(), 1, PCAP_NETMASK_UNKNOWN) < 0) {
        pcap_close(handle); fail("pcap_compile failed"); return;
    }
    if (pcap_setfilter(handle, &fp) < 0) {
        pcap_freecode(&fp); pcap_close(handle); fail("pcap_setfilter failed"); return;
    }
    pcap_freecode(&fp);

    // Non-blocking connect.
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { pcap_close(handle); fail("socket failed"); return; }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    sa.sin_addr.s_addr = target_ip;
    int rc = connect(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd); pcap_close(handle); fail("connect failed"); return;
    }

    // Local IP + ephemeral source port (for matching our own packets).
    struct sockaddr_in local {};
    socklen_t llen = sizeof(local);
    getsockname(fd, reinterpret_cast<struct sockaddr *>(&local), &llen);
    uint32_t local_ip = local.sin_addr.s_addr;
    uint16_t local_port = ntohs(local.sin_port);

    // Capture SYN, SYN-ACK, ACK with timestamps.
    double t_syn = -1.0, t_synack = -1.0, t_ack = -1.0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        struct pcap_pkthdr *hdr = nullptr;
        const u_char *data = nullptr;
        int prc = pcap_next_ex(handle, &hdr, &data);
        if (prc < 0) break;       // capture error
        if (prc == 0) continue;   // timeout, keep looping

        if (!data || hdr->caplen < 14) continue;
        const u_char *ip = data + 14;                 // skip ethernet header
        if ((ip[0] >> 4) != 4) continue;              // IPv4 only
        int ip_hdr_len = (ip[0] & 0x0f) * 4;
        if (static_cast<int>(hdr->caplen) < 14 + ip_hdr_len + 20) continue;
        if (ip[9] != IPPROTO_TCP) continue;

        const struct tcphdr *tcp =
            reinterpret_cast<const struct tcphdr *>(ip + ip_hdr_len);
        uint32_t src_ip = 0, dst_ip = 0;
        std::memcpy(&src_ip, ip + 12, 4);
        std::memcpy(&dst_ip, ip + 16, 4);
        uint16_t sport = ntohs(tcp->source);
        uint16_t dport = ntohs(tcp->dest);
        bool is_syn = (tcp->syn != 0) && (tcp->ack == 0);
        bool is_synack = (tcp->syn != 0) && (tcp->ack != 0);
        bool is_ack = (tcp->syn == 0) && (tcp->ack != 0);

        double ts = static_cast<double>(hdr->ts.tv_sec) +
                    static_cast<double>(hdr->ts.tv_usec) / 1e6;

        if (is_syn && src_ip == local_ip && sport == local_port &&
            dst_ip == target_ip && dport == static_cast<uint16_t>(port)) {
            if (t_syn < 0) t_syn = ts;
        } else if (is_synack && src_ip == target_ip &&
                   sport == static_cast<uint16_t>(port) &&
                   dst_ip == local_ip && dport == local_port) {
            if (t_synack < 0) t_synack = ts;
        } else if (is_ack && src_ip == local_ip && sport == local_port &&
                   dst_ip == target_ip && dport == static_cast<uint16_t>(port)) {
            if (t_ack < 0) t_ack = ts;
        }

        if (t_synack > 0 && t_ack > 0) break;
    }

    close(fd);
    pcap_close(handle);

    if (t_syn < 0 || t_synack < 0) {
        fail("handshake packets not captured (timeout)");
        return;
    }
    metric.set_latency_ms((t_synack - t_syn) * 1000.0);
    auto attrs = metric.mutable_attributes();
    if (t_ack > 0) {
        (*attrs)["synack_ack_ms"] =
            std::to_string((t_ack - t_synack) * 1000.0);
    }
    metric.set_success(true);
}

} // namespace pudimagent

#else  // !HAVE_LIBPCAP

namespace pudimagent {

void ProbeTcpHandshake(const std::string &host_port, pudimnetmon::Metric &metric) {
    metric.set_check_type(pudimnetmon::CHECK_TYPE_TCP_HANDSHAKE);
    metric.set_target(host_port);
    metric.set_success(false);
    metric.set_detail("libpcap not available in this build");
}

} // namespace pudimagent

#endif

