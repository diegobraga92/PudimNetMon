#pragma once

#include <chrono>
#include <memory>
#include <string>

struct addrinfo;

namespace pudimagent {

using AddrInfoPtr = std::shared_ptr<struct addrinfo>;

struct LookupResult {
    bool ok = false;
    std::string error;
    AddrInfoPtr addrs;
    std::string canonname;
};

class DnsResolver {
public:
    DnsResolver();
    ~DnsResolver();

    DnsResolver(const DnsResolver &) = delete;
    DnsResolver &operator=(const DnsResolver &) = delete;

    LookupResult Lookup(const std::string &host, const std::string &service,
                        const struct addrinfo &hints, int timeout_ms = 3000,
                        bool bypass_cache = false);

    void Clear();

    static constexpr int kDefaultTtlMs = 60000;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

DnsResolver &GlobalResolver();

}  // namespace pudimagent
