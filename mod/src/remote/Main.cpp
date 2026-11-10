#include "RemoteServer.h"

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
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    int port = 10767;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--port") == 0 && i + 1 < argc) {
            port = _wtoi(argv[++i]);
        }
    }

    std::wcout << L"============================================================" << std::endl;
    std::wcout << L" AMExclusive Remote Bridge v0.1" << std::endl;
    std::wcout << L" Compatible with 'Remote for Cider' Android App" << std::endl;
    std::wcout << L" Listening on: http://0.0.0.0:" << port << std::endl;
    std::wcout << L" Bit-perfect Apple Music Remote Control Active" << std::endl;
    std::wcout << L" Press Ctrl+C to stop." << std::endl;
    std::wcout << L"============================================================" << std::endl;

    ammod::remote::RemoteServer server(port);
    if (!server.Start()) {
        std::wcerr << L"Failed to start remote server on port " << port << std::endl;
        return 1;
    }

    while (!g_stopRequested) {
        Sleep(500);
    }

    std::wcout << L"\nShutting down remote server..." << std::endl;
    server.Stop();
    std::wcout << L"Server stopped cleanly." << std::endl;

    return 0;
}
