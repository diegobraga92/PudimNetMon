#pragma once

#include <memory>
#include <string>

struct ssl_ctx_st;    // SSL_CTX
struct ssl_session_st;  // SSL_SESSION

namespace pudimagent {

// Returns the process-wide client SSL_CTX (created once, thread-safe for
// SSL_new afterwards). It enables TLS >= 1.2 and loads the system default CA
// store so certificate verification works; callers that only measure handshake
// latency may override per-SSL verify mode with SSL_set_verify().
struct ssl_ctx_st *SharedSslCtx();

// Small per-host TLS session cache so repeated handshakes to the same target
// can use abbreviated (session-resumed) handshakes, avoiding a full key
// exchange every probe cycle. Thread-safe.
class TlsSessionCache {
public:
    TlsSessionCache();
    ~TlsSessionCache();

    TlsSessionCache(const TlsSessionCache &) = delete;
    TlsSessionCache &operator=(const TlsSessionCache &) = delete;

    // Returns a session previously stored for `host_port` or nullptr if none
    // is cached. The returned reference is owned by the caller and must be
    // released with SSL_SESSION_free() (a concurrent Put() cannot free it out
    // from under the caller).
    struct ssl_session_st *Get(const std::string &host_port);

    // Stores `session`, taking ownership of one reference. Any previous
    // session for the same key is released.
    void Put(const std::string &host_port, struct ssl_session_st *session);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Process-wide session cache, safe for concurrent use.
TlsSessionCache &GlobalSessionCache();

}  // namespace pudimagent
