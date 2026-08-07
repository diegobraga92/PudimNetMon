#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pudimagent {

// Persistent, bounded on-disk buffer for metric batches (Phase 7 DR). Batches
// are serialized protobuf blobs stored in a local SQLite database so metrics
// survive extended collector downtime and agent restarts. Built without
// libsqlite3, the buffer is disabled and reports Available()==false.
class DiskBuffer {
public:
    explicit DiskBuffer(std::string db_path, uint64_t max_bytes = 100ULL * 1024 * 1024);
    ~DiskBuffer();

    DiskBuffer(const DiskBuffer &) = delete;
    DiskBuffer &operator=(const DiskBuffer &) = delete;

    // Opens/creates the database. Returns false if SQLite is unavailable or
    // the database could not be opened.
    bool Open(std::string &error);

    // True when the buffer is backed by a working SQLite database.
    bool Available() const;

    // Serializes and enqueues a batch. Returns false if unavailable or full.
    bool Push(const std::string &payload_blob);

    // Fetches up to `limit` oldest payloads (FIFO) into `out`.
    void Peek(std::vector<std::string> &out, size_t limit);

    // Removes the `count` oldest rows (after a successful send).
    void Pop(size_t count);

    // Number of pending payloads.
    uint64_t Size() const;

    // Drops oldest rows until under the size cap (returns dropped count).
    uint64_t Trim();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_db_path;
    uint64_t m_max_bytes;
};

} // namespace pudimagent
