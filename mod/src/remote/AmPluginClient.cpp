#include "AmPluginClient.h"
#include "IpcTransport.h"
#include "LocalOutputDevices.h"

#include <iostream>
#include <algorithm>

namespace ammod::remote {

namespace {

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

std::string StateToString(ipc::RuntimeState state) {
    switch (state) {
    case ipc::RuntimeState::Off: return "Off";
    case ipc::RuntimeState::Requested: return "Requested";
    case ipc::RuntimeState::WaitingForStream: return "WaitingForStream";
    case ipc::RuntimeState::Probing: return "Probing";
    case ipc::RuntimeState::Initializing: return "Initializing";
    case ipc::RuntimeState::AlignRetry: return "AlignRetry";
    case ipc::RuntimeState::Active: return "Active";
    case ipc::RuntimeState::DisablePending: return "DisablePending";
    case ipc::RuntimeState::FailedOff: return "FailedOff";
    default: return "Unknown";
    }
}

} // namespace

AmPluginClient& AmPluginClient::Instance() {
    static AmPluginClient instance;
    return instance;
}

AmPluginClient::AmPluginClient() = default;

AmPluginClient::~AmPluginClient() {
    Stop();
}

void AmPluginClient::Start() {
    if (running_.exchange(true)) return;

    // Load local devices (WASAPI + ASIO) initially
    {
        std::lock_guard lock(stateMutex_);
        devices_.clear();
        const auto wasapi = local_devices::EnumerateWasapi();
        for (const auto& d : wasapi) {
            devices_.push_back({ Narrow(d.id), Narrow(d.name), "wasapi", true, "" });
        }
        const auto asio = local_devices::EnumerateAsio();
        for (const auto& d : asio) {
            devices_.push_back({ Narrow(d.id), Narrow(d.name), "asio", d.usable, Narrow(d.note) });
        }
    }

    workerThread_ = std::thread([this]() { WorkerLoop(); });
}

void AmPluginClient::Stop() {
    if (!running_.exchange(false)) return;

    {
        std::lock_guard lock(pipeMutex_);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void AmPluginClient::WorkerLoop() {
    while (running_) {
        const std::wstring pipeName = ipc::PipeName();
        if (pipeName.empty()) {
            Sleep(1000);
            continue;
        }

        HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            {
                std::lock_guard lock(stateMutex_);
                currentStatus_.connected = false;
            }
            Sleep(1500);
            continue;
        }

        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
            CloseHandle(pipe);
            Sleep(1000);
            continue;
        }

        // Send Hello
        auto hello = ipc::NewMessage(ipc::MessageType::Hello, ipc::Role::Controller);
        hello.senderPid = GetCurrentProcessId();
        if (!ipc::WriteMessage(pipe, hello)) {
            CloseHandle(pipe);
            Sleep(1000);
            continue;
        }

        {
            std::lock_guard lock(pipeMutex_);
            pipe_ = pipe;
        }

        {
            std::lock_guard lock(stateMutex_);
            currentStatus_.connected = true;
        }
        std::cout << "[AM-CLIENT] Connected to AMExclusive broker pipe" << std::endl;

        // Request device refresh
        auto refresh = ipc::NewMessage(ipc::MessageType::RefreshDevices, ipc::Role::Controller);
        refresh.senderPid = GetCurrentProcessId();
        (void)ipc::WriteMessage(pipe, refresh);

        // Message reading loop
        ipc::Message incoming{};
        while (running_) {
            if (!ipc::ReadMessageTimeout(pipe, incoming, 250)) {
                DWORD err = GetLastError();
                if (err == ERROR_TIMEOUT) continue;
                break; // Pipe disconnected
            }

            if (incoming.type == ipc::MessageType::StatusChanged) {
                HandleStatusMessage(incoming);
            } else if (incoming.type == ipc::MessageType::DeviceFound ||
                       incoming.type == ipc::MessageType::DeviceLost) {
                HandleDeviceMessage(incoming, incoming.type == ipc::MessageType::DeviceFound);
            }
        }

        std::cout << "[AM-CLIENT] Disconnected from AMExclusive broker pipe" << std::endl;
        {
            std::lock_guard lock(pipeMutex_);
            if (pipe_ == pipe) pipe_ = INVALID_HANDLE_VALUE;
            CloseHandle(pipe);
        }
        {
            std::lock_guard lock(stateMutex_);
            currentStatus_.connected = false;
        }

        Sleep(1000);
    }
}

void AmPluginClient::HandleStatusMessage(const ipc::Message& msg) {
    std::lock_guard lock(stateMutex_);
    currentStatus_.connected = true;
    currentStatus_.state = static_cast<uint16_t>(msg.state);
    currentStatus_.stateName = StateToString(msg.state);
    currentStatus_.enabled = (msg.enabledIntent != 0);
    currentStatus_.hardwareBufferMs = msg.hardwareBufferMs;
    currentStatus_.allowResampling = (msg.allowResampling != 0);
    currentStatus_.endpointId = Narrow(msg.endpointId);
    currentStatus_.endpointName = Narrow(msg.endpointName);
    currentStatus_.format = Narrow(msg.format);
    currentStatus_.detail = Narrow(msg.detail);
    currentStatus_.errorCategory = static_cast<uint32_t>(msg.error);
    if (msg.hresult != S_OK) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"0x%08X", static_cast<unsigned>(msg.hresult));
        currentStatus_.errorString = Narrow(buf);
    } else {
        currentStatus_.errorString.clear();
    }
}

