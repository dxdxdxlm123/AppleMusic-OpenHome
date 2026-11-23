#pragma once

#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ammod::raat {

std::string ToUtf8(const std::wstring& value);
std::wstring ToWide(const std::string& value);

std::string JsonEscape(const std::string& value);
bool JsonFindString(const std::string& json, const std::string& key, std::string& out);
std::string JsonGetString(const std::string& json, const std::string& key);
bool JsonFindInt(const std::string& json, const std::string& key, std::int64_t& out);
std::int64_t JsonGetInt(const std::string& json, const std::string& key,
                        std::int64_t fallback = 0);
bool JsonFindDouble(const std::string& json, const std::string& key, double& out);
double JsonGetDouble(const std::string& json, const std::string& key,
                     double fallback = 0.0);
bool JsonFindBool(const std::string& json, const std::string& key, bool& out);
bool JsonGetBool(const std::string& json, const std::string& key,
                 bool fallback = false);

std::uint64_t NowMicroseconds() noexcept;
// The renderer's clock domain is nanoseconds: RAATCLKR carries
// get_local_time() values and req_start adds min_offset to the same
// unit. Keep this separate from the discovery/telemetry timestamps.
std::uint64_t NowNanoseconds() noexcept;
std::wstring NewUuidString();

// Process-wide WSAStartup (idempotent, never calls WSACleanup). The hook
// process may not have initialized Winsock before RAAT is selected.
bool EnsureWinsock() noexcept;

// TCP helper: connect with a bounded timeout, optional local interface bind,
// graceful SD_BOTH shutdown, and last Winsock error reporting.
class TcpConnection final {
public:
    TcpConnection() = default;
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    bool Connect(const std::wstring& address, std::uint16_t port,
                 std::uint32_t timeoutMs, std::wstring& error,
                 const std::wstring& bindAddress = {}) noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept { return socket_ != INVALID_SOCKET; }

    bool SendAll(const void* data, std::size_t size,
                 std::uint32_t timeoutMs) noexcept;
    bool ReadSome(void* data, std::size_t capacity, std::size_t& received,
                  std::uint32_t timeoutMs) noexcept;

    SOCKET Handle() const noexcept { return socket_; }
    std::wstring LocalAddress() const noexcept;
    int LastWsaError() const noexcept { return lastWsaError_; }

private:
    SOCKET socket_{INVALID_SOCKET};
    int lastWsaError_{0};
};

// Official Roon LL timeouts and keepalive cadence (Roon.Audio.Raat.Client).
inline constexpr std::uint32_t kControlReconnectTimeoutMs = 1500;
inline constexpr std::uint32_t kLlDefaultKeepaliveMs = 2000;
inline constexpr std::uint32_t kLlDefaultKeepaliveTimeoutMs = 10000;
inline constexpr std::uint32_t kLlMaxMessageLen = 4u * 1024u * 1024u;

// LL frame types (bit 31 = 0x80000000 selects native C-layer vs Lua dispatcher):
//   0x80000001  LL_REQUEST          (load_script, get_log, set_log_level, force_exit)
//   0x80000002  LL_RESPONSE         (C-layer reply)
//   0x80000003  LL_KEEPALIVE        (0-byte binary keepalive every 2000ms)
//   0x80000004  LL_BEGIN_KEEPALIVE  (initial keepalive negotiation / hello frame)
//   0x80000005  LL_END_KEEPALIVE
//   0x00000001  REQUEST             (Lua-dispatched JSON RPC)
//   0x00000002  RESPONSE            (Lua reply, final=0 for async pushes)
//   0x00000003  ARTWORK             (raw binary image blob before update_artwork)
inline constexpr std::uint32_t kLlTypeFlag = 0x80000000u;
inline constexpr std::uint32_t kLlTypeRequest = 0x00000001u;
inline constexpr std::uint32_t kLlTypeRequestInternal = 0x80000001u;
inline constexpr std::uint32_t kLlTypeResponse = 0x00000002u;
inline constexpr std::uint32_t kLlTypeResponseInternal = 0x80000002u;
inline constexpr std::uint32_t kLlTypeArtwork = 0x00000003u;
inline constexpr std::uint32_t kLlTypeKeepalive = 0x80000003u;
inline constexpr std::uint32_t kLlTypeHello = 0x80000004u;
inline constexpr std::uint32_t kLlTypeBeginKeepalive = 0x80000004u;
inline constexpr std::uint32_t kLlTypeEndKeepalive = 0x80000005u;

