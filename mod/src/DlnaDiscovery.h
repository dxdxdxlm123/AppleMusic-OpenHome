#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ammod::dlna {

struct DiscoveredDevice final {
    std::wstring udn;
    std::wstring friendlyName;
    std::wstring location;
    bool openHome{};
    bool avTransport{};
    bool online{};
};

// SSDP discovery runs in the broker process: it is long-lived, unpackaged,
// and already owns persisted user state. It never touches Apple's processes.
class DlnaDiscovery final {
public:
    using Callback = std::function<void(const DiscoveredDevice& device)>;
    using LogCallback = std::function<void(const std::wstring& text)>;

    DlnaDiscovery() = default;
    ~DlnaDiscovery();

    DlnaDiscovery(const DlnaDiscovery&) = delete;
    DlnaDiscovery& operator=(const DlnaDiscovery&) = delete;

    bool Start(Callback callback, LogCallback log, std::wstring& error) noexcept;
    void Stop() noexcept;
    void Refresh() noexcept;
    std::vector<DiscoveredDevice> Snapshot() const;

    // Configures the periodic SSDP scanning interval. Default is 30s.
    // When output is RAAT (idle), this can be lengthened (e.g. 300s).
    void SetScanInterval(std::chrono::seconds interval) noexcept;
    std::chrono::seconds ScanInterval() const noexcept;

    // Pauses or resumes periodic SSDP scanning and device XML fetching.
    // When paused (e.g. during active RAAT playback), periodic scans and
    // XML fetches are completely suppressed to avoid control plane load on renderers.
    // Explicit manual Refresh() calls are still honored.
    void SetPaused(bool paused) noexcept;
    bool IsPaused() const noexcept;

    // Adds a device from "http://host:port/device.xml", "http://host:port/"
    // or "host:port". Fails closed unless the description proves a renderer
    // (AVTransport or OpenHome Playlist).
    bool AddManualDevice(const std::wstring& input, std::wstring& error) noexcept;

private:
    void ThreadMain() noexcept;
    void ScanOnce() noexcept;
    bool FetchDescription(const std::wstring& location, DiscoveredDevice& device) noexcept;
    void Publish(const DiscoveredDevice& device) noexcept;
    void LogLine(const std::wstring& text) const;

    Callback callback_;
    LogCallback log_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> refresh_{false};
    std::atomic<bool> paused_{false};
    std::atomic<std::uint32_t> scanIntervalSec_{30};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::unordered_map<std::wstring, DiscoveredDevice> devices_;
    std::unordered_map<std::wstring, std::uint64_t> lastSeenTick_;
    std::unordered_map<std::wstring, std::uint64_t> nonRendererCache_;
};

} // namespace ammod::dlna
