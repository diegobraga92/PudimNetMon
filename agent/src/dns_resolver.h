#pragma once

#include <chrono>
#include <memory>
#include <string>

struct addrinfo;

namespace pudimagent {

// Owns a deep-copied getaddrinfo() result chain (a regular addrinfo linked
// list). The chain is freed when the last shared reference goes away, so it is
// safe to return from the cache and share between threads.
using AddrInfoPtr = std::shared_ptr<struct addrinfo>;

struct LookupResult {
    bool ok = false;
    std::string error;       // human-readable message when !ok
    AddrInfoPtr addrs;       // valid (non-null) when ok
    std::string canonname;   // filled when AI_CANONNAME was requested
};

// Thread-safe, time-bounded, TTL-cached DNS resolver.
//
// Lookup() never blocks longer than `timeout_ms`: if the underlying
// getaddrinfo() does not finish in time the call returns ok=false while a
// detached resolver thread keeps running and back-fills the cache, so the next
// cycle is served from cache instead of stalling the probe loop again.
class DnsResolver {
public:
    DnsResolver();
    ~DnsResolver();

    DnsResolver(const DnsResolver &) = delete;
    DnsResolver &operator=(const DnsResolver &) = delete;

    // Resolves `host`/`service` with the supplied hints. A fresh cache entry
    // returns immediately; otherwise a time-bounded background lookup is
    // started and awaited for up to `timeout_ms`. With `bypass_cache` the
    // cache is skipped entirely (still time-bounded) but the result still
    // back-fills the cache for other callers - use this for probes that must
    // measure a real resolution instead of a cache hit.
    LookupResult Lookup(const std::string &host, const std::string &service,
                        const struct addrinfo &hints, int timeout_ms = 3000,
                        bool bypass_cache = false);

    // Removes all cached and in-flight state (mainly for tests).
    void Clear();

    // How long successful resolutions stay cached.
    static constexpr int kDefaultTtlMs = 60000;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Process-wide resolver. Safe for concurrent use from any probe thread.
DnsResolver &GlobalResolver();

}  // namespace pudimagent
