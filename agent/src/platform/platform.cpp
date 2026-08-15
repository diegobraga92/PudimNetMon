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
#include <windows.h>
#include <bcrypt.h>
#include <wchar.h>
#else
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
    // n is tiny (8 or 16 bytes) so a direct ULONG cast is safe.
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
#if defined(__linux__) && !defined(_WIN32)
    // getrandom() is a single syscall with no file-descriptor or buffering
    // cost; fall back to /dev/urandom on other POSIX platforms.
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
