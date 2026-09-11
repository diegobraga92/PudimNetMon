#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "commands.h"
#include "logger.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pudimagent {

namespace {

using pudimnetmon::CommandResponse;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ReadAll(FILE *fp) {
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    return out;
}

// Creates `path` (including parents) when missing. Returns true only when the
// directory exists and the agent can write to it.
bool MakeDirs(const std::string &path) {
    if (path.empty()) return false;
#ifdef _WIN32
    const std::wstring w = pudimagent::platform::Utf8ToWide(path);
    if (w.empty()) return false;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] != L'\\' && w[i] != L'/') continue;
        CreateDirectoryW(w.substr(0, i).c_str(), nullptr);
    }
    if (!CreateDirectoryW(w.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    return true;
#else
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        if (::mkdir(path.substr(0, i).c_str(), 0700) != 0 && errno != EEXIST) {
            return false;
        }
    }
    // The temporary fallback has a predictable name, so never hand a directory
    // that somebody else owns (or that others can write to) to a child process.
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    if (st.st_uid != ::geteuid()) return false;
    return (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
#endif
}

// Directory handed to external tools as $HOME.
const std::string &ToolHomeDirImpl() {
    static const std::string resolved = [] {
        const std::vector<std::string> candidates = {
#ifdef _WIN32
            pudimagent::platform::DefaultStateDir() + "\\tool-home",
            pudimagent::platform::TempDir() + "pudim-tool-home",
#else
            pudimagent::platform::DefaultStateDir() + "/tool-home",
            pudimagent::platform::TempDir() + "/pudim-tool-home",
#endif
        };
        for (const auto &dir : candidates) {
            if (MakeDirs(dir)) return dir;
        }
        return std::string();
    }();
    return resolved;
}

// Shell prefix pointing external tools at a private, writable HOME.
std::string ToolHomePrefix() {
    const std::string &home = ToolHomeDirImpl();
    if (home.empty()) return std::string();
#ifdef _WIN32
    const std::string cfg = home + "\\.config";
    MakeDirs(cfg);
    return "set \"HOME=" + home + "\" && set \"XDG_CONFIG_HOME=" + cfg +
           "\" && ";
#else
    const std::string cfg = home + "/.config";
    MakeDirs(cfg);
    return "HOME='" + home + "' XDG_CONFIG_HOME='" + cfg + "' ";
#endif
}

// Renders a pclose() status as "exit 0", "killed by signal 6 (Aborted)", ...
std::string DescribeExitStatus(int status) {
    if (status < 0) return "no exit status";
#ifdef _WIN32
    return "exit " + std::to_string(status);
#else
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        const char *name = ::strsignal(sig);
        return "killed by signal " + std::to_string(sig) +
               (name != nullptr ? std::string(" (") + name + ")" : std::string());
    }
    if (WIFEXITED(status)) return "exit " + std::to_string(WEXITSTATUS(status));
    return "status " + std::to_string(status);
#endif
}

// Runs `cmd` through the shell with stdout and stderr merged.
std::string RunFixed(const std::string &cmd, int *exit_status = nullptr) {
    if (exit_status != nullptr) *exit_status = -1;
    const std::string full = ToolHomePrefix() + cmd;
#ifdef _WIN32
    FILE *fp = popen((full + " 2>&1").c_str(), "r");
#else
    FILE *fp = popen(("( " + full + " ) 2>&1").c_str(), "r");
#endif
    if (!fp) return "";
    std::string out = ReadAll(fp);
    const int status = pclose(fp);
    if (exit_status != nullptr) *exit_status = status;
    return out;
}

bool FileExists(const char *path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(path, &st) == 0;
#endif
}

std::string Hostname() {
#ifdef _WIN32
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = sizeof(buf);
    if (GetComputerNameA(buf, &n)) return std::string(buf, n);
    return "unknown";
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) return std::string(buf);
    return "unknown";
#endif
}

void AddField(CommandResponse *resp, const std::string &key,
              const std::string &value) {
    (*resp->mutable_fields())[key] = value;
}

std::string Trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string GetParam(const CommandParams &params, const std::string &key,
                     const std::string &def) {
    auto it = params.find(key);
    return (it == params.end() || it->second.empty()) ? def : it->second;
}

