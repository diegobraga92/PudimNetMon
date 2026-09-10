#include "win_service.h"

#include <string>
#include <thread>

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

// Process command line captured from main() in TryRunAsService.
int g_proc_argc = 0;
char **g_proc_argv = nullptr;

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

void WINAPI ServiceMain(DWORD, LPWSTR *) {
    g_status_handle =
        RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
    if (!g_status_handle) return;
    ReportStatus(SERVICE_START_PENDING, 5000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) return;

    // Run the agent with the process command line so that the ImagePath
    // arguments (e.g. --node-id=...) reach the config loader.
    static char *kFallbackArgv[] = {const_cast<char *>("pudim-agent"), nullptr};
    int agent_argc = 1;
    char **agent_argv = kFallbackArgv;
    if (g_proc_argv && g_proc_argc > 0) {
        agent_argc = g_proc_argc;
        agent_argv = g_proc_argv;
    }

    std::thread worker([agent_argc, agent_argv]() {
        g_exit_code = g_run_main ? g_run_main(agent_argc, agent_argv) : 1;
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
    g_run_main = std::move(run_main);
    g_on_stop = std::move(on_stop);
    g_proc_argc = argc;
    g_proc_argv = argv;

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr },
    };
    if (!StartServiceCtrlDispatcherW(table)) {
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

