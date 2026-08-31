#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "config_file.h"

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &name) {
    if (cond) {
        std::cout << "PASS " << name << "\n";
    } else {
        std::cerr << "FAIL " << name << "\n";
        ++g_failures;
    }
}

std::string WriteTempConfig(const std::string &content) {
    auto ns = std::chrono::high_resolution_clock::now()
                  .time_since_epoch()
                  .count();
    auto path = std::filesystem::temp_directory_path() /
                ("pudim_cfg_test_" + std::to_string(ns) + ".conf");
    std::ofstream out(path);
    out << content;
    out.close();
    return path.string();
}

} // namespace

int main() {
    // 1. Basic parse: comments, blank lines, whitespace trimming, comma lists.
    {
        std::string path = WriteTempConfig(
            "# comment\n"
            "collector-endpoints=collector.lan:50051,collector2.lan:50052\n"
            "\n"
            "  node-id  =  web-01  \n"
            "; semicolon comment\n"
            "interval=10000\n");
        std::map<std::string, std::string> cfg;
        std::string err;
        Check(config::LoadConfigFile(path, cfg, err), "load basic");
        Check(cfg.size() == 3, "basic count");
        Check(cfg["collector-endpoints"] ==
                  "collector.lan:50051,collector2.lan:50052",
              "basic comma list preserved");
        Check(cfg["node-id"] == "web-01", "whitespace trimmed");
        Check(cfg["interval"] == "10000", "basic int as string");
        std::filesystem::remove(path);
    }

    // 2. Value containing '=' must split on the first '=' only.
    {
        std::string path = WriteTempConfig(
            "dns-expected=example.com=A:1.2.3.4\n");
        std::map<std::string, std::string> cfg;
        std::string err;
        Check(config::LoadConfigFile(path, cfg, err), "load eq-in-value");
        Check(cfg["dns-expected"] == "example.com=A:1.2.3.4",
              "eq-in-value preserved");
        std::filesystem::remove(path);
    }

    // 3. CRLF line endings (Windows-authored config files).
    {
        std::string path = WriteTempConfig("node-id=win-01\r\ninterval=5000\r\n");
        std::map<std::string, std::string> cfg;
        std::string err;
        Check(config::LoadConfigFile(path, cfg, err), "load crlf");
        Check(cfg["node-id"] == "win-01", "crlf node-id");
        Check(cfg["interval"] == "5000", "crlf interval");
        std::filesystem::remove(path);
    }

    // 4. Duplicate keys: last occurrence wins.
    {
        std::string path = WriteTempConfig("node-id=one\nnode-id=two\n");
        std::map<std::string, std::string> cfg;
        std::string err;
        Check(config::LoadConfigFile(path, cfg, err), "load duplicate");
        Check(cfg["node-id"] == "two", "duplicate last-wins");
        std::filesystem::remove(path);
    }

    // 5. Malformed line (no '=') is rejected with a line number.
    {
        std::string path = WriteTempConfig(
            "node-id=ok\nthis line has no equals\n");
        std::map<std::string, std::string> cfg;
        std::string err;
        Check(!config::LoadConfigFile(path, cfg, err), "reject malformed line");
        Check(err.find("line 2") != std::string::npos, "malformed line number");
        std::filesystem::remove(path);
    }

    // 6. Missing file is rejected with a non-empty error.
    {
        std::map<std::string, std::string> cfg;
        std::string err;
        Check(!config::LoadConfigFile("/nonexistent/pudim/does_not_exist.conf",
                                      cfg, err),
              "reject missing file");
        Check(!err.empty(), "missing file error set");
    }

    // 7. Exists() helper (used for the optional default config path).
    {
        std::string path = WriteTempConfig("node-id=x\n");
        Check(config::Exists(path), "exists true");
        Check(!config::Exists(path + ".nope"), "exists false");
        std::filesystem::remove(path);
    }

    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "All config tests passed\n";
    return 0;
}