int64_t ParseIntParam(const CommandParams &params, const std::string &key,
                      int64_t def, int64_t lo, int64_t hi) {
    auto it = params.find(key);
    if (it == params.end() || it->second.empty()) return def;
    try {
        int64_t v = std::stoll(it->second);
        return v < lo ? lo : (v > hi ? hi : v);
    } catch (...) {
        return def;
    }
}

std::string FirstToken(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = b;
    while (e < s.size() && !std::isspace(static_cast<unsigned char>(s[e]))) ++e;
    return s.substr(b, e - b);
}

// Locates an executable on the agent.
std::string FindTool(const std::string &name,
                     const std::vector<std::string> &paths) {
    for (const auto &p : paths) {
        if (FileExists(p.c_str())) return p;
    }
#ifndef _WIN32
    std::string out = RunFixed("command -v " + name + " || true");
#else
    std::string out = RunFixed("where " + name);
#endif
    std::string first = FirstToken(Trim(out));
    return first.empty() ? std::string() : first;
}

// Extracts the JSON value that follows `"key"` inside `text` (first match).
std::string JsonValue(const std::string &text, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < text.size() && text[pos] != ':') ++pos;
    if (pos >= text.size()) return "";
    ++pos;
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
    if (pos >= text.size()) return "";
    if (text[pos] == '"') {
        std::string out;
        ++pos;
        while (pos < text.size() && text[pos] != '"') {
            if (text[pos] == '\\' && pos + 1 < text.size()) {
                out += text[pos + 1];
                pos += 2;
                continue;
            }
            out += text[pos++];
        }
        return out;
    }
    size_t start = pos;
    while (pos < text.size()) {
        char c = text[pos];
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
            c == '\n' || c == '\r') {
            break;
        }
        ++pos;
    }
    return text.substr(start, pos - start);
}

// Number value for `key`, or `def` when absent/unparseable.
double JsonNumber(const std::string &text, const std::string &key,
                  double def) {
    std::string v = JsonValue(text, key);
    if (v.empty()) return def;
    try {
        return std::stod(v);
    } catch (...) {
        return def;
    }
}

// Slice of `text` between two keys, inclusive of nothing else.
std::string JsonBlock(const std::string &text, const std::string &from,
                      const std::string &to) {
    const std::string a = "\"" + from + "\"";
    size_t start = text.find(a);
    if (start == std::string::npos) return "";
    size_t end = to.empty() ? text.size() : text.find("\"" + to + "\"", start + a.size());
    if (end == std::string::npos) end = text.size();
    return text.substr(start, end - start);
}

double BitsPerSecToMbps(double bps) { return bps / 1000000.0; }
double BytesPerSecToMbps(double bps) { return bps * 8.0 / 1000000.0; }

// Runs `cmd` wrapped in `timeout` when available (POSIX only) so a hung
// external tool cannot block the agent forever.
std::string RunBounded(const std::string &cmd, int timeout_s,
                       int *exit_status = nullptr) {
#ifndef _WIN32
    return RunFixed("timeout " + std::to_string(timeout_s) + " " + cmd,
                    exit_status);
#else
    (void)timeout_s;
    return RunFixed(cmd, exit_status);
#endif
}

void RunAgentInfo(const CommandParams &, CommandResponse *resp) {
    std::string os =
#ifdef _WIN32
        "windows";
#else
        "linux";
#endif
    std::string arch =
#if defined(__x86_64__) || defined(_M_X64)
        "x86_64";
#elif defined(__aarch64__)
        "aarch64";
#else
        "unknown";
#endif
    AddField(resp, "os", os);
    AddField(resp, "arch", arch);
    AddField(resp, "hostname", Hostname());
    resp->set_summary("agent on " + os + "/" + arch + " (" + Hostname() + ")");
    resp->set_success(true);
}

#ifndef _WIN32

std::string RootDisk() {
    FILE *fp = fopen("/proc/self/mounts", "r");
    if (!fp) return "";
    std::string dev;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        std::string s(line);
        auto i = s.find(' ');
        if (i == std::string::npos) continue;
        std::string device = s.substr(0, i);
        s = s.substr(i + 1);
        i = s.find(' ');
        if (i == std::string::npos) continue;
        std::string mountpoint = s.substr(0, i);
        if (mountpoint == "/") {
            dev = device;
            break;
        }
    }
    fclose(fp);
    if (dev.rfind("/dev/", 0) != 0) return "";
    while (!dev.empty() && dev.back() >= '0' && dev.back() <= '9') {
        dev.pop_back();
    }
    if (!dev.empty() && dev.back() == 'p') dev.pop_back();
    return dev;
}

