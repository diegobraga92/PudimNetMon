#pragma once

#include <memory>
#include <string>

struct ssl_ctx_st;    // SSL_CTX
struct ssl_session_st;  // SSL_SESSION

namespace pudimagent {

struct ssl_ctx_st *SharedSslCtx();

class TlsSessionCache {
public:
    TlsSessionCache();
    ~TlsSessionCache();

    TlsSessionCache(const TlsSessionCache &) = delete;
    TlsSessionCache &operator=(const TlsSessionCache &) = delete;

    struct ssl_session_st *Get(const std::string &host_port);

    void Put(const std::string &host_port, struct ssl_session_st *session);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

TlsSessionCache &GlobalSessionCache();

}  // namespace pudimagent
