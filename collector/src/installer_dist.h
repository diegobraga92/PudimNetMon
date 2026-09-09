#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pudimcollector {

// One staged installer artifact served to the dashboard.
struct InstallerArtifact {
    std::string id;          // e.g. "windows-amd64" | "linux-amd64"
    std::string os;          // "windows" | "linux"
    std::string arch;        // "amd64" | "arm64" | ...
    std::string arch_label;  // human-readable, e.g. "x86_64" / "aarch64"
    std::string kind;        // "setup" | "run" | "archive"
    std::string filename;    // canonical staged name, e.g. "PudimNetMon-Agent-Setup-0.1.0.exe"
    std::string extension;   // ".exe" | ".run" | ".tar.gz"
    std::string sha256;      // hex digest of the staged artifact
    uint64_t size_bytes = 0;
};

// Scans a directory of staged installer artifacts and serves them through the
// collector HTTP API. Recognized layouts:
//
//   PudimNetMon-Agent-Setup-<version>.exe          windows setup (amd64)
//   PudimNetMon-Agent-Setup-latest.exe             windows setup (release mirror)
//   pudimnetmon-agent-install-<v>-linux-<arch>.run  linux single-file installer
//   pudimnetmon-agent-install-linux-<arch>.run      linux single-file (release mirror)
//   version.txt            optional version override
class InstallerDist {
public:
    bool Scan(const std::string &dir);

    std::vector<InstallerArtifact> Artifacts() const;
    bool Has(const std::string &platform_id) const { return Find(platform_id); }
    bool Find(const std::string &platform_id) const;
    std::string Version() const;

    bool Load(const std::string &platform_id, std::vector<char> &out) const;

    std::string ManifestJson() const;

    std::string DownloadName(const std::string &platform_id,
                             const std::string &config) const;

private:
    const InstallerArtifact *FindUnlocked(const std::string &platform_id) const;

    mutable std::mutex mutex_;
    std::string dir_;
    std::string version_;
    std::vector<InstallerArtifact> artifacts_;
};

}  // namespace pudimcollector