std::string FindSmartctl() {
    const char *candidates[] = {"/usr/sbin/smartctl", "/sbin/smartctl",
                                "/usr/local/sbin/smartctl"};
    for (const char *p : candidates) {
        if (FileExists(p)) return std::string(p);
    }
    return "";
}
#endif

void RunHddCheck(const CommandParams &, CommandResponse *resp) {
#ifdef _WIN32
    std::string state_dir = pudimagent::platform::DefaultStateDir();
    std::string drive = state_dir.size() >= 2 && state_dir[1] == ':'
                            ? state_dir.substr(0, 3)
                            : "C:\\";
    ULARGE_INTEGER total = {}, free_bytes = {};
    if (GetDiskFreeSpaceExA(drive.c_str(), &free_bytes, &total, nullptr)) {
        unsigned long long used = total.QuadPart > free_bytes.QuadPart
                                      ? total.QuadPart - free_bytes.QuadPart
                                      : 0;
        int pct = total.QuadPart > 0
                      ? static_cast<int>((used * 100) / total.QuadPart)
                      : 0;
        AddField(resp, "volume", drive);
        AddField(resp, "disk_total_bytes", std::to_string(total.QuadPart));
        AddField(resp, "disk_used_bytes", std::to_string(used));
        AddField(resp, "disk_usage_percent", std::to_string(pct));
        if (pct >= 90) {
            resp->add_issues("volume " + drive + " usage is " +
                             std::to_string(pct) + "% (>= 90%)");
        }
    } else {
        AddField(resp, "disk_usage_percent", "unknown");
    }
    AddField(resp, "smart_supported", "false");
    resp->mutable_detail()->append(
        "SMART health check is not available in this build on Windows.\n");
#else
    // 1. Filesystem usage of the root volume (pure API, no shell).
    struct statvfs vfs;
    if (::statvfs("/", &vfs) == 0) {
        unsigned long long total =
            static_cast<unsigned long long>(vfs.f_frsize) * vfs.f_blocks;
        unsigned long long free_bytes =
            static_cast<unsigned long long>(vfs.f_frsize) * vfs.f_bavail;
        unsigned long long used =
            total > free_bytes ? total - free_bytes : 0;
        int pct = total > 0 ? static_cast<int>((used * 100) / total) : 0;
        AddField(resp, "volume", "/");
        AddField(resp, "disk_total_bytes", std::to_string(total));
        AddField(resp, "disk_used_bytes", std::to_string(used));
        AddField(resp, "disk_usage_percent", std::to_string(pct));
        if (pct >= 90) {
            resp->add_issues("root volume usage is " + std::to_string(pct) +
                             "% (>= 90%)");
        }
    } else {
        AddField(resp, "disk_usage_percent", "unknown");
    }

    // 2. SMART health via a fixed smartctl invocation (never requester input).
    std::string smartctl = FindSmartctl();
    std::string disk = RootDisk();
    if (smartctl.empty()) {
        AddField(resp, "smart_supported", "false");
        resp->mutable_detail()->append(
            "smartctl not installed; SMART health check skipped.\n");
    } else if (disk.empty()) {
        AddField(resp, "smart_supported", "false");
        resp->mutable_detail()->append(
            "no raw block device found for the root volume; SMART health "
            "check skipped.\n");
    } else {
        AddField(resp, "smart_supported", "true");
        AddField(resp, "smart_device", disk);
        std::string out = RunFixed(smartctl + " -H -A " + disk);
        bool passed = out.find("PASSED") != std::string::npos;
        bool failed = out.find("FAILED") != std::string::npos;
        AddField(resp, "smart_passed",
                 failed ? "false" : (passed ? "true" : "unknown"));
        resp->mutable_detail()->append("=== smartctl -H -A " + disk + " ===\n");
        resp->mutable_detail()->append(out);
        if (failed) {
            resp->add_issues("SMART health check FAILED for " + disk);
        }
    }
#endif

    resp->set_summary(
        "disk check finished: " +
        (resp->issues_size() == 0
             ? "no issues found"
             : std::to_string(resp->issues_size()) + " issue(s) found"));
    resp->set_success(true);
}

