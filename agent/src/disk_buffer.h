#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pudimagent {

class DiskBuffer {
public:
    explicit DiskBuffer(std::string db_path, uint64_t max_bytes = 100ULL * 1024 * 1024);
    ~DiskBuffer();

    DiskBuffer(const DiskBuffer &) = delete;
    DiskBuffer &operator=(const DiskBuffer &) = delete;

    bool Open(std::string &error);
    bool Available() const;

    bool Push(const std::string &payload_blob);
    void Peek(std::vector<std::string> &out, size_t limit);
    void Pop(size_t count);

    uint64_t Size() const;
    uint64_t Trim();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_db_path;
    uint64_t m_max_bytes;
};

} // namespace pudimagent
