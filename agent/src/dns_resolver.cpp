#include "dns_resolver.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace pudimagent {

namespace {

void FreeAddrinfoChain(struct addrinfo *head) {
    while (head) {
        struct addrinfo *next = head->ai_next;
        std::free(head->ai_addr);
        std::free(head->ai_canonname);
        std::free(head);
        head = next;
    }
}

char *StrDup(const char *s) {
    if (!s) return nullptr;
    size_t n = std::strlen(s);
    char *out = static_cast<char *>(std::malloc(n + 1));
    if (!out) return nullptr;
    std::memcpy(out, s, n + 1);
    return out;
}

// Deep-copies a getaddrinfo() chain into a self-owned representation that can
// outlive the original allocation.
AddrInfoPtr DeepCopyChain(const struct addrinfo *src) {
    if (!src) return AddrInfoPtr();
    struct addrinfo *head = nullptr;
    struct addrinfo **tail = &head;
    for (const struct addrinfo *ai = src; ai; ai = ai->ai_next) {
        struct addrinfo *node =
            static_cast<struct addrinfo *>(std::calloc(1, sizeof(struct addrinfo)));
        if (!node) {
            FreeAddrinfoChain(head);
            return AddrInfoPtr();
        }
        node->ai_flags = ai->ai_flags;
        node->ai_family = ai->ai_family;
        node->ai_socktype = ai->ai_socktype;
        node->ai_protocol = ai->ai_protocol;
        node->ai_addrlen = ai->ai_addrlen;
        if (ai->ai_addr && ai->ai_addrlen > 0) {
            node->ai_addr =
                static_cast<struct sockaddr *>(std::calloc(1, ai->ai_addrlen));
            if (!node->ai_addr) {
                node->ai_addrlen = 0;
            } else {
                std::memcpy(node->ai_addr, ai->ai_addr, ai->ai_addrlen);
            }
        }
        if (ai->ai_canonname) node->ai_canonname = StrDup(ai->ai_canonname);
        *tail = node;
        tail = &node->ai_next;
    }
    return AddrInfoPtr(head, FreeAddrinfoChain);
}

std::string MakeKey(const std::string &host, const std::string &service,
                    const struct addrinfo &hints) {
    return host + "\x1f" + service + "\x1f" +
           std::to_string(hints.ai_family) + "\x1f" +
           std::to_string(hints.ai_socktype) + "\x1f" +
           std::to_string(hints.ai_protocol) + "\x1f" +
           std::to_string(hints.ai_flags & AI_CANONNAME);
}

}  // namespace

struct DnsResolver::Impl {
    struct CacheEntry {
        LookupResult result;
        std::chrono::steady_clock::time_point expiry;
    };
    struct PendingLookup {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        LookupResult result;
    };

    std::mutex mu;
    std::map<std::string, CacheEntry> cache;
    std::map<std::string, std::shared_ptr<PendingLookup>> inflight;
    int ttl_ms = DnsResolver::kDefaultTtlMs;
};

DnsResolver::DnsResolver() : impl_(std::make_shared<Impl>()) {}
DnsResolver::~DnsResolver() = default;

void DnsResolver::Clear() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->cache.clear();
    impl_->inflight.clear();
}

LookupResult DnsResolver::Lookup(const std::string &host,
                                 const std::string &service,
                                 const struct addrinfo &hints,
                                 int timeout_ms, bool bypass_cache) {
    const std::string key = MakeKey(host, service, hints);
    const auto now = std::chrono::steady_clock::now();
    std::shared_ptr<Impl::PendingLookup> pending;

    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!bypass_cache) {
            auto it = impl_->cache.find(key);
            if (it != impl_->cache.end() && it->second.expiry > now) {
                return it->second.result;
            }
        }
        if (!bypass_cache) {
            auto inf = impl_->inflight.find(key);
            if (inf != impl_->inflight.end()) {
                pending = inf->second;  // reuse an already-running lookup
            }
        }
    }

    if (pending) {
        // A lookup for this key is already in flight; wait on it instead of
        // spawning another resolver thread.
        std::unique_lock<std::mutex> lock(pending->mu);
        if (!pending->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [&] { return pending->done; })) {
            LookupResult r;
            r.ok = false;
            r.error = "resolution timed out after " +
                      std::to_string(timeout_ms) + "ms";
            return r;
        }
        return pending->result;
    }

    // Start a background lookup. The shared Impl keeps the worker's cache
    // references alive even if this resolver instance is destroyed while the
    // lookup is still in flight.
    pending = std::make_shared<Impl::PendingLookup>();
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->inflight[key] = pending;
    }

    std::string h = host;
    std::string s = service;
    std::shared_ptr<Impl> impl = impl_;
    std::thread worker([impl, pending, key, h, s, hints]() {
        struct addrinfo *raw = nullptr;
        int rc = getaddrinfo(h.c_str(), s.empty() ? nullptr : s.c_str(),
                             &hints, &raw);
        LookupResult result;
        if (rc == 0 && raw) {
            result.ok = true;
            result.addrs = DeepCopyChain(raw);
            if (raw->ai_canonname) result.canonname = raw->ai_canonname;
            freeaddrinfo(raw);
        } else {
            result.ok = false;
#ifdef _WIN32
            result.error = "resolution failed";
#else
            result.error = (rc == 0) ? "no addresses" : gai_strerror(rc);
#endif
        }

        {
            std::lock_guard<std::mutex> lock(pending->mu);
            pending->result = std::move(result);
            pending->done = true;
        }
        pending->cv.notify_all();

        // Back-fill the cache so the next cycle is served instantly. Failures
        // are intentionally not cached so a transient DNS outage recovers
        // automatically.
        std::lock_guard<std::mutex> lock(impl->mu);
        auto inf = impl->inflight.find(key);
        if (inf != impl->inflight.end() &&
            inf->second.get() == pending.get()) {
            impl->inflight.erase(inf);
        }
        if (pending->result.ok) {
            auto cache_it = impl->cache.find(key);
            auto now2 = std::chrono::steady_clock::now();
            if (cache_it == impl->cache.end() || cache_it->second.expiry <= now2) {
                Impl::CacheEntry e;
                e.result = pending->result;  // shares the same chain
                e.expiry = now2 + std::chrono::milliseconds(impl->ttl_ms);
                impl->cache[key] = std::move(e);
            }
        }
    });
    worker.detach();

    // Wait for the background lookup with a hard deadline.
    std::unique_lock<std::mutex> lock(pending->mu);
    if (!pending->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [&] { return pending->done; })) {
        LookupResult r;
        r.ok = false;
        r.error = "resolution timed out after " +
                  std::to_string(timeout_ms) + "ms";
        return r;
    }
    return pending->result;
}

DnsResolver &GlobalResolver() {
    static DnsResolver resolver;
    return resolver;
}

}  // namespace pudimagent

