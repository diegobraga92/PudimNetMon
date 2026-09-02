// Windows Service integration for pudim-agent.
//
// The agent binary is both a console application and a Windows service. When
// the Service Control Manager launches it, StartServiceCtrlDispatcher() routes
// execution to ServiceMain, which runs the normal agent main in a worker
// thread and pumps SERVICE_CONTROL_STOP/SHUTDOWN into the agent's stop flag.
//
// On non-Windows builds this file compiles to an empty translation unit with
// safe no-op stubs, so it can be part of the build unconditionally.

#include "win_service.h"

#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <winsvc.h>

namespace pudimagent::platform {

namespace {

const wchar_t kServiceName[] = L"PudimNetMonAgent";
const wchar_t kServiceDisplayName[] = L"PudimNetMon Agent";

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_stop_event = nullptr;
DWORD g_exit_code = 0;

std::function<int(int, char **)> g_run_main;
std::function<void()> g_on_stop;

void ReportStatus(DWORD state, DWORD wait_hint) {
    if (!g_status_handle) return;
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN)
                                   : 0;
    status.dwWin32ExitCode = NO_ERROR;
    status.dwServiceSpecificExitCode = 0;
    status.dwCheckPoint = 0;
    status.dwWaitHint = wait_hint;
    SetServiceStatus(g_status_handle, &status);
}

DWORD WINAPI ControlHandler(DWORD control, DWORD, void *, void *) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportStatus(SERVICE_STOP_PENDING, 30000);
            if (g_on_stop) g_on_stop();
            if (g_stop_event) SetEvent(g_stop_event);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// UTF-8 argv storage, kept alive for the whole service lifetime.
std::vector<std::string> g_svc_argv_strs;
std::vector<char *> g_svc_argv_ptrs;

void WINAPI ServiceMain(DWORD argc, LPWSTR *argv) {
    g_status_handle =
        RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
    if (!g_status_handle) return;
    ReportStatus(SERVICE_START_PENDING, 5000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) return;

    // Convert the SCM-provided command line to a UTF-8 argv for the agent.
    g_svc_argv_strs.clear();
    g_svc_argv_ptrs.clear();
    g_svc_argv_ptrs.reserve(argc);
    for (DWORD i = 0; i < argc; ++i) {
        g_svc_argv_strs.push_back(WideToUtf8(argv[i]));
        g_svc_argv_ptrs.push_back(
            const_cast<char *>(g_svc_argv_strs.back().c_str()));
    }
    int svc_argc = static_cast<int>(g_svc_argv_ptrs.size());
    char **svc_argv = svc_argc > 0 ? g_svc_argv_ptrs.data() : nullptr;

    std::thread worker([svc_argc, svc_argv]() {
        g_exit_code = g_run_main ? g_run_main(svc_argc, svc_argv) : 1;
    });

    ReportStatus(SERVICE_RUNNING, 0);
    WaitForSingleObject(g_stop_event, INFINITE);

    if (worker.joinable()) worker.join();
    ReportStatus(SERVICE_STOPPED, 0);
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
}

// Formats a specific Win32 error code. LastErrorString() only reads the
// thread's last error, so restore the code first.
std::string ErrorText(DWORD err) {
    if (err == ERROR_SUCCESS) return std::string();
    ::SetLastError(err);
    return LastErrorString();
}

// Encodes a single command-line token so that a later Windows argv parse (the
// CRT startup the SCM performs when starting the service) yields the same
// token. Per the CommandLineToArgvW rules, backslashes preceding a quote are
// doubled (2n+1 -> n literal + escaped quote) and trailing backslashes are
// doubled so the closing quote is not consumed as an escape.
std::wstring QuoteArg(const std::wstring &s) {
    if (s.empty()) return L"\"\"";
    bool need_quotes = false;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'"') {
            need_quotes = true;
            break;
        }
    }
    if (!need_quotes) return s;

    std::wstring out;
    out.reserve(s.size() + 2);
    out.push_back(L'"');
    size_t i = 0;
    while (i < s.size()) {
        size_t bs = 0;
        while (i < s.size() && s[i] == L'\\') {
            ++bs;
            ++i;
        }
        if (i >= s.size()) {
            out.append(bs * 2, L'\\');  // trailing backslashes: keep them literal
            break;
        }
        if (s[i] == L'"') {
            out.append(bs * 2 + 1, L'\\');  // escape the quote (literal, not delimiter)
            out.push_back(L'"');
            ++i;
        } else {
            out.append(bs, L'\\');  // backslashes before any other char are literal
            out.push_back(s[i]);    // and the ordinary character itself
            ++i;
        }
    }
    out.push_back(L'"');
    return out;
}

} // namespace

