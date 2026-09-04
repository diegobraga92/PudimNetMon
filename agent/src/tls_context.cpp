#include "tls_context.h"

#include <map>
#include <mutex>

#include <openssl/ssl.h>

namespace pudimagent {

struct ssl_ctx_st *SharedSslCtx() {
    static std::once_flag once;
    static SSL_CTX *ctx = nullptr;
    std::call_once(once, []() {
        SSL_CTX *c = SSL_CTX_new(TLS_client_method());
        if (c) {
            SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
            SSL_CTX_set_default_verify_paths(c);
            // Session caching is managed manually per-host (TlsSessionCache);
            // keep the context from accumulating unbounded internal state.
            SSL_CTX_set_session_cache_mode(
                c, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        }
        ctx = c;
    });
    return ctx;
}

struct TlsSessionCache::Impl {
    std::mutex mu;
    std::map<std::string, SSL_SESSION *> sessions;

    ~Impl() {
        for (auto &kv : sessions) SSL_SESSION_free(kv.second);
        sessions.clear();
    }
};

TlsSessionCache::TlsSessionCache() : impl_(std::make_unique<Impl>()) {}
TlsSessionCache::~TlsSessionCache() = default;

struct ssl_session_st *TlsSessionCache::Get(const std::string &host_port) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->sessions.find(host_port);
    if (it == impl_->sessions.end()) return nullptr;
    
    SSL_SESSION_up_ref(it->second);
    return it->second;
}

void TlsSessionCache::Put(const std::string &host_port,
                          struct ssl_session_st *session) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->sessions.find(host_port);
    if (it != impl_->sessions.end()) {
        SSL_SESSION_free(it->second);
        it->second = session;
    } else {
        impl_->sessions.emplace(host_port, session);
    }
}

TlsSessionCache &GlobalSessionCache() {
    static TlsSessionCache cache;
    return cache;
}

}  // namespace pudimagent
