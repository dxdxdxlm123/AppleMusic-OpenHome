#include "RemoteServer.h"
#include "AmPluginClient.h"

#include <windows.h>
#include <winrt/base.h>

#include <csignal>
#include <iostream>
#include <atomic>

namespace {
std::atomic<bool> g_stopRequested = false;

BOOL WINAPI ConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        g_stopRequested = true;
        return TRUE;
    }
    return FALSE;
}
}

int wmain(int argc, wchar_t** argv) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\AMExclusiveRemoteServerMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0; // Already running silently in background
    }

    HANDLE hStopEvent = CreateEventW(nullptr, FALSE, FALSE, L"Global\\AMExclusiveRemoteStopEvent");
    if (hStopEvent) {
        ResetEvent(hStopEvent);
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    int port = 10767;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--port") == 0 && i + 1 < argc) {
            port = _wtoi(argv[++i]);
        }
    }

    ammod::remote::AmPluginClient::Instance().Start();

    ammod::remote::RemoteServer server(port);
    if (!server.Start()) {
        ammod::remote::AmPluginClient::Instance().Stop();
        if (hStopEvent) CloseHandle(hStopEvent);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    while (!g_stopRequested) {
        DWORD wait = WaitForSingleObject(hStopEvent, 500);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
    }

    server.Stop();
    ammod::remote::AmPluginClient::Instance().Stop();
    if (hStopEvent) CloseHandle(hStopEvent);
    if (hMutex) CloseHandle(hMutex);

    return 0;
}
