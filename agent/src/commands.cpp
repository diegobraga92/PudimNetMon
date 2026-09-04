#include <chrono>
#include <cstdio>
#include <exception>
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

std::string RunFixed(const std::string &cmd) {
    FILE *fp = popen((cmd + " 2>&1").c_str(), "r");
    if (!fp) return "";
    std::string out = ReadAll(fp);
    pclose(fp);
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

struct CommandDef {
    const char *id;
    const char *description;
    void (*run)(const CommandParams &, CommandResponse *);
};

const std::vector<CommandDef> &Catalog() {
    static const std::vector<CommandDef> kCatalog = {
        {"hdd_check",
         "Disk health check: usage of the volume holding the agent, plus a "
         "SMART self-assessment when smartctl is available.",
         RunHddCheck},
        {"agent_info",
         "Reports the agent's OS, architecture and hostname (lightweight "
         "self-test).",
         RunAgentInfo},
    };
    return kCatalog;
}

} // anonymous namespace

void ListCommands(pudimnetmon::ListCommandsResponse *resp) {
    if (!resp) return;
    for (const auto &c : Catalog()) {
        auto *info = resp->add_commands();
        info->set_command_id(c.id);
        info->set_description(c.description);
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
