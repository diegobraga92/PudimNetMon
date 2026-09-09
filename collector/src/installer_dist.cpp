#include "installer_dist.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace pudimcollector {

namespace {

std::string ArchLabel(const std::string &arch) {
    if (arch == "amd64") return "x86_64";
    if (arch == "arm64") return "aarch64";
    return arch;
}

bool HasSuffix(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool HasPrefix(const std::string &s, const std::string &prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

// Parses a staged installer artifact filename into an InstallerArtifact.
bool ParseInstallerFilename(const std::filesystem::path &path,
                            InstallerArtifact &out) {
    std::string name = path.filename().string();

    const std::string kWinPrefix = "PudimNetMon-Agent-Setup";
    if (HasPrefix(name, kWinPrefix) && HasSuffix(name, ".exe")) {
        out.id = "windows-amd64";
        out.os = "windows";
        out.arch = "amd64";
        out.arch_label = "x86_64";
        out.kind = "setup";
        out.extension = ".exe";
        out.filename = name;
        return true;
    }

    const std::string kLinPrefix = "pudimnetmon-agent-";
    if (!HasPrefix(name, kLinPrefix)) return false;

    const std::string kMarker = "-linux-";
    auto marker = name.find(kMarker);
    if (marker == std::string::npos) return false;

    std::string extension;
    std::string kind;
    if (HasSuffix(name, ".run")) {
        extension = ".run";
        kind = "run";
    } else {
        return false;  // *.sha256, *.tar.gz and other artifacts are not installers.
    }

    // arch = the token between the "-linux-" marker and the extension.
    std::string arch = name.substr(marker + kMarker.size(),
                                   name.size() - marker - kMarker.size() -
                                       extension.size());
    if (arch.empty() || arch.find('-') != std::string::npos) return false;

    out.id = "linux-" + arch;
    out.os = "linux";
    out.arch = arch;
    out.arch_label = ArchLabel(arch);
    out.kind = kind;
    out.extension = extension;
    out.filename = name;
    return true;
}

}  // namespace

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

bool ValidConfigToken(const std::string &token) {
    if (token.empty() || token.size() > 256) return false;
    for (unsigned char c : token) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9');
        if (!alnum && c != '-' && c != '_') return false;
    }
    return true;
}

bool InstallerDist::Scan(const std::string &dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    artifacts_.clear();
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
        InstallerArtifact artifact;
        if (!ParseInstallerFilename(entry.path(), artifact)) continue;
        // One artifact per platform (e.g. keep the first staged Windows setup).
        if (FindUnlocked(artifact.id)) continue;
        std::vector<char> bytes;
        if (!ReadFile(entry.path().string(), bytes)) continue;
        artifact.size_bytes = static_cast<uint64_t>(bytes.size());
        artifact.sha256 = Sha256Hex(bytes);
        artifacts_.push_back(std::move(artifact));
    }
    return true;
}

const InstallerArtifact *InstallerDist::FindUnlocked(
    const std::string &platform_id) const {
    for (const auto &a : artifacts_) {
        if (a.id == platform_id) return &a;
    }
    return nullptr;
}

bool InstallerDist::Find(const std::string &platform_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return FindUnlocked(platform_id) != nullptr;
}

std::vector<InstallerArtifact> InstallerDist::Artifacts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return artifacts_;
}

std::string InstallerDist::Version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

bool InstallerDist::Load(const std::string &platform_id,
                         std::vector<char> &out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const InstallerArtifact *a = FindUnlocked(platform_id);
    if (!a) return false;
    return ReadFile(dir_ + "/" + a->filename, out);
}

std::string InstallerDist::ManifestJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json j;
    j["version"] = version_;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &a : artifacts_) {
        nlohmann::json item;
        item["id"] = a.id;
        item["os"] = a.os;
        item["arch"] = a.arch_label;
        item["kind"] = a.kind;
        item["filename"] = a.filename;
        item["size_bytes"] = a.size_bytes;
        item["sha256"] = a.sha256;
        item["download_url"] = "/api/installers/download?platform=" + a.id;
        arr.push_back(std::move(item));
    }
    j["installers"] = arr;
    return j.dump();
}

std::string InstallerDist::DownloadName(const std::string &platform_id,
                                        const std::string &config) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const InstallerArtifact *a = FindUnlocked(platform_id);
    if (!a) return "";
    if (!ValidConfigToken(config)) return a->filename;

    std::string stem = a->filename.substr(0, a->filename.size() - a->extension.size());
    return stem + "-cfg-" + config + a->extension;
}

}  // namespace pudimcollector