std::string Fmt(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << v;
    std::string s = os.str();
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// Parses the integer (or decimal) token immediately preceding `marker`.
double NumberBefore(const std::string &text, const std::string &marker) {
    size_t pos = text.find(marker);
    if (pos == std::string::npos) return 0;
    size_t end = pos;
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    size_t start = end;
    while (start > 0) {
        char c = text[start - 1];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' ||
            c == '-') {
            --start;
        } else {
            break;
        }
    }
    try {
        return std::stod(text.substr(start, end - start));
    } catch (...) {
        return 0;
    }
}

// True when the candidate binary is Ookla's client. The Python
// `speedtest-cli` package installs entry points named both `speedtest` and
// `speedtest-cli`, so the binary name alone does not identify the vendor.
bool IsOoklaTool(const std::string &bin) {
    if (bin.empty()) return false;
    std::string out = RunBounded(bin + " --version", 15);
    for (char &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out.find("ookla") != std::string::npos;
}

// True when the CLI rejected the flags we passed instead of reporting results.
bool SpeedtestRejectedFlags(const std::string &out) {
    std::string lower;
    lower.reserve(out.size());
    for (char c : out) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower.find("usage:") != std::string::npos ||
           lower.find("unrecognized option") != std::string::npos ||
           lower.find("unknown option") != std::string::npos ||
           lower.find("unrecognized arguments") != std::string::npos ||
           lower.find("traceback") != std::string::npos;
}

// True when the CLI died instead of reporting a result.
bool SpeedtestCrashed(const std::string &out) {
    return out.find("terminate called") != std::string::npos ||
           out.find("what():") != std::string::npos ||
           out.find("core dumped") != std::string::npos ||
           out.find("Segmentation fault") != std::string::npos;
}

// Extracts Ookla's `--format=json` result object.
std::string OoklaResultJson(const std::string &out) {
    std::string first_object;
    size_t line_start = 0;
    while (line_start < out.size()) {
        size_t line_end = out.find('\n', line_start);
        if (line_end == std::string::npos) line_end = out.size();
        const std::string line =
            Trim(out.substr(line_start, line_end - line_start));
        line_start = line_end + 1;
        if (line.empty() || line.front() != '{') continue;
        if (line.find("\"type\":\"result\"") != std::string::npos) return line;
        if (first_object.empty()) first_object = line;
    }
    return first_object;
}

// One-line description of the CLI's output for error messages.
std::string FirstMeaningfulLine(const std::string &out) {
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find('\n', pos);
        if (end == std::string::npos) end = out.size();
        const std::string line = Trim(out.substr(pos, end - pos));
        pos = end + 1;
        if (!line.empty()) return line;
    }
    return "";
}

