#include "agent_dist.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace pudimcollector {

namespace {

// Detects whether a filename is a staged agent binary and fills the platform
// metadata (including reading size + sha256 from disk).
bool ParseAgentFilename(const std::filesystem::path &path,
                        AgentPlatform &out) {
    std::string name = path.filename().string();
    const std::string kPrefix = "pudim-agent-";
    if (name.compare(0, kPrefix.size(), kPrefix) != 0) return false;

    std::string rest = name.substr(kPrefix.size());
    const std::string kExe = ".exe";
    if (rest.size() >= kExe.size() &&
        rest.compare(rest.size() - kExe.size(), kExe.size(), kExe) == 0) {
        rest = rest.substr(0, rest.size() - kExe.size());
    }

    // Split "<os>-<arch>" at the last '-'.
    auto dash = rest.find_last_of('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= rest.size()) {
        return false;
    }
    std::string os = rest.substr(0, dash);
    std::string arch = rest.substr(dash + 1);
    // Guard against platforms we don't recognize (ignore anything else).
    static const char *kKnownOs[] = {"linux", "windows", "darwin", "freebsd"};
    if (std::find(std::begin(kKnownOs), std::end(kKnownOs), os) ==
        std::end(kKnownOs)) {
        return false;
    }

    out.id = os + "-" + arch;
    out.os = os;
    out.arch = arch;
    if (arch == "amd64") {
        out.arch_label = "x86_64";
    } else if (arch == "arm64") {
        out.arch_label = "aarch64";
    } else {
        out.arch_label = arch;
    }
    out.filename = name;
    return true;
}

std::string Sha256Hex(const std::vector<char> &data) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    EVP_MD_CTX_free(ctx);
    std::string hex;
    hex.reserve(len * 2);
    static const char kHex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < len; ++i) {
        hex += kHex[(digest[i] >> 4) & 0x0f];
        hex += kHex[digest[i] & 0x0f];
    }
    return hex;
}

bool ReadFile(const std::string &path, std::vector<char> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return in.good() || in.eof();
}

std::string Trim(const std::string &s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(s.begin(), s.end(), not_space);
    auto last = std::find_if(s.rbegin(), s.rend(), not_space);
    if (first == s.end()) return "";
    return std::string(first, last.base());
}

}  // namespace

bool AgentDist::Scan(const std::string &dir) {
    platforms_.clear();
    version_ = "0.1.0";
    if (dir.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    dir_ = dir;

    // Optional version file.
    std::vector<char> ver_buf;
    if (ReadFile(dir + "/version.txt", ver_buf) && !ver_buf.empty()) {
        std::string v(ver_buf.begin(), ver_buf.end());
        v = Trim(v);
        if (!v.empty()) version_ = v;
    }

    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        AgentPlatform plat;
        if (!ParseAgentFilename(entry.path(), plat)) continue;
        std::vector<char> bytes;
        if (!ReadFile(entry.path().string(), bytes)) continue;
        plat.size_bytes = static_cast<uint64_t>(bytes.size());
        plat.sha256 = Sha256Hex(bytes);
        platforms_.push_back(std::move(plat));
    }
    return true;
}

const AgentPlatform *AgentDist::Find(const std::string &platform_id) const {
    for (const auto &p : platforms_) {
        if (p.id == platform_id) return &p;
    }
    return nullptr;
}

bool AgentDist::LoadBinary(const std::string &platform_id,
                           std::vector<char> &out) const {
    const AgentPlatform *p = Find(platform_id);
    if (!p) return false;
    return ReadFile(dir_ + "/" + p->filename, out);
}

std::string AgentDist::ManifestJson() const {
    nlohmann::json j;
    j["version"] = version_;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &p : platforms_) {
        nlohmann::json item;
        item["id"] = p.id;
        item["os"] = p.os;
        item["arch"] = p.arch_label;
        item["filename"] = p.filename;
        item["size_bytes"] = p.size_bytes;
        item["sha256"] = p.sha256;
        item["download_url"] = "/api/agent/download?platform=" + p.id;
        arr.push_back(std::move(item));
    }
    j["platforms"] = arr;
    return j.dump();
}

}  // namespace pudimcollector
