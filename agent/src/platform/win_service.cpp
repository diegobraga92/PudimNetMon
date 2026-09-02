// Windows Service runtime integration for pudim-agent.
//
// The agent binary is a console application and a Windows service: when the
// Service Control Manager launches it, StartServiceCtrlDispatcher() routes
// execution to ServiceMain, which runs the normal agent main in a worker
// thread and pumps SERVICE_CONTROL_STOP/SHUTDOWN into the agent's stop flag.
//
// Registering/removing the "PudimNetMonAgent" service is owned by the Inno
// Setup installer (installer\installer-agent.iss drives sc.exe directly), so
// the binary itself exposes no --install-service/--uninstall-service verbs.
//
// On non-Windows builds this file compiles to an empty translation unit with
// safe no-op stubs, so it can be part of the build unconditionally.

#include "win_service.h"

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

} // namespace

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

bool InitNetwork(std::string &) { return true; }
void CleanupNetwork() {}
bool TryRunAsService(int, char **, std::function<int(int, char **)>,
                     std::function<void()>) {
    return false;
}

} // namespace pudimagent::platform

#endif