// Requires a speedtest tool on the agent.
void RunSpeedtest(const CommandParams &params, CommandResponse *resp) {
    std::string server_id = GetParam(params, "server_id", "");
    std::string named =
        FindTool("speedtest", {"/usr/local/bin/speedtest", "/usr/bin/speedtest",
                               "/opt/homebrew/bin/speedtest"});
    std::string cli = FindTool("speedtest-cli", {});
    if (named.empty() && cli.empty()) {
        resp->set_success(false);
        resp->set_error("no speedtest tool on agent: install Ookla 'speedtest' "
                        "or 'speedtest-cli'");
        resp->mutable_detail()->append(resp->error() + "\n");
        resp->set_summary("speedtest unavailable");
        return;
    }

    // Prefer Ookla when present (nested JSON, bandwidth in bits/s); otherwise
    // fall back to speedtest-cli, which reports flat bytes/s fields.
    std::string tool;
    bool ookla = false;
    if (IsOoklaTool(named)) {
        tool = named;
        ookla = true;
    } else if (IsOoklaTool(cli)) {
        tool = cli;
        ookla = true;
    } else {
        tool = cli.empty() ? named : cli;
    }

    std::string base =
        ookla ? (tool + " --format=json --accept-license --accept-gdpr")
              : (tool + " --json");
    if (!server_id.empty()) {
        base += ookla ? (" --server-id=" + server_id)
                      : (" --server " + server_id);
    }

    int status = -1;
    std::string out = RunBounded(base, 420, &status);
    if (SpeedtestRejectedFlags(out)) {
        // Some installs expose a single client under both names, so retry once
        // with the opposite flag spelling before giving up.
        std::string alt =
            ookla ? (tool + " --json")
                  : (tool + " --format=json --accept-license --accept-gdpr");
        if (!server_id.empty()) {
            alt += ookla ? (" --server " + server_id)
                         : (" --server-id=" + server_id);
        }
        out = RunBounded(alt, 420, &status);
    }

    resp->mutable_detail()->append("=== " + tool + " ===\n");
    resp->mutable_detail()->append(out);
    if (!out.empty() && out.back() != '\n') resp->mutable_detail()->append("\n");

    if (Trim(out).empty() || SpeedtestCrashed(out)) {
        resp->set_success(false);
        AddField(resp, "tool", ookla ? "speedtest (ookla)" : "speedtest-cli");
        const std::string line = FirstMeaningfulLine(out);
        resp->set_error(
            "speedtest tool " +
            std::string(Trim(out).empty() ? "produced no output" : "crashed") +
            " (" + DescribeExitStatus(status) + ")" +
            (line.empty() ? "" : ": " + line));
        resp->set_summary("speedtest failed");
        return;
    }

    resp->set_success(true);
    AddField(resp, "tool", ookla ? "speedtest (ookla)" : "speedtest-cli");
    if (!server_id.empty()) AddField(resp, "server_id", server_id);

    // Parse only the machine-readable payload
    std::string parse = ookla ? OoklaResultJson(out) : out;
    if (parse.empty()) parse = out;

    std::string dl_json = JsonBlock(parse, "download", "upload");
    std::string ul_json = JsonBlock(parse, "upload", "packetLoss");
    if (ul_json.empty()) ul_json = JsonBlock(parse, "upload", "");
    std::string ping_json = JsonBlock(parse, "ping", "download");

    double download_mbps =
        ookla ? BitsPerSecToMbps(JsonNumber(dl_json, "bandwidth", 0.0))
              : BytesPerSecToMbps(JsonNumber(parse, "download", 0.0));
    double upload_mbps =
        ookla ? BitsPerSecToMbps(JsonNumber(ul_json, "bandwidth", 0.0))
              : BytesPerSecToMbps(JsonNumber(parse, "upload", 0.0));
    double ping_ms =
        JsonNumber(ping_json, "latency", JsonNumber(parse, "ping", 0.0));
    double jitter_ms = JsonNumber(ping_json, "jitter", 0.0);
    double loss_pct = JsonNumber(parse, "packetLoss", -1.0);
    std::string server_name =
        JsonValue(JsonBlock(parse, "server", "result"), "name");
    if (server_name.empty()) server_name = JsonValue(parse, "server");
    std::string isp = JsonValue(parse, "isp");

    AddField(resp, "download_mbps", Fmt(download_mbps));
    AddField(resp, "upload_mbps", Fmt(upload_mbps));
    if (ping_ms > 0) AddField(resp, "ping_ms", Fmt(ping_ms));
    if (jitter_ms > 0) AddField(resp, "jitter_ms", Fmt(jitter_ms));
    if (loss_pct >= 0) AddField(resp, "packet_loss_pct", Fmt(loss_pct));
    if (!server_name.empty()) AddField(resp, "server", server_name);
    if (!isp.empty()) AddField(resp, "isp", isp);

    if (loss_pct >= 5.0) {
        resp->add_issues("speedtest measured " + Fmt(loss_pct) +
                         "% packet loss");
    }
    if (download_mbps <= 0.0 && upload_mbps <= 0.0) {
        resp->add_issues("speedtest reported zero throughput");
    }
    resp->set_summary("download=" + Fmt(download_mbps) +
                      " Mbps upload=" + Fmt(upload_mbps) +
                      " ping=" + Fmt(ping_ms) + " ms");
}

