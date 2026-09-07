#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pudimcollector {

// One staged agent binary served to the dashboard.
struct AgentPlatform {
    std::string id;        // e.g. "linux-amd64"
    std::string os;        // "linux" | "windows" | ...
    std::string arch;      // "amd64" | "arm64" | ...
    std::string arch_label;// human-readable, e.g. "x86_64" / "aarch64"
    std::string filename;  // e.g. "pudim-agent-linux-amd64"
    std::string sha256;    // hex digest of the staged binary
    uint64_t size_bytes = 0;
};

// Scans a directory of staged pudim-agent binaries and serves them through the
// collector HTTP API. Expected layout
//
//   pudim-agent-linux-amd64
//   pudim-agent-linux-arm64
//   pudim-agent-windows-amd64.exe
//   version.txt            optional version override
class AgentDist {
public:
    // Returns false when the directory is empty or unreadable.
    bool Scan(const std::string &dir);

    bool Has(const std::string &platform_id) const { return Find(platform_id) != nullptr; }

    const std::vector<AgentPlatform> &Platforms() const { return platforms_; }
    const AgentPlatform *Find(const std::string &platform_id) const;
    const std::string &Version() const { return version_; }

    // Reads the staged binary bytes for a platform. Returns false when the
    // platform is unknown or the file cannot be read.
    bool LoadBinary(const std::string &platform_id, std::vector<char> &out) const;

    // JSON manifest with the version and the platform list.
    std::string ManifestJson() const;

private:
    std::string dir_;
    std::string version_;
    std::vector<AgentPlatform> platforms_;
};

}  // namespace pudimcollector