void AmPluginClient::HandleDeviceMessage(const ipc::Message& msg, bool found) {
    std::lock_guard lock(stateMutex_);
    std::string id = Narrow(msg.endpointId);
    std::string name = Narrow(msg.endpointName);
    std::string loc = Narrow(msg.detail);
    std::string type = (msg.reserved[3] == 1u) ? "raat" : "dlna";

    auto it = std::find_if(devices_.begin(), devices_.end(),
                           [&id](const DeviceInfo& d) { return d.id == id; });
    if (it != devices_.end()) {
        it->online = found;
        if (!name.empty()) it->name = name;
        if (!loc.empty()) it->location = loc;
        it->type = type;
    } else if (found && !id.empty()) {
        devices_.push_back({ id, name, type, true, loc });
    }
}

PluginStatus AmPluginClient::GetStatus() {
    std::lock_guard lock(stateMutex_);
    return currentStatus_;
}

std::vector<DeviceInfo> AmPluginClient::GetDevices() {
    std::lock_guard lock(stateMutex_);
    return devices_;
}

bool AmPluginClient::SetEnabled(bool enabled) {
    std::lock_guard lock(pipeMutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    auto msg = ipc::NewMessage(ipc::MessageType::SetEnabled, ipc::Role::Controller);
    msg.senderPid = GetCurrentProcessId();
    msg.enabledIntent = enabled ? 1 : 0;
    bool ok = ipc::WriteMessage(pipe_, msg);
    if (ok) {
        std::lock_guard sLock(stateMutex_);
        currentStatus_.enabled = enabled;
        if (enabled && currentStatus_.state == 0) {
            currentStatus_.state = 1; // Requested
            currentStatus_.stateName = "Requested";
        } else if (!enabled) {
            currentStatus_.state = 0; // Off
            currentStatus_.stateName = "Off";
        }
    }
    return ok;
}

bool AmPluginClient::SetHardwareBuffer(uint32_t ms) {
    std::lock_guard lock(pipeMutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    auto msg = ipc::NewMessage(ipc::MessageType::SetHardwareBuffer, ipc::Role::Controller);
    msg.senderPid = GetCurrentProcessId();
    msg.hardwareBufferMs = ms;
    bool ok = ipc::WriteMessage(pipe_, msg);
    if (ok) {
        std::lock_guard sLock(stateMutex_);
        currentStatus_.hardwareBufferMs = ms;
    }
    return ok;
}

bool AmPluginClient::SetAllowResampling(bool allow) {
    std::lock_guard lock(pipeMutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    auto msg = ipc::NewMessage(ipc::MessageType::SetAllowResampling, ipc::Role::Controller);
    msg.senderPid = GetCurrentProcessId();
    msg.allowResampling = allow ? 1 : 0;
    bool ok = ipc::WriteMessage(pipe_, msg);
    if (ok) {
        std::lock_guard sLock(stateMutex_);
        currentStatus_.allowResampling = allow;
    }
    return ok;
}

bool AmPluginClient::SetOutputTarget(uint8_t kind, const std::wstring& id, const std::wstring& name, const std::wstring& location) {
    std::lock_guard lock(pipeMutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    auto msg = ipc::NewMessage(ipc::MessageType::SetOutputTarget, ipc::Role::Controller);
    msg.senderPid = GetCurrentProcessId();
    msg.enabledIntent = kind; // 0=WASAPI, 1=DLNA, 2=ASIO, 3=Roon RAAT

    if (kind == 1 || kind == 3) { // DLNA or Roon RAAT
        wcsncpy_s(msg.endpointId, id.c_str(), _TRUNCATE);
        wcsncpy_s(msg.endpointName, name.c_str(), _TRUNCATE);
        wcsncpy_s(msg.detail, location.c_str(), _TRUNCATE);
    } else { // WASAPI or ASIO
        wcsncpy_s(msg.format, id.c_str(), _TRUNCATE);
        wcsncpy_s(msg.endpointName, name.c_str(), _TRUNCATE);
    }

    return ipc::WriteMessage(pipe_, msg);
}

void AmPluginClient::RefreshDevices() {
    // Reload local devices
    {
        std::lock_guard lock(stateMutex_);
        // Retain online DLNA devices
        std::vector<DeviceInfo> dlnaDevices;
        for (const auto& d : devices_) {
            if (d.type == "dlna") dlnaDevices.push_back(d);
        }
        devices_.clear();
        const auto wasapi = local_devices::EnumerateWasapi();
        for (const auto& d : wasapi) {
            devices_.push_back({ Narrow(d.id), Narrow(d.name), "wasapi", true, "" });
        }
        const auto asio = local_devices::EnumerateAsio();
        for (const auto& d : asio) {
            devices_.push_back({ Narrow(d.id), Narrow(d.name), "asio", d.usable, Narrow(d.note) });
        }
        for (const auto& d : dlnaDevices) {
            devices_.push_back(d);
        }
    }

    // Request DLNA refresh from Broker
    std::lock_guard lock(pipeMutex_);
    if (pipe_ != INVALID_HANDLE_VALUE) {
        auto msg = ipc::NewMessage(ipc::MessageType::RefreshDevices, ipc::Role::Controller);
        msg.senderPid = GetCurrentProcessId();
        (void)ipc::WriteMessage(pipe_, msg);
    }
}

} // namespace ammod::remote