bool WantsInstallService(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--install-service") == 0) return true;
    }
    return false;
}

bool WantsUninstallService(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--uninstall-service") == 0) return true;
    }
    return false;
}

bool InstallAgentService(int argc, char **argv, std::string &error) {
    error.clear();
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        error = "cannot determine the agent executable path";
        return false;
    }

    // ImagePath = "<exe>" + every forwarded CLI token (the --install-service
    // verb itself is stripped). Each token is quoted so values containing
    // spaces survive the SCM's re-parse of this command line.
    std::wstring bin = QuoteArg(exe);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--install-service") == 0) continue;
        bin += L" " + QuoteArg(Utf8ToWide(argv[i]));
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        error = ErrorText(GetLastError());
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        bin.c_str(), nullptr, nullptr, nullptr, L"NT AUTHORITY\\LocalSystem",
        nullptr);
    DWORD err = svc ? ERROR_SUCCESS : GetLastError();

    if (!svc && err == ERROR_SERVICE_EXISTS) {
        // Reinstall/upgrade: the service already exists. Update its ImagePath
        // (and description) so new arguments actually take effect instead of
        // silently keeping the stale registration from the previous install.
        svc = OpenServiceW(scm, kServiceName,
                           SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
        err = svc ? ERROR_SUCCESS : GetLastError();
        if (svc &&
            !ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                  SERVICE_NO_CHANGE, bin.c_str(), nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr)) {
            err = GetLastError();
        }
    }
    if (svc) {
        SERVICE_DESCRIPTIONW desc{ L"PudimNetMon network monitoring agent" };
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);

    if (err != ERROR_SUCCESS) {
        error = ErrorText(err);
        if (err == ERROR_ACCESS_DENIED) error += " (run from an elevated prompt)";
        return false;
    }
    return true;
}

bool UninstallAgentService(std::string &error) {
    error.clear();
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = ErrorText(GetLastError());
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        // Idempotent uninstall: nothing left to remove is not a failure.
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        error = ErrorText(err);
        return false;
    }

    SERVICE_STATUS status{};
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        // Wait up to ~10s for the stop to complete (DeleteService requires it).
        for (int i = 0; i < 20; ++i) {
            QueryServiceStatus(svc, &status);
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(500);
        }
    }
    bool deleted = DeleteService(svc) != 0;
    DWORD err = deleted ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    if (!deleted) {
        if (err == ERROR_SERVICE_MARKED_FOR_DELETE) {
            error = "service is still stopping; retry in a few seconds";
        } else {
            error = ErrorText(err);
        }
        return false;
    }
    return true;
}

bool InitNetwork(std::string &error) {
    WSADATA wsa{};
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        error = "WSAStartup failed with code " + std::to_string(rc);
        return false;
    }
    return true;
}

void CleanupNetwork() { WSACleanup(); }

bool TryRunAsService(int argc, char **argv,
                     std::function<int(int, char **)> run_main,
                     std::function<void()> on_stop) {
    (void)argc;
    (void)argv;
    g_run_main = std::move(run_main);
    g_on_stop = std::move(on_stop);

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr },
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        // ERROR_FAILED_SERVICE_CONTROLLER_CONNECT means we were not launched by
        // the SCM -> fall through to console mode.
        return false;
    }
    return true;
}

} // namespace pudimagent::platform

#else  // !_WIN32

namespace pudimagent::platform {

bool WantsInstallService(int, char **) { return false; }
bool WantsUninstallService(int, char **) { return false; }
bool InstallAgentService(int, char **, std::string &) { return false; }
bool UninstallAgentService(std::string &) { return false; }
bool InitNetwork(std::string &) { return true; }
void CleanupNetwork() {}
bool TryRunAsService(int, char **, std::function<int(int, char **)>,
                     std::function<void()>) {
    return false;
}

} // namespace pudimagent::platform

#endif

