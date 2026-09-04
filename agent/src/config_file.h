#pragma once

#include <cctype>
#include <fstream>
#include <map>
#include <string>

namespace config {

inline bool Exists(const std::string &path) {
    std::ifstream in(path);
    return in.good();
}

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
