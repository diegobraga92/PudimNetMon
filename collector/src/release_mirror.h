#pragma once

#include <string>
#include <vector>

namespace pudimcollector {

// Downloads the "latest" GitHub release's single-file installer assets into the
// collector's staged installer dir, so the dashboard offers whatever the repo
// last published without manual staging.
struct ReleaseMirrorConfig {
    std::string owner;       // e.g. "diegobraga92"
    std::string repo;        // e.g. "PudimNetMon"
    std::string api_base = "https://api.github.com";
    std::string token;       // optional to raise API rate limits / private repos
    std::string installer_dir;   // where .run / .exe artifacts are staged
};

class ReleaseMirror {
public:
    ReleaseMirror(ReleaseMirrorConfig config);
    ~ReleaseMirror() = default;

    const ReleaseMirrorConfig &Config() const { return config_; }

    bool SyncOnce(std::string &summary);

    static std::vector<std::string> StableAssetNames();

private:
    bool HttpGet(const std::string &url, const std::string &accept,
                 std::string &out, std::string &err) const;

    bool DownloadToFile(const std::string &url, const std::string &dest,
                        std::string &err) const;

    ReleaseMirrorConfig config_;
};

}  // namespace pudimcollector