// Sustained ICMP burst measuring loss and jitter over time
void RunPingBurst(const CommandParams &params, CommandResponse *resp) {
    std::string target = GetParam(params, "target", "1.1.1.1");
    int64_t count = ParseIntParam(params, "count", 100, 5, 1000);
    int64_t interval_ms = ParseIntParam(params, "interval_ms", 200, 50, 2000);

    AddField(resp, "target", target);
    AddField(resp, "count", std::to_string(count));

    std::string cmd;
    int64_t est_s = (count * interval_ms) / 1000;
#ifdef _WIN32
    cmd = "ping -n " + std::to_string(count) + " -w 2000 " + target;
#else
    std::string interval_s = Fmt(static_cast<double>(interval_ms) / 1000.0);
    cmd = "ping -c " + std::to_string(count) + " -i " + interval_s + " " +
          target;
#endif
    std::string out = RunBounded(cmd, static_cast<int>(est_s + 60));
    resp->mutable_detail()->append("=== " + cmd + " ===\n");
    resp->mutable_detail()->append(out);
    if (!out.empty() && out.back() != '\n') resp->mutable_detail()->append("\n");

    // Linux: "64 packets transmitted, 64 received, 0.0% packet loss" and
    //        "rtt min/avg/max/mdev = 1.234/2.345/3.456/4.567 ms".
    // Windows: "Packets: Sent = 100, Received = 98, Lost = 2 (2% loss)" and
    //          "Minimum = 1ms, Maximum = 4ms, Average = 2ms".
    double sent = 0, received = 0, loss_pct = 0;
    if (out.find("packets transmitted") != std::string::npos) {
        sent = NumberBefore(out, "packets transmitted");
        received = NumberBefore(out, "received");
        loss_pct = NumberBefore(out, "% packet loss");
    } else {
        sent = NumberBefore(out, "Sent =");
        received = NumberBefore(out, "Received =");
        size_t lost_marker = out.find("Lost =");
        if (lost_marker != std::string::npos) {
            size_t pct = out.find('%', lost_marker);
            if (pct != std::string::npos) {
                size_t start = pct;
                while (start > lost_marker &&
                       (std::isdigit(static_cast<unsigned char>(out[start - 1])) ||
                        out[start - 1] == '.')) --start;
                try {
                    loss_pct = std::stod(out.substr(start, pct - start));
                } catch (...) {
                    loss_pct = 0;
                }
            }
        }
    }

    double min_rtt = 0, avg_rtt = 0, max_rtt = 0, mdev = 0;
    size_t eq = out.find("min/avg/max/mdev =");
    if (eq != std::string::npos) {
        size_t a = out.find('=', eq) + 1;
        size_t b = out.find(" ms", a);
        std::string part =
            Trim(out.substr(a, b == std::string::npos ? std::string::npos
                                                      : b - a));
        std::vector<std::string> nums;
        std::string cur;
        for (char c : part) {
            if (c == '/') {
                nums.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) nums.push_back(cur);
        if (nums.size() >= 4) {
            try {
                min_rtt = std::stod(nums[0]);
                avg_rtt = std::stod(nums[1]);
                max_rtt = std::stod(nums[2]);
                mdev = std::stod(nums[3]);
            } catch (...) {
            }
        }
    } else {
        min_rtt = NumberBefore(out, "Minimum =");
        max_rtt = NumberBefore(out, "Maximum =");
        avg_rtt = NumberBefore(out, "Average =");
    }

    bool parsed = sent > 0;
    if (parsed) {
        AddField(resp, "sent", Fmt(sent));
        AddField(resp, "received", Fmt(received));
        AddField(resp, "packet_loss_pct", Fmt(loss_pct));
    }
    if (avg_rtt > 0) {
        AddField(resp, "rtt_avg_ms", Fmt(avg_rtt));
        AddField(resp, "rtt_min_ms", min_rtt > 0 ? Fmt(min_rtt) : "0");
        AddField(resp, "rtt_max_ms", max_rtt > 0 ? Fmt(max_rtt) : "0");
    }
    if (mdev > 0) AddField(resp, "jitter_ms", Fmt(mdev));

    if (loss_pct >= 20.0) {
        resp->add_issues("packet loss of " + Fmt(loss_pct) + "% toward " +
                         target);
    } else if (parsed && (received < sent || loss_pct > 0)) {
        resp->add_issues("some packets lost toward " + target);
    }
    if (avg_rtt >= 500.0) {
        resp->add_issues("average RTT " + Fmt(avg_rtt) + " ms toward " +
                         target + " is high");
    }
    resp->set_success(true);
    resp->set_summary(
        parsed ? ("burst to " + target + ": " + Fmt(sent) + " sent, " +
                  Fmt(received) + " received, " + Fmt(loss_pct) +
                  "% loss, avg " + Fmt(avg_rtt) + " ms")
               : ("ping burst to " + target +
                  " completed (output may be localized; see detail for raw "
                  "results)"));
}

// Measures per-hop path loss over time with mtr (Linux) or pathping (Windows).
// falling back to traceroute when unavailable.
void RunRouteQuality(const CommandParams &params, CommandResponse *resp) {
    std::string target = GetParam(params, "target", "1.1.1.1");
    int64_t packets = ParseIntParam(params, "packets", 20, 5, 100);

    AddField(resp, "target", target);
    AddField(resp, "packets", std::to_string(packets));

    std::string out;
    std::string mode;
#ifdef _WIN32
    mode = "pathping";
    out = RunBounded("pathping -n -q 10 -w 500 -h 15 " + target, 600);
#else
    std::string mtr =
        FindTool("mtr", {"/usr/bin/mtr", "/usr/sbin/mtr",
                         "/usr/local/bin/mtr"});
    if (!mtr.empty()) {
        mode = "mtr";
        out = RunBounded(mtr + " -r -c " + std::to_string(packets) + " -n " +
                             target, 420);
    } else {
        mode = "traceroute";
        out = RunBounded("traceroute -n -m 15 -w 1 " + target, 120);
    }
#endif

    resp->mutable_detail()->append("=== " + mode + " " + target + " ===\n");
    resp->mutable_detail()->append(out);
    if (!out.empty() && out.back() != '\n') resp->mutable_detail()->append("\n");

    resp->set_success(true);
    AddField(resp, "tool", mode);

    // mtr: "  3.|-- 10.0.0.1     0.0%    20   1.0   1.2 ..."
    int hops = 0;
    double worst_loss = 0;
    int worst_hop = 0;
    double dest_loss = 0;
    int dest_hop = 0;
    std::vector<std::string> lines;
    std::string cur;
    for (char c : out) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);

    for (const auto &line : lines) {
        if (mode == "mtr") {
            if (line.find("|--") == std::string::npos) continue;
            ++hops;
            double loss = 0;
            if (!ParseMtrLoss(line, &loss)) continue;
            dest_loss = loss;
            dest_hop = hops;
            if (loss > worst_loss) {
                worst_loss = loss;
                worst_hop = hops;
            }
        } else if (mode == "pathping") {
            // pathping rows look like "  5  10.0.0.1  0/ 100 = 0%".
            if (line.find('/') == std::string::npos ||
                line.find('=') == std::string::npos) continue;
            ++hops;
            size_t eq_pos = line.find('=');
            size_t pct = line.find('%', eq_pos);
            if (pct == std::string::npos) continue;
            size_t start = eq_pos + 1;
            while (start < pct &&
                   !std::isdigit(static_cast<unsigned char>(line[start]))) ++start;
            try {
                double loss = std::stod(line.substr(start, pct - start));
                dest_loss = loss;
                dest_hop = hops;
                if (loss > worst_loss) {
                    worst_loss = loss;
                    worst_hop = hops;
                }
            } catch (...) {
            }
        } else {
            // traceroute fallback: count numbered hop rows (line starts with
            // " N."), loss parsing is left to the raw detail output.
            if (Trim(line).empty() || line.size() < 3) continue;
            if (line[0] == ' ' && (std::isdigit(static_cast<unsigned char>(
                                       Trim(line)[0])) ||
                                   Trim(line)[0] == '*')) {
                ++hops;
            }
        }
    }

    AddField(resp, "hops", std::to_string(hops));
    if (worst_loss > 0) {
        AddField(resp, "worst_hop_loss_pct", Fmt(worst_loss));
        AddField(resp, "worst_hop", std::to_string(worst_hop));
    }
    if (mode == "traceroute") {
        AddField(resp, "note",
                 "mtr not installed; per-hop loss parsing skipped (raw "
                 "traceroute available in detail)");
        resp->set_summary(mode + " to " + target + ": " +
                          std::to_string(hops) +
                          " hops (per-hop loss unavailable)");
        return;
    }

    AddField(resp, "destination_hop", std::to_string(dest_hop));
    AddField(resp, "destination_loss_pct", Fmt(dest_loss));
    if (dest_loss >= 20.0) {
        resp->add_issues("destination loss of " + Fmt(dest_loss) +
                         "% toward " + target);
    } else if (worst_loss >= 20.0) {
        AddField(resp, "note",
                 "loss on intermediate hops (" + Fmt(worst_loss) +
                     "% at hop " + std::to_string(worst_hop) +
                     ") is usually ICMP rate limiting; the target itself is " +
                     Fmt(dest_loss) + "%");
    }
    resp->set_summary(mode + " to " + target + ": " + std::to_string(hops) +
                      " hops, destination loss " + Fmt(dest_loss) + "%");
}

