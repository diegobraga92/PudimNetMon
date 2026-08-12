#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pudimagent::platform {

// Monotonic clock in microseconds (portable CLOCK_MONOTONIC).
int64_t MonotonicUs();

// Wall-clock now in milliseconds since the Unix epoch.
int64_t NowMs();

// Fills `n` cryptographically-random bytes. Returns false on failure.
bool RandomBytes(unsigned char *out, size_t n);

// Directory for agent state (ProgramData\PudimNetMon on Windows;
// /var/lib/pudim on Linux). On Windows the directory is created if needed.
std::string DefaultStateDir();

// System temporary directory ("/tmp" on POSIX).
std::string TempDir();

// Human-readable string for the last OS error (errno / GetLastError).
std::string LastErrorString();

#ifdef _WIN32
// Wide<->UTF-8 conversions (Windows only; no-ops elsewhere).
std::string WideToUtf8(const std::wstring &w);
std::wstring Utf8ToWide(const std::string &s);
#endif

} // namespace pudimagent::platform