// True for requests the renderer's C layer answers itself.
bool IsInternalRequest(const std::string& json) noexcept;

// Official-parity RAAT LL control channel with:
// - Background async reader thread dispatching frames by rid
// - Native binary 0x80000003 LL_KEEPALIVE (2s interval, 10s inbound watchdog)
// - Async event callback for non-final pushes (Dropout, OutputMessage, Lost, Teardown)
// - No silent reconnect that bypasses Lua script injection
class ControlChannel final {
public:
    using EventCallback = std::function<void(std::uint32_t rid,
                                             const std::string& json,
                                             bool isFinal)>;

    ControlChannel() = default;
    ~ControlChannel();

    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    bool Connect(const std::wstring& address, std::uint16_t port,
                 std::uint32_t timeoutMs, std::wstring& error,
                 const std::wstring& bindAddress = {}) noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept {
        return open_.load(std::memory_order_acquire) && connection_.IsOpen();
    }
    std::wstring LocalAddress() const noexcept { return connection_.LocalAddress(); }

    void SetEventCallback(EventCallback callback) noexcept;

    // Sends one request and waits for the first response carrying the same rid.
    bool Request(const std::string& json, std::uint32_t timeoutMs,
                 std::string& response, bool& final, std::wstring& error) noexcept;

    // Fire-and-forget request (returns the assigned rid so callers can watch
    // async responses on another rid such as setup_rid for teardown).
    bool SendAsync(const std::string& json, std::uint32_t timeoutMs,
                   std::uint32_t& outRid) noexcept;

    // Sends a raw binary artwork frame (0x00000003) before update_artwork.
    bool SendArtwork(const void* data, std::size_t size,
                     std::uint32_t timeoutMs) noexcept;

    // Fire-and-forget probe (kept for test compatibility).
    bool SendProbe(const std::string& json, std::uint32_t timeoutMs) noexcept;

    // Kept for tool/test compatibility.
    bool WaitForReady(std::uint32_t timeoutMs, std::wstring& error) noexcept;

    std::uint32_t LastRequestRid() const noexcept {
        return lastRid_.load(std::memory_order_acquire);
    }

    // Reads the next queued frame with `expectedRid` (for multi-frame replies
    // such as setup OutputMessage -> port frame or stream Buffering -> Ready).
    bool ReadFrame(std::uint32_t expectedRid, std::uint32_t timeoutMs,
                   std::string& response, bool& final, std::wstring& error) noexcept;

private:
    struct QueuedFrame {
        std::string json;
        bool isFinal{false};
    };

    bool SendHello(std::wstring& error) noexcept;
    bool SendKeepaliveFrame() noexcept;
    bool SendFrame(std::uint32_t rid, const std::string& json,
                   std::uint32_t timeoutMs) noexcept;
    void ReaderMain() noexcept;
    void KeepaliveMain() noexcept;
    void MarkDisconnectedLocked(const std::wstring& reason) noexcept;

    TcpConnection connection_;
    std::mutex sendMutex_;
    std::mutex stateMutex_;
    std::condition_variable frameCv_;
    std::unordered_map<std::uint32_t, std::deque<QueuedFrame>> queues_;
    std::wstring disconnectReason_;
    EventCallback eventCallback_;

    std::thread readerThread_;
    std::thread keepaliveThread_;
    std::atomic<bool> stopThreads_{false};
    std::atomic<bool> open_{false};
    std::atomic<std::uint64_t> lastInboundMs_{0};

    std::atomic<std::uint32_t> nextRid_{1};
    std::atomic<std::uint32_t> lastRid_{0};
};

// Minimal RoonBridge/RAATServer "signal band" wake: TCP 9200 carries line
// delimited JSON.
bool WakeDevice(const std::wstring& address, const std::wstring& deviceId,
                std::uint16_t& controlPort, std::wstring& error,
                const std::wstring& bindAddress = {}) noexcept;

} // namespace ammod::raat