struct CommandDef {
    const char *id;
    const char *description;
    std::vector<const char *> param_names;
    void (*run)(const CommandParams &, CommandResponse *);
};

const std::vector<CommandDef> &Catalog() {
    static const std::vector<CommandDef> kCatalog = {
        {"hdd_check",
         "Disk health check: usage of the volume holding the agent, plus a "
         "SMART self-assessment when smartctl is available.",
         {},
         RunHddCheck},
        {"agent_info",
         "Reports the agent's OS, architecture and hostname (lightweight "
         "self-test).",
         {},
         RunAgentInfo},
        {"speedtest",
         "Heavy: measures internet download/upload throughput, latency, "
         "jitter and loss via a speedtest CLI installed on the agent (Ookla "
         "'speedtest' or 'speedtest-cli'). Takes roughly a minute.",
         {"server_id"},
         RunSpeedtest},
        {"ping_burst",
         "Heavy: sustained ICMP burst (default 100 packets at 200 ms) "
         "measuring packet loss, RTT spread and jitter toward a target. Runs "
         "for tens of seconds, unlike the light per-heartbeat probes.",
         {"target", "count", "interval_ms"},
         RunPingBurst},
        {"route_quality",
         "Heavy: per-hop path loss toward a target using mtr (Linux) or "
         "pathping (Windows), with traceroute as fallback.",
         {"target", "packets"},
         RunRouteQuality},
    };
    return kCatalog;
}

} // anonymous namespace

