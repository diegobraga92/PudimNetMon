#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pudimagent::platform {

int64_t MonotonicUs();
int64_t NowMs();

bool RandomBytes(unsigned char *out, size_t n);

std::string DefaultStateDir();
std::string TempDir();

std::string LastErrorString();

#ifdef _WIN32
std::string WideToUtf8(const std::wstring &w);
std::wstring Utf8ToWide(const std::string &s);
#endif

} // namespace pudimagent::platform
