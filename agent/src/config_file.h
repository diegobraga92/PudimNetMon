#pragma once

#include <cctype>
#include <fstream>
#include <map>
#include <string>

// Layered-config support for the agent: a flat key=value file whose keys match
// the long CLI option names (e.g. `collector-endpoints=...`). Precedence is:
// built-in defaults < config file < CLI flags.
//
// Format (see agent/config/agent.conf.example):
//   # comment lines start with '#' or ';'
//   key = value           (whitespace around key/value is trimmed)
//   list=one,two,three    (values are comma-separated, like the CLI)
// The value may itself contain '='; the line is split on the first '='.
// CRLF line endings are tolerated. Unknown keys are not rejected here — the
// caller validates them against its option table.
namespace config {

// True when a file exists at `path` (used to make the default config path
// optional: silently skipped when absent).
inline bool Exists(const std::string &path) {
    std::ifstream in(path);
    return in.good();
}

// Parses `path` into `out`. Returns false and sets `error` if the file cannot
// be opened or a line is malformed. Duplicate keys: last occurrence wins.
inline bool LoadConfigFile(const std::string &path,
                           std::map<std::string, std::string> &out,
                           std::string &error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        error = "cannot open file";
        return false;
    }

    auto trim = [](std::string s) {
        size_t b = 0;
        size_t e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    };

    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;

        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            error = "line " + std::to_string(line_no) +
                    ": expected key=value, got '" + t + "'";
            return false;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key.empty()) {
            error = "line " + std::to_string(line_no) + ": empty key";
            return false;
        }
        out[key] = val;
    }
    return true;
}

} // namespace config
