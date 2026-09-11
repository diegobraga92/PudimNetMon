#include "platform.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wchar.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(__linux__) && !defined(_WIN32)
#include <sys/random.h>
#endif

namespace pudimagent::platform {

int64_t MonotonicUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool RandomBytes(unsigned char *out, size_t n) {
    if (!out || n == 0) return n == 0;
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
#if defined(__linux__) && !defined(_WIN32)
    ssize_t n_read = getrandom(out, n, 0);
    if (n_read > 0 && static_cast<size_t>(n_read) == n) return true;
#endif
    FILE *f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t got = std::fread(out, 1, n, f);
    std::fclose(f);
    return got == n;
#endif
}

#ifdef _WIN32
std::string WideToUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                  static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          &w[0], n);
    return w;
}
#endif

std::string DefaultStateDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wcscpy_s(buf, MAX_PATH, L"C:\\ProgramData");
    }
    std::wstring dir = std::wstring(buf) + L"\\PudimNetMon";
    CreateDirectoryW(dir.c_str(), nullptr);
    return WideToUtf8(dir);
#else
    return "/var/lib/pudim";
#endif
}

std::string TempDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return ".";
    return WideToUtf8(std::wstring(buf));
#else
    return "/tmp";
#endif
}

std::string AdvertisedDiagnosticEndpoint(const std::string &collector_endpoint,
                                         const std::string &diagnostic_port) {
    if (collector_endpoint.empty() || diagnostic_port.empty()) return "";

    // Accepts "host:port", "host" and "[v6]:port".
    std::string host = collector_endpoint;
    if (host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string::npos) return "";
        host = host.substr(1, close - 1);
    } else {
        const auto sep = host.rfind(':');
        if (sep != std::string::npos) host = host.substr(0, sep);
    }
    if (host.empty()) return "";

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *addrs = nullptr;
    if (::getaddrinfo(host.c_str(), "9", &hints, &addrs) != 0 || addrs == nullptr) {
        return "";
    }

    std::string out;
    for (struct addrinfo *ai = addrs; ai != nullptr && out.empty();
         ai = ai->ai_next) {
#ifdef _WIN32
        SOCKET fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
#else
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
#endif
        // Connecting a UDP socket sends no packets
#ifdef _WIN32
        const int rc =
            ::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
#else
        const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
#endif
        if (rc == 0) {
            struct sockaddr_storage local {};
#ifdef _WIN32
            int len = sizeof(local);
#else
            socklen_t len = sizeof(local);
#endif
            if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&local),
                              &len) == 0) {
                char buf[INET6_ADDRSTRLEN] = {0};
                if (local.ss_family == AF_INET6) {
                    auto *v6 = reinterpret_cast<struct sockaddr_in6 *>(&local);
                    if (::inet_ntop(AF_INET6, &v6->sin6_addr, buf,
                                    sizeof(buf)) != nullptr) {
                        out = "[" + std::string(buf) + "]:" + diagnostic_port;
                    }
                } else if (local.ss_family == AF_INET) {
                    auto *v4 = reinterpret_cast<struct sockaddr_in *>(&local);
                    if (::inet_ntop(AF_INET, &v4->sin_addr, buf,
                                    sizeof(buf)) != nullptr) {
                        out = std::string(buf) + ":" + diagnostic_port;
                    }
                }
            }
        }
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }
    ::freeaddrinfo(addrs);
    return out;
}

std::string LastErrorString() {
#ifdef _WIN32
    DWORD err = GetLastError();
    if (err == 0) return "no error";
    wchar_t *msg = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string out;
    if (n > 0 && msg) {
        out = WideToUtf8(std::wstring(msg, n));
        while (!out.empty() &&
               (out.back() == '\r' || out.back() == '\n' || out.back() == '.')) {
            out.pop_back();
        }
        LocalFree(msg);
    } else {
        out = "error " + std::to_string(err);
    }
    return out;
#else
    return std::string(std::strerror(errno));
#endif
}

} // namespace pudimagent::platform