const std::string &ToolHomeDir() { return ToolHomeDirImpl(); }

bool ParseMtrLoss(const std::string &line, double *loss) {
    if (loss == nullptr) return false;
    size_t pos = line.find("|--");
    if (pos == std::string::npos) return false;
    pos += 3;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    // Skip the host/IP column.
    while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    const size_t start = pos;
    while (pos < line.size() &&
           (std::isdigit(static_cast<unsigned char>(line[pos])) ||
            line[pos] == '.')) {
        ++pos;
    }
    if (pos == start) return false;
    try {
        *loss = std::stod(line.substr(start, pos - start));
        return true;
    } catch (...) {
        return false;
    }
}

void ListCommands(pudimnetmon::ListCommandsResponse *resp) {
    if (!resp) return;
    for (const auto &c : Catalog()) {
        auto *info = resp->add_commands();
        info->set_command_id(c.id);
        info->set_description(c.description);
        for (const char *p : c.param_names) {
            info->add_param_names(p);
        }
    }
    resp->set_success(true);
}

bool RunCommand(const std::string &id, const CommandParams &params,
                pudimnetmon::CommandResponse *resp) {
    if (!resp) return false;
    resp->set_command_id(id);
    resp->set_timestamp_unix_ms(NowMs());
    for (const auto &c : Catalog()) {
        if (id != c.id) continue;
        LOG_INFO("Running pre-set command '" + id + "'");
        try {
            c.run(params, resp);
        } catch (const std::exception &e) {
            resp->set_success(false);
            resp->set_error(std::string("command threw: ") + e.what());
        }
        if (resp->summary().empty()) {
            resp->set_summary(resp->success() ? "ok" : "failed");
        }
        if (resp->issues_size() > 0) {
            LOG_WARN("Pre-set command '" + id + "' found " +
                     std::to_string(resp->issues_size()) + " issue(s)");
        }
        return true;
    }
    resp->set_success(false);
    resp->set_error("unknown command '" + id + "' (see ListCommands)");
    return false;
}

} // namespace pudimagent
