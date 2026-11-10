#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "IpcProtocol.h"

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace ammod::remote {

struct PluginStatus {
    bool connected{false};
    uint16_t state{0};
    std::string stateName{"Off"};
    bool enabled{false};
    uint32_t hardwareBufferMs{20};
    bool allowResampling{false};
    std::string endpointId;
    std::string endpointName;
    std::string format;
    std::string detail;
    uint32_t errorCategory{0};
    std::string errorString;
};

struct DeviceInfo {
    std::string id;
    std::string name;
    std::string type; // "wasapi", "asio", "dlna"
    bool online{true};
    std::string location;
};

class AmPluginClient {
public:
    static AmPluginClient& Instance();

    void Start();
    void Stop();

    PluginStatus GetStatus();
    std::vector<DeviceInfo> GetDevices();

    bool SetEnabled(bool enabled);
    bool SetHardwareBuffer(uint32_t ms);
    bool SetAllowResampling(bool allow);
    bool SetOutputTarget(uint8_t kind, const std::wstring& id, const std::wstring& name, const std::wstring& location = L"");
    void RefreshDevices();

private:
    AmPluginClient();
    ~AmPluginClient();

    void WorkerLoop();
    void HandleStatusMessage(const ipc::Message& msg);
    void HandleDeviceMessage(const ipc::Message& msg, bool found);

    std::atomic<bool> running_{false};
    std::thread workerThread_;

    std::mutex pipeMutex_;
    HANDLE pipe_{INVALID_HANDLE_VALUE};

    std::mutex stateMutex_;
    PluginStatus currentStatus_;
    std::vector<DeviceInfo> devices_;
};

} // namespace ammod::remote
