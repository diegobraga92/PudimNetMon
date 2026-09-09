#include "release_mirror.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "logging.h"

namespace pudimcollector {

ReleaseMirror::ReleaseMirror(ReleaseMirrorConfig config)
    : config_(std::move(config)) {
    // libcurl is safe to init once per process from multiple threads.
    static std::once_flag curl_init;
    std::call_once(curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

namespace {

// GitHub requires a User-Agent header on API calls.
const char *kUserAgent = "pudimnetmon-collector/0.1.0";

size_t WriteToString(void *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

size_t WriteToFile(void *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *f = static_cast<std::FILE *>(userdata);
    return std::fwrite(ptr, size, nmemb, f);
}

}  // namespace

bool ReleaseMirror::HttpGet(const std::string &url, const std::string &accept,
                            std::string &out, std::string &err) const {
    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "curl init failed";
        return false;
    }
    out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Accept: " + accept).c_str());
    if (!config_.token.empty()) {
        headers = curl_slist_append(
            headers, ("Authorization: Bearer " + config_.token).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        return false;
    }
    if (http_code < 200 || http_code >= 300) {
        err = "HTTP " + std::to_string(http_code);
        return false;
    }
    return true;
}
bool ReleaseMirror::DownloadToFile(const std::string &url,
                                   const std::string &dest,
                                   std::string &err) const {
    std::string tmp = dest + ".part";
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "cannot open " + tmp;
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(f);
        std::remove(tmp.c_str());
        err = "curl init failed";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);

    struct curl_slist *headers = nullptr;
    if (!config_.token.empty()) {
        headers = curl_slist_append(
            headers, ("Authorization: Bearer " + config_.token).c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    std::fclose(f);

    if (rc != CURLE_OK || http_code < 200 || http_code >= 300) {
        std::remove(tmp.c_str());
        err = rc != CURLE_OK ? curl_easy_strerror(rc)
                             : "HTTP " + std::to_string(http_code);
        return false;
    }
    // Atomic replace so a concurrent HTTP download never reads a partial file.
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        std::remove(tmp.c_str());
        err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

std::vector<std::string> ReleaseMirror::StableAssetNames() {
    return {
        // Linux single-file installers (matching InstallerDist parsing).
        "pudimnetmon-agent-install-linux-amd64.run",
        "pudimnetmon-agent-install-linux-arm64.run",
        // Windows setup EXE.
        "PudimNetMon-Agent-Setup.exe",
    };
}

bool ReleaseMirror::SyncOnce(std::string &summary) {
    const std::string api_url = config_.api_base + "/repos/" + config_.owner +
                                "/" + config_.repo + "/releases/latest";
    std::string body, err;
    if (!HttpGet(api_url, "application/vnd.github+json", body, err)) {
        logger::emit("info", "Latest GitHub release fetch failed (" + err +
                                 "); keeping staged installers");
        summary = "release fetch failed: " + err;
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const std::exception &e) {
        logger::emit("error", "Failed to parse GitHub release JSON: " +
                                  std::string(e.what()));
        summary = "release JSON parse failed";
        return false;
    }

    std::string version = j.value("tag_name", std::string(""));
    if (version.size() > 1 && version[0] == 'v') version = version.substr(1);
    if (version.empty()) version = "0.1.0";

    // Collect browser_download_url by asset name from the release JSON.
    std::vector<std::string> wanted = StableAssetNames();
    int downloaded = 0;
    for (const auto &asset : j.value("assets", nlohmann::json::array())) {
        std::string name = asset.value("name", std::string());
        if (name.empty()) continue;
        bool is_wanted = false;
        for (const auto &w : wanted) {
            if (w == name) {
                is_wanted = true;
                break;
            }
        }
        if (!is_wanted) continue;

        std::string url = asset.value("browser_download_url", std::string());
        if (url.empty()) continue;

        std::error_code ec;
        if (config_.installer_dir.empty() ||
            !std::filesystem::is_directory(config_.installer_dir, ec)) {
            continue;
        }
        std::string dest = config_.installer_dir + "/" + name;
        std::string derr;
        if (DownloadToFile(url, dest, derr)) {
            ++downloaded;
        } else {
            logger::emit("warn", "Failed to mirror " + name + ": " + derr);
        }
    }

    // version.txt overrides the default "0.1.0" in the staged dir.
    if (!config_.installer_dir.empty()) {
        std::ofstream vf(config_.installer_dir + "/version.txt");
        vf << version << "\n";
    }

    summary = "mirrored " + std::to_string(downloaded) +
              " installer asset(s) from release " + version;
    logger::emit("info", summary);
    return downloaded > 0;
}

}  // namespace pudimcollector

