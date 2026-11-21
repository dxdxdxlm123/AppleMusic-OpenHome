#include "RaatSink.h"

#include "DlnaLog.h"
#include "PcmTypes.h"
#include "RaatDiscovery.h"
#include "../AudioErrors.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <thread>

namespace ammod::audio_v2 {
namespace {

using namespace ammod::raat;

constexpr std::uint32_t kRaatDefaultLookaheadMs = 1000;
constexpr std::uint32_t kRaatDefaultStartLeadMs = 300;
constexpr std::uint32_t kRaatConnectTimeoutMs = 2500;
constexpr std::uint32_t kRaatRequestTimeoutMs = 5000;

std::wstring IniPath() {
    wchar_t publicDir[MAX_PATH]{};
    GetEnvironmentVariableW(L"PUBLIC", publicDir,
                            static_cast<DWORD>(std::size(publicDir)));
    if (!publicDir[0]) return {};
    return std::filesystem::path(publicDir) / L"AMExclusive" / L"am-exclusive.ini";
}

void LogRaat(const std::wstring& text) {
    LogDlnaLine(L"raat " + text);
}

std::string JsonStatus(const std::string& response) {
    return JsonGetString(response, "status");
}

void AppendBe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24u));
    out.push_back(static_cast<std::uint8_t>(value >> 16u));
    out.push_back(static_cast<std::uint8_t>(value >> 8u));
    out.push_back(static_cast<std::uint8_t>(value));
}

void WriteBe32(std::uint8_t* data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 24u);
    data[1] = static_cast<std::uint8_t>(value >> 16u);
    data[2] = static_cast<std::uint8_t>(value >> 8u);
    data[3] = static_cast<std::uint8_t>(value);
}

void AppendBe64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint32_t ReadBe32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24u) |
           (static_cast<std::uint32_t>(data[1]) << 16u) |
           (static_cast<std::uint32_t>(data[2]) << 8u) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t ReadBe64(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint64_t>(data[0]) << 56u) |
           (static_cast<std::uint64_t>(data[1]) << 48u) |
           (static_cast<std::uint64_t>(data[2]) << 40u) |
           (static_cast<std::uint64_t>(data[3]) << 32u) |
           (static_cast<std::uint64_t>(data[4]) << 24u) |
           (static_cast<std::uint64_t>(data[5]) << 16u) |
           (static_cast<std::uint64_t>(data[6]) << 8u) |
           static_cast<std::uint64_t>(data[7]);
}

std::string SourceBitsText(std::uint16_t bits) {
    return std::to_string(bits);
}

bool ContainsFormatToken(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

RaatSink::~RaatSink() { Close(); }

HRESULT RaatSink::Open(const AudioSinkOpenParams& params) noexcept {
    Close();
    if (!IsValidFormat(params.format)) return E_INVALIDARG;
    if (params.raat.deviceId.empty() && params.raat.address.empty()) {
        return ammod::audio::kRaatDeviceUnavailable;
    }

    params_ = params;
    format_ = params.format;
    target_ = params.raat;
    source_ = nullptr;
    sampleIndex_ = 0;
    lastSendTick_ = 0;

    const std::uint16_t sourceBits = EffectiveSourceValidBits(format_);
    int configuredSoodPort = 0;
    ReadSettingInt(configuredSoodPort, L"sood_port", 0);
    SetSoodPort(configuredSoodPort > 0 && configuredSoodPort <= 65535
                    ? static_cast<std::uint16_t>(configuredSoodPort) : 9003);
    int configuredBits = 0;
    ReadSettingInt(configuredBits, L"wire_bits", 0);
    std::uint16_t wire = sourceBits;
    if (configuredBits == 16 || configuredBits == 24 || configuredBits == 32) {
        if (static_cast<std::uint16_t>(configuredBits) >= sourceBits) {
            wire = static_cast<std::uint16_t>(configuredBits);
        }
    }
    wireBits_ = wire;
    format_.validBits = wire;
    format_.containerBits = wire;

    lookaheadMs_ = params.streamBufferMs;
    if (lookaheadMs_ == 0) {
        int value = 0;
        ReadSettingInt(value, L"buffer_ms",
                       static_cast<int>(kRaatDefaultLookaheadMs));
        lookaheadMs_ = static_cast<std::uint32_t>(value);
    }
    lookaheadMs_ = std::clamp<std::uint32_t>(lookaheadMs_, 100u, 5000u);

    int lead = 0;
    ReadSettingInt(lead, L"start_lead_ms", static_cast<int>(kRaatDefaultStartLeadMs));
    startLeadMs_ = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(lead, 300)), 300u, 4000u);

    // 2048 frames (42.7 ms at 48 kHz) is the largest block the capture queue
    // hands out and is close to the renderer's own 50 ms output period.
    blockFrames_ = std::min<std::uint32_t>(kMaxBlockFrames, 2048u);
    block_.assign(static_cast<std::size_t>(blockFrames_) * format_.channels, 0);
    pcm_.resize(static_cast<std::size_t>(blockFrames_) * format_.channels * 4u);
    frame_.reserve(pcm_.size() + 16u);

    streamId_ = static_cast<std::uint32_t>((GetTickCount64() & 0x7fffffffu) | 1u);
    lastError_.store(S_OK, std::memory_order_release);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    LogRaat(L"open device=" + target_.friendlyName + L" id=" + target_.deviceId +
            L" addr=" + target_.address + L":" + std::to_wstring(target_.controlPort) +
            L" raat=" + target_.raatVersion + L" rate=" +
            std::to_wstring(format_.sampleRate) + L" bits=" +
            std::to_wstring(wireBits_) + L" ch=" + std::to_wstring(format_.channels) +
            L" lookahead=" + std::to_wstring(lookaheadMs_) + L"ms lead=" +
            std::to_wstring(startLeadMs_) + L"ms");
    return S_OK;
}

HRESULT RaatSink::Start(IIntegerPcmSource& source) noexcept {
    if (state_.load(std::memory_order_acquire) == AudioSinkState::Closed ||
        state_.load(std::memory_order_acquire) == AudioSinkState::Faulted) {
        return E_UNEXPECTED;
    }
    source_ = &source;
    endOfStreamSubmitted_.store(false, std::memory_order_release);
    endOfStreamComplete_.store(false, std::memory_order_release);

    if (startedOnce_.load(std::memory_order_acquire) &&
        control_.IsOpen() && audio_.IsOpen()) {
        paused_.store(false, std::memory_order_release);
        paceStartTick_ = 0;
        if (!SendStart(true)) {
            RecordError(ammod::audio::kRaatControlFailed);
            return ammod::audio::kRaatControlFailed;
        }
        state_.store(AudioSinkState::Running, std::memory_order_release);
        LogRaat(L"resume sample=" + std::to_wstring(sampleIndex_));
        return S_OK;
    }

    stop_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    startedOnce_.store(false, std::memory_order_release);
    sampleIndex_ = 0;
    paceStartTick_ = 0;
    paceStartSample_ = 0;
    paceLastLogTick_ = 0;
    lastError_.store(S_OK, std::memory_order_release);

    if (!EnsureSession()) {
        const HRESULT error = lastError_.load(std::memory_order_acquire);
        RecordError(error == S_OK ? ammod::audio::kRaatStreamFailed : error);
        StopKeepalive();
        StopClock();
        control_.Close();
        audio_.Close();
        state_.store(AudioSinkState::Faulted, std::memory_order_release);
        return error == S_OK ? ammod::audio::kRaatStreamFailed : error;
    }

    StopKeepalive();
    keepaliveStop_.store(false, std::memory_order_release);
    try {
        keepaliveThread_ = std::thread(&RaatSink::KeepaliveMain, this);
    } catch (...) {
    }

    try {
        pumpThread_ = std::thread(&RaatSink::PumpMain, this);
    } catch (...) {
        RecordError(ammod::audio::kRaatStreamFailed);
        StopKeepalive();
        StopClock();
        control_.Close();
        audio_.Close();
        return ammod::audio::kRaatStreamFailed;
    }
    startedOnce_.store(true, std::memory_order_release);
    state_.store(AudioSinkState::Running, std::memory_order_release);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
    LogRaat(L"session live " + target_.address + L":" +
            std::to_wstring(target_.controlPort));
    return S_OK;
}

void RaatSink::Pause() noexcept {
    if (state_.load(std::memory_order_acquire) != AudioSinkState::Running) return;
    paused_.store(true, std::memory_order_release);
    SetThreadExecutionState(ES_CONTINUOUS);
    if (control_.IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_.Request("{\"request\":\"stop\"}", 1500, response, final, error);
    }
    state_.store(AudioSinkState::Open, std::memory_order_release);
    LogRaat(L"pause");
}

void RaatSink::Stop() noexcept {
    stop_.store(true, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    SetThreadExecutionState(ES_CONTINUOUS);

    // 1. Join pump thread first so no more audio chunks are written
    if (pumpThread_.joinable()) pumpThread_.join();

    // 2. Send stop / end_stream on control TCP BEFORE touching audio socket!
    // This allows ALSA on Linux RT kernel (e.g. VEGA G2) to transition cleanly
    // to STOPPED/SETUP via snd_pcm_drop/drain without hanging hw:0,0.
    if (control_.IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_.Request("{\"request\":\"stop\"}", 1000, response, final, error);
        (void)control_.Request("{\"request\":\"end_stream\"}", 1000, response, final, error);
    }

    // 3. Grace period for ALSA DMA drain
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 4. Gracefully close audio TCP connection
    {
        std::lock_guard lock(audioMutex_);
        audio_.Close();
    }

    StopKeepalive();
    StopClock();
    control_.Close();
    startedOnce_.store(false, std::memory_order_release);
    sampleIndex_ = 0;
    if (state_.load(std::memory_order_acquire) != AudioSinkState::Faulted) {
        state_.store(AudioSinkState::Open, std::memory_order_release);
    }
    LogRaat(L"stop");
}

void RaatSink::Close() noexcept {
    stop_.store(true, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    if (pumpThread_.joinable()) pumpThread_.join();

    if (control_.IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_.Request("{\"request\":\"stop\"}", 1000, response, final, error);
        (void)control_.Request("{\"request\":\"end_stream\"}", 1000, response, final, error);
        (void)control_.Request("{\"request\":\"teardown\"}", 1500, response, final, error);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard lock(audioMutex_);
        audio_.Close();
    }
    StopKeepalive();
    StopClock();
    control_.Close();
    startedOnce_.store(false, std::memory_order_release);
    sampleIndex_ = 0;
    source_ = nullptr;
    SetThreadExecutionState(ES_CONTINUOUS);
    state_.store(AudioSinkState::Closed, std::memory_order_release);
    LogRaat(L"close");
}

bool RaatSink::EnsureSession() noexcept {
    std::wstring error;
    if (!EnsureWinsock()) {
        error = L"WSAStartup failed";
        lastError_.store(ammod::audio::kRaatControlFailed, std::memory_order_release);
        return false;
    }

    // The control port MOVES whenever the renderer restarts or re-enables its
    // RAAT output (35649 -> 43795 -> 38313 was observed on the same VEGA). A
    // cached ini port is therefore stale far more often than an address is.
    // Always refresh from SOOD before dialling; a failed query keeps the
    // cached value so an offline discovery service cannot block playback.
    {
        RaatDevice fresh;
        std::wstring queryError;
        if (RaatDiscovery::QueryOnce(target_.deviceId, 1500, fresh, queryError,
                                     target_.address) &&
            fresh.controlPort != 0) {
            if (fresh.controlPort != target_.controlPort) {
                LogRaat(L"sood port refresh " + std::to_wstring(target_.controlPort) +
                        L" -> " + std::to_wstring(fresh.controlPort));
            }
            target_.controlPort = fresh.controlPort;
            if (!fresh.address.empty()) target_.address = fresh.address;
            if (!fresh.raatVersion.empty()) target_.raatVersion = fresh.raatVersion;
            if (target_.friendlyName.empty()) target_.friendlyName = fresh.friendlyName;
        }
    }

    if (!OpenControl(error)) {
        LogRaat(L"control open failed: " + error);
        lastError_.store(target_.address.empty() ? ammod::audio::kRaatDeviceUnavailable
                                                 : ammod::audio::kRaatControlFailed,
                         std::memory_order_release);
        return false;
    }
    // Connect() already sent the "hi" frame. The captured working session then
    // repeats echo/info/info before injecting, so do the same. These are
    // Lua-dispatched requests, so before load_script they answer
    // NotImplemented; nothing waits for the replies.
    //
    // The old code also blocked here for the device's type-3 "ready" event
    // (~1.8 s after hi) because injecting earlier appeared to leave the
    // dispatcher uninstalled. Direct testing shows load_script succeeds and
    // the dispatcher runs whether or not that event was awaited, so the wait
    // is gone: it only delayed every session start by up to 3 s.
    {
        std::lock_guard lock(controlMutex_);
        control_.SendProbe("{\"request\":\"echo\"}", 1500);
        control_.SendProbe("{\"request\":\"info\"}", 1500);
        control_.SendProbe("{\"request\":\"info\"}", 1500);
        LogRaat(L"injection session greeted (hi + echo/info/info)");
    }
    if (!LoadScripts(error)) {
        LogRaat(L"scripts failed: " + error);
        lastError_.store(ammod::audio::kRaatScriptRejected, std::memory_order_release);
        return false;
    }

    // Official Roon control handshake alignment:
    // 1. info
    SendInfo(error);
    // 2. set_client_type: Roon
    SendClientType(error);
    // 3. subscribe_transport & subscribe_controls
    SendSubscriptions(error);
    // 4. request_source (switches DAC input to RAAT streaming)
    SendRequestSource();

    // 5. setup (format negotiation, returns audio_port_tcp & clock_port)
    if (!SendSetup(error)) {
        LogRaat(L"setup failed: " + error);
        return false;
    }
    localAddress_ = control_.LocalAddress();

    // 6. Setup local UDP socket for Core-side clock telemetry & drift monitoring
    clockPort_ = 0;
    clockProbesIn_.store(0, std::memory_order_relaxed);
    clockRepliesOut_.store(0, std::memory_order_relaxed);
    clockRepliesIn_.store(0, std::memory_order_relaxed);
    clockStop_.store(false, std::memory_order_release);
    if (clockSocket_ == INVALID_SOCKET) {
        clockSocket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (clockSocket_ != INVALID_SOCKET) {
            DWORD timeoutMs = 200;
            setsockopt(clockSocket_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = INADDR_ANY;
            local.sin_port = 0;
            if (bind(clockSocket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0) {
                sockaddr_in bound{};
                int length = sizeof(bound);
                if (getsockname(clockSocket_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
                    clockPort_ = ntohs(bound.sin_port);
                }
            }
            try {
                clockThread_ = std::thread(&RaatSink::ClockMain, this);
            } catch (...) {
                clockStop_.store(true, std::memory_order_release);
            }
        }
    }
    LogRaat(L"clock telemetry local udp/" + std::to_wstring(clockPort_) +
            L" device clock_port=" + std::to_wstring(deviceClockPort_));

    // Measure initial endpoint clock offset via Core-initiated RAATCLKQ
    QueryDeviceClock();

    // 7. stream request (buffer_size 5.0s, ready_threshold 0.45)
    if (!SendStream(error)) {
        LogRaat(L"stream failed: " + error);
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }

    // 8. Connect audio TCP channel
    if (!OpenAudio(error)) {
        LogRaat(L"audio connect failed: " + error);
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }

    // 9. Pre-buffer audio to satisfy ready_threshold (2.25s of audio)
    if (!Prefill()) {
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }

    // 10. Wait for {"status":"Ready"} notification from device
    std::wstring readyError;
    if (!WaitForReady(2000, readyError)) {
        LogRaat(L"ready wait: " + readyError + L" (proceeding with start)");
    } else {
        LogRaat(L"device confirmed Ready");
    }

    // 11. Start playback. Note: NO {"request":"sync"} is sent to device!
    // VEGA G2 is Clock Master (ALSA hardware clock).
    if (!SendStart(false)) {
        lastError_.store(ammod::audio::kRaatControlFailed, std::memory_order_release);
        return false;
    }
    return true;
}

bool RaatSink::OpenControl(std::wstring& error) noexcept {
    std::vector<std::uint16_t> ports;
    if (target_.controlPort != 0) ports.push_back(target_.controlPort);

    const auto tryConnect = [&](std::uint16_t port, std::wstring& failure) -> bool {
        if (port == 0) return false;
        const auto started = std::chrono::steady_clock::now();
        if (!control_.Connect(target_.address, port, kRaatConnectTimeoutMs, failure)) {
            // Name the port: a stale cached port is the most common cause and
            // the log is the only place it can be seen.
            LogRaat(L"control connect " + target_.address + L":" +
                    std::to_wstring(port) + L" failed: " + failure);
            return false;
        }
        std::lock_guard lock(controlMutex_);
        // The hi frame (sent by Connect) is the whole greeting this firmware
        // needs. echo/info sent here are Lua-dispatched requests, so before
        // load_script they can only answer NotImplemented; they are kept
        // because the reference sender sends them and some firmwares use them
        // as a liveness check, but their replies are never waited for.
        control_.SendProbe("{\"request\":\"echo\"}", 1500);
        control_.SendProbe("{\"request\":\"info\"}", 1500);
        control_.SendProbe("{\"request\":\"info\"}", 1500);
        target_.controlPort = port;
        LogRaat(L"control hello sent on :" + std::to_wstring(port) +
                L" in " + std::to_wstring(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count()) + L"ms");
        return true;
    };

    // Historically this opened a throwaway "probe" connection, waited for the
    // device's type-3 ready event, closed it, slept, and injected on a second
    // connection - a shape inferred from a captured foobar2000 session. Direct
    // testing against the VEGA G2.2 showed the shape is irrelevant: a single
    // connection that sends hi and then load_script immediately works, and the
    // "NotImplemented" that motivated the dance was caused by the request
    // frame type, not by connection reuse (see kLlTypeRequest in
    // RaatProtocol.h). The probe/reconnect round trip only added ~4 s to every
    // session start, so it is gone.
    for (std::uint16_t port : ports) {
        std::wstring failure;
        if (tryConnect(port, failure)) {
            LogRaat(L"control port " + std::to_wstring(port) + L" verified");
            return true;
        }
    }

    // Direct connect failed: ask the 9200 signal band to enable the output.
    std::uint16_t wakePort = 0;
    std::wstring wakeError;
    if (!target_.address.empty() &&
        WakeDevice(target_.address, target_.deviceId, wakePort, wakeError)) {
        LogRaat(L"wake reported fresh tcp_port " + std::to_wstring(wakePort));
        std::wstring failure;
        if (tryConnect(wakePort, failure)) return true;
        error = failure;
    } else if (!wakeError.empty()) {
        LogRaat(L"wake failed: " + wakeError);
    }

    // Last resort: re-query SOOD for a moved port/address.
    RaatDevice device;
    std::wstring queryError;
    if (RaatDiscovery::QueryOnce(target_.deviceId, 1500, device, queryError)) {
        target_.address = device.address;
        target_.raatVersion = device.raatVersion;
        target_.friendlyName = device.friendlyName;
        std::wstring failure;
        if (tryConnect(device.controlPort, failure)) {
            LogRaat(L"SOOD re-query rescued port " + std::to_wstring(device.controlPort));
            return true;
        }
        error = failure;
    } else if (!queryError.empty()) {
        LogRaat(L"SOOD re-query failed: " + queryError);
    }
    if (error.empty()) error = wakeError.empty() ? L"no candidate ports" : wakeError;
    LogRaat(L"control open gave up: " + error);
    return false;
}

int RaatSink::GetPreferredVariant(const std::wstring& endpoint) noexcept {
    if (endpoint.empty()) return 0;
    std::lock_guard lock(s_variantMutex);
    const auto it = s_preferredVariants.find(endpoint);
    return it != s_preferredVariants.end() ? it->second : 0;
}

void RaatSink::SetPreferredVariant(const std::wstring& endpoint, int variant) noexcept {
    if (endpoint.empty()) return;
    std::lock_guard lock(s_variantMutex);
    s_preferredVariants[endpoint] = variant;
}

bool RaatSink::LoadScripts(std::wstring& error) noexcept {
    const bool newGen = RaatVersionIsNewGen(target_.raatVersion);
    if (newGen) LogRaat(L"newgen endpoint: replaying official signed corpus");
    else LogRaat(L"legacy endpoint: signed corpus first, clean-room only as fallback");

    const auto runCorpus = [&](const ScriptRecord* records, std::size_t count,
                               bool official) -> bool {
        (void)official;
        // Inject the whole bundle with ONE wire variant, then verify that the
        // Lua dispatcher really came up. Some firmwares answer every
        // load_script with Success but refuse to install the scripts; echo
        // (and later setup) then return {"status":"NotImplemented"}. Mixing
        // variants across scripts would leave a half-installed dispatcher.
        const int preferred = GetPreferredVariant(target_.address);
        std::vector<int> variants;
        variants.push_back(preferred);
        for (int v = 0; v < 3; ++v) {
            if (v != preferred) variants.push_back(v);
        }
        for (int variant : variants) {
            bool allLoaded = true;
            for (std::size_t index = 0; index < count; ++index) {
                const auto& record = records[index];
                std::wstring lastError;
                if (!LoadOneScript(record, variant, lastError)) {
                    error = std::wstring(L"load ") + ToWide(record.name) +
                        L" failed: " + lastError;
                    allLoaded = false;
                    break;
                }
                LogRaat(std::wstring(L"load ") + ToWide(record.name) + L" ok" +
                        (official ? L" (signed)" : L""));
            }
            if (!allLoaded) {
                error.clear();
                continue;
            }
            LogRaat(L"corpus variant " + std::to_wstring(variant) + L" injected");
            // The captured working sender waited ~200 ms between the last
            // load_script and its echo canary while the firmware installed
            // the dispatcher.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::string response;
            bool final = false;
            std::wstring canaryError;
            bool ready = false;
            {
                std::lock_guard lock(controlMutex_);
                if (control_.Request("{\"request\":\"echo\"}", 800,
                                     response, final, canaryError) &&
                    JsonStatus(response) == "Success") {
                    ready = true;
                }
            }
            if (ready) {
                SetPreferredVariant(target_.address, variant);
                error.clear();
                LogRaat(L"dispatcher ready (echo canary, variant " +
                        std::to_wstring(variant) + L")");
                return true;
            }
            LogRaat(L"dispatcher not ready after variant " +
                    std::to_wstring(variant) + L" (" +
                    (canaryError.empty() ? ToWide(JsonStatus(response)) : canaryError) +
                    L")");
        }
        return false;
    };

    // A firmware that verifies signatures needs the VERBATIM corpus: field
    // order, "keypair":"Roon", the 64-byte signature and "module" all matter.
    // Replay each jsonl record byte-for-byte instead of rebuilding the JSON
    // from our own ScriptRecord table (which omits keypair).
    const auto replaySignedCorpus = [&](const std::wstring& path) -> bool {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        if (!input) return false;
        int injected = 0;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.find("\"request\"") == std::string::npos) continue;
            std::string response;
            bool final = false;
            std::wstring requestError;
            {
                std::lock_guard lock(controlMutex_);
                if (!control_.Request(line, 8000, response, final, requestError)) {
                    LogRaat(L"signed corpus replay failed: " + requestError);
                    return false;
                }
            }
            const std::string status = JsonStatus(response);
            if (!status.empty() && status != "Success") {
                LogRaat(L"signed corpus record refused: " + ToWide(status));
                return false;
            }
            ++injected;
        }
        if (injected == 0) return false;
        LogRaat(L"signed corpus replayed " + std::to_wstring(injected) +
                L" records from " + path);
        for (int attempt = 0; attempt < 6; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::string response;
            bool final = false;
            std::wstring canaryError;
            bool ready = false;
            {
                std::lock_guard lock(controlMutex_);
                if (control_.Request("{\"request\":\"echo\"}", 800,
                                     response, final, canaryError) &&
                    JsonStatus(response) == "Success") {
                    ready = true;
                }
            }
            if (ready) {
                error.clear();
                LogRaat(L"dispatcher ready after signed corpus replay");
                return true;
            }
        }
        LogRaat(L"dispatcher not ready after signed corpus replay");
        return false;
    };

    // Preferred path: replay the EXACT load_script requests captured from the
    // working session (tools/raat_signed_corpus.jsonl, installed as
    // raat_signed_corpus.jsonl). Byte-for-byte identical to what this firmware
    // demonstrably accepts.
    // Override with the RAAT_CORPUS environment variable when debugging.
    {
        std::wstring corpusPath;
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"RAAT_CORPUS", env, MAX_PATH) > 0) {
            corpusPath = env;
        } else {
            const auto iniDir = std::filesystem::path(IniPath()).parent_path();
            if (std::filesystem::exists(iniDir / L"raat_signed_corpus.jsonl")) {
                corpusPath = (iniDir / L"raat_signed_corpus.jsonl").wstring();
            } else if (std::filesystem::exists(iniDir / L"tools" / L"raat_signed_corpus.jsonl")) {
                corpusPath = (iniDir / L"tools" / L"raat_signed_corpus.jsonl").wstring();
            }
        }
        if (!corpusPath.empty() && replaySignedCorpus(corpusPath)) return true;
    }

    // Fallback: embedded OFFICIAL scripts with keypair:"Roon" and baked signatures.
    std::size_t count = 0;
    const ScriptRecord* records = OfficialScripts(count);
    if (runCorpus(records, count, true)) return true;

    // AURALiC VEGA G2 / G2.2 must NEVER fall back to clean-room scripts:
    // clean-room scripts lack hardware hooks (e.g. ROON_DISPLAY_PORT) and cause
    // ALSA RT kernel hangs / device reboots.
    const bool isVegaG2 = (target_.friendlyName.find(L"AURALiC") != std::wstring::npos ||
                           target_.friendlyName.find(L"VEGA") != std::wstring::npos ||
                           target_.deviceId.find(L"AURALiC") != std::wstring::npos ||
                           target_.deviceId.find(L"VEGA") != std::wstring::npos);
    if (isVegaG2) {
        LogRaat(L"VEGA G2 target detected: clean-room fallback forbidden to prevent hardware hang");
        error = L"official signed script injection failed on VEGA G2";
        return false;
    }

    LogRaat(L"official scripts refused; falling back to clean-room scripts");
    records = CleanroomScripts(count);
    if (runCorpus(records, count, false)) {
        target_.raatVersion = L"legacy";
        return true;
    }

    return false;
}

bool RaatSink::LoadOneScript(const ScriptRecord& record, int variant,
                             std::wstring& error) noexcept {
    const std::string script = LoadScriptResource(record.resourceId);
    if (script.empty()) {
        error = L"embedded resource missing";
        return false;
    }
    const std::string name = record.name ? record.name : "";
    const std::string module = record.module ? record.module : "";
    const std::string signature = record.signature ? record.signature : "";
    const std::string keypair = record.keypair ? record.keypair : "";

    std::string json = "{\"request\":\"load_script\",\"name\":\"" +
        JsonEscape(name) + "\"";
    if (variant == 0) {
        if (!module.empty()) json += ",\"module\":\"" + JsonEscape(module) + "\"";
        if (!keypair.empty()) json += ",\"keypair\":\"" + JsonEscape(keypair) + "\"";
        if (!signature.empty()) {
            json += ",\"signature\":\"" + signature + "\"";
        } else {
            json += ",\"signature\":\"0\"";
        }
        json += ",\"script\":\"" + JsonEscape(script) + "\"";
    } else if (variant == 1) {
        if (!keypair.empty()) json += ",\"keypair\":\"" + JsonEscape(keypair) + "\"";
        json += ",\"script\":\"" + JsonEscape(script) + "\",\"signature\":\"0\"";
    } else {
        if (!keypair.empty()) json += ",\"keypair\":\"" + JsonEscape(keypair) + "\"";
        json += ",\"script\":\"" + JsonEscape(script) + "\"";
    }
    json += "}";

    std::lock_guard lock(controlMutex_);
    std::string response;
    bool final = false;
    if (!control_.Request(json, kRaatRequestTimeoutMs, response, final, error)) {
        return false;
    }
    const std::string status = JsonStatus(response);
    if (status == "Success" || status.empty()) return true;
    error = L"status " + ToWide(status);
    LogRaat(L"load " + ToWide(name) + L" -> " + ToWide(status) +
            L" (variant " + std::to_wstring(variant) + L")");
    return false;
}

bool RaatSink::SendInfo(std::wstring& error) noexcept {
    std::string response;
    bool final = false;
    {
        std::lock_guard lock(controlMutex_);
        if (!control_.Request("{\"request\":\"info\"}", 800,
                              response, final, error)) {
            // Not fatal: several firmwares answer info only after playback
            // starts. setup is the authoritative format negotiation.
            LogRaat(L"info not answered yet (continuing)");
            error.clear();
            return true;
        }
    }
    const std::string status = JsonStatus(response);
    if (status != "Success") {
        // VEGA G2.2 answers info with "NotImplemented" once the clean-room
        // dispatcher is installed. The captured working foobar2000 session
        // never waits for info at all - setup is the authoritative format
        // negotiation. Do not let this advisory reply kill the session.
        LogRaat(L"info not available (" + ToWide(status) + L"); continuing");
        error.clear();
        return true;
    }
    const std::string supported = JsonGetString(response, "supported_formats");
    if (!CheckSupportedFormat(supported)) {
        error = L"device does not advertise " + std::to_wstring(format_.sampleRate) +
            L"/" + std::to_wstring(wireBits_) + L"/" + std::to_wstring(format_.channels);
        return false;
    }
    LogRaat(L"info formats=\"" + ToWide(supported) + L"\"");
    return true;
}

bool RaatSink::CheckSupportedFormat(const std::string& supported) const noexcept {
    if (supported.empty()) return true;
    if (supported.find('/') == std::string::npos) return true;
    bool sawRateBits = false;
    std::stringstream entries(supported);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        std::stringstream fields(entry);
        std::string rateText;
        std::string bitsText;
        std::string channelsText;
        if (!std::getline(fields, rateText, '/') ||
            !std::getline(fields, bitsText, '/')) {
            continue;
        }
        std::getline(fields, channelsText);
        const int rate = std::atoi(rateText.c_str());
        const int bits = std::atoi(bitsText.c_str());
        if (rate != static_cast<int>(format_.sampleRate) ||
            bits != static_cast<int>(wireBits_)) {
            continue;
        }
        sawRateBits = true;
        std::stringstream channelList(channelsText);
        std::string channel;
        while (std::getline(channelList, channel, '|')) {
            if (std::atoi(channel.c_str()) == static_cast<int>(format_.channels)) {
                return true;
            }
        }
    }
    return !sawRateBits ? true : false;
}

bool RaatSink::SendClientType(std::wstring& error) noexcept {
    std::string response;
    bool final = false;
    std::lock_guard lock(controlMutex_);
    if (!control_.Request("{\"request\":\"set_client_type\",\"client_type\":\"Roon\"}",
                          1000, response, final, error)) {
        LogRaat(L"set_client_type not answered (" + error + L"); continuing");
        error.clear();
        return true;
    }
    LogRaat(L"set_client_type -> " + ToWide(JsonStatus(response)));
    return true;
}

bool RaatSink::SendSubscriptions(std::wstring& error) noexcept {
    std::string response;
    bool final = false;
    std::lock_guard lock(controlMutex_);
    (void)control_.Request("{\"request\":\"subscribe_transport\"}", 1000, response, final, error);
    LogRaat(L"subscribe_transport -> " + ToWide(JsonStatus(response)));

    const std::string ctrlId = ToUtf8(NewUuidString());
    std::string req = "{\"request\":\"subscribe_controls\",\"controller_id\":\"" + ctrlId + "\"}";
    (void)control_.Request(req, 1000, response, final, error);
    LogRaat(L"subscribe_controls -> " + ToWide(JsonStatus(response)));
    error.clear();
    return true;
}

bool RaatSink::SendRequestSource() noexcept {
    std::string response;
    bool final = false;
    std::wstring error;
    std::lock_guard lock(controlMutex_);
    if (control_.Request("{\"request\":\"request_source\"}", 1000, response, final, error)) {
        LogRaat(L"request_source -> " + ToWide(JsonStatus(response)));
        return true;
    }
    return false;
}

bool RaatSink::SendSetup(std::wstring& error) noexcept {
    const std::uint16_t sourceBits = EffectiveSourceValidBits(format_);
    std::vector<std::uint16_t> candidates;
    const auto add = [&](std::uint16_t bits) {
        if (bits != 16 && bits != 24 && bits != 32) return;
        if (bits < sourceBits) return;
        if (std::find(candidates.begin(), candidates.end(), bits) == candidates.end()) {
            candidates.push_back(bits);
        }
    };
    add(wireBits_);
    add(24);
    add(32);
    add(16);

    for (std::uint16_t bits : candidates) {
        std::ostringstream json;
        json << "{\"request\":\"setup\",\"format\":{\"sample_type\":\"pcm\","
             << "\"sample_rate\":" << format_.sampleRate
             << ",\"bits_per_sample\":" << bits
             << ",\"channels\":" << format_.channels
             << ",\"sample_subtype\":\"none\"}}";
        std::string response;
        bool final = false;
        std::wstring requestError;
        {
            std::lock_guard lock(controlMutex_);
            if (!control_.Request(json.str(), kRaatRequestTimeoutMs,
                                  response, final, requestError)) {
                error = requestError;
                continue;
            }
            // The signed scripts answer `setup` in TWO frames with the same rid:
            // first {"status":"OutputMessage","message":{"signal_path":…}} and
            // roughly a second later the frame carrying audio_port_tcp /
            // clock_port. Request() returns the first rid match, so the ports
            // would be judged missing and setup would look like a rejection.
            // Keep reading the same rid until the frame with the ports arrives.
            if (JsonStatus(response) == "OutputMessage") {
                const std::uint32_t rid = control_.LastRequestRid();
                LogRaat(L"setup output message received; waiting for the port frame");
                for (int attempt = 0; attempt < 4; ++attempt) {
                    std::string follow;
                    bool followFinal = false;
                    std::wstring followError;
                    if (!control_.ReadFrame(rid, 1500, follow, followFinal, followError)) {
                        LogRaat(L"setup port frame wait failed: " + followError);
                        break;
                    }
                    response = follow;
                    if (JsonGetInt(response, "audio_port_tcp", 0) > 0 ||
                        JsonGetInt(response, "clock_port", 0) > 0) {
                        break;
                    }
                    if (JsonStatus(response) != "OutputMessage") break;
                }
            }
        }
        const std::string status = JsonStatus(response);
        if (status == "Success") {
            const auto clockPort = JsonGetInt(response, "clock_port", 0);
            const auto audioPort = JsonGetInt(response, "audio_port_tcp", 0);
            if (clockPort <= 0 || clockPort > 65535 ||
                audioPort <= 0 || audioPort > 65535) {
                error = L"setup: no clock_port/audio_port_tcp";
                continue;
            }
            clockPort_ = static_cast<std::uint16_t>(clockPort);
            deviceClockPort_ = static_cast<std::uint16_t>(clockPort);
            audioPort_ = static_cast<std::uint16_t>(audioPort);
            wireBits_ = bits;
            format_.validBits = bits;
            format_.containerBits = bits;
            LogRaat(L"setup OK: clock udp/" + std::to_wstring(clockPort_) +
                    L", audio tcp/" + std::to_wstring(audioPort_) +
                    L", wire=" + std::to_wstring(bits) + L"bit");
            return true;
        }
        LogRaat(L"setup " + ToWide(status) + L" at " + std::to_wstring(bits) + L"bit");
        if (!ContainsFormatToken(status, "FORMAT_NOT_SUPPORTED") &&
            !ContainsFormatToken(status, "NotSupported")) {
            error = L"setup status " + ToWide(status);
            return false;
        }
    }
    if (error.empty()) error = L"no supported wire bit depth";
    return false;
}

bool RaatSink::OpenAudio(std::wstring& error) noexcept {
    if (audioPort_ == 0) {
        error = L"audio port not announced";
        return false;
    }
    if (!audio_.Connect(target_.address, audioPort_, kRaatConnectTimeoutMs, error)) {
        return false;
    }
    return SendAudioCommands(sampleIndex_);
}

bool RaatSink::SendAudioCommands(std::uint64_t startSample) noexcept {
    // Order and contents mirror the reference sender exactly: STREAMID, then
    // STREAMSAMPLE, then CHMAP. Nothing else is sent on the audio channel.
    //
    // Deliberately NOT sending CMD_NORMALIZATION (4): the device-side script
    // keeps tcp_gain/tcp_peak at 0.0, and 0.0 is the "no normalisation" value
    // passed straight to stream:write. Sending gain=1.0/peak=0.0 instead makes
    // the device scale every sample by peak, i.e. by zero - silent output with
    // a perfectly healthy control session, which is exactly the symptom this
    // fixed.
    std::vector<std::uint8_t> commands;
    commands.push_back(2); // STREAMID
    AppendBe32(commands, streamId_);
    commands.push_back(3); // STREAMSAMPLE
    AppendBe64(commands, startSample);
    // CHMAP: the device calls stream:write_chmap when has_write_chmap is set,
    // and only a CHMAP command populates tcp_chmap. Without it the device keeps
    // tcp_chmap = 0 and the channel map is left undefined.
    commands.push_back(1); // CHMAP
    AppendBe32(commands, ChannelMapFor(format_.channels));
    std::lock_guard lock(audioMutex_);
    if (!audio_.SendAll(commands.data(), commands.size(), 5000)) {
        return false;
    }
    lastSendTick_ = GetTickCount64();
    return true;
}

bool RaatSink::SendStream(std::wstring& error) noexcept {
    // Official Roon stream parameters:
    // buffer_size: 5.0s, ready_threshold: 0.45, first_seq: -1, nak_port: -1
    std::ostringstream json;
    json << "{\"request\":\"stream\",\"stream_id\":" << streamId_
         << ",\"first_seq\":-1,\"nak_port\":-1,\"buffer_size\":5.0,\"ready_threshold\":0.45}";
    std::string response;
    bool final = false;
    {
        std::lock_guard lock(controlMutex_);
        if (!control_.Request(json.str(), kRaatRequestTimeoutMs,
                              response, final, error)) {
            return false;
        }
        streamRid_ = control_.LastRequestRid();
    }
    const std::string status = JsonStatus(response);
    LogRaat(L"stream id=" + std::to_wstring(streamId_) + L" status=" + ToWide(status) +
            L" rid=" + std::to_wstring(streamRid_));
    if (status == "Success" || status == "Buffering" || status == "Ready") {
        return true;
    }
    error = L"stream status " + ToWide(status);
    return false;
}

bool RaatSink::Prefill() noexcept {
    if (!source_) return false;
    // With buffer_size 5.0 and ready_threshold 0.45, the device requires
    // 5.0 * 0.45 = 2.25 seconds of PCM before emitting {"status":"Ready"}.
    // Prefill 2300ms of audio frames directly into the audio TCP stream.
    const std::uint32_t prefillMs = 2300u;
    const std::uint64_t targetFrames =
        static_cast<std::uint64_t>(format_.sampleRate) * prefillMs / 1000u;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(3000u);
    std::uint64_t produced = 0;
    while (produced < targetFrames && !stop_.load(std::memory_order_acquire)) {
        const std::uint32_t requestFrames = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(blockFrames_, targetFrames - produced));
        if (source_->Prepare(requestFrames) == kAudioSourceWouldBlock) {
            if (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (!SendSilence(requestFrames)) return false;
            produced += requestFrames;
            continue;
        }
        std::uint32_t written = 0;
        bool endOfStream = false;
        const HRESULT fill = source_->Fill(block_.data(), requestFrames, written, endOfStream);
        if (fill == kAudioSourceWouldBlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (FAILED(fill) || written == 0) break;
        if (!SendData(block_.data(), written)) return false;
        produced += written;
        sampleIndex_ += written;
        if (endOfStream) {
            endOfStreamSubmitted_.store(true, std::memory_order_release);
            break;
        }
    }
    LogRaat(L"prefill " + std::to_wstring(produced) + L"/" +
            std::to_wstring(targetFrames) + L" frames sent to device");
    return true;
}

bool RaatSink::WaitForReady(std::uint32_t timeoutMs, std::wstring& error) noexcept {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline && !stop_.load(std::memory_order_acquire)) {
        std::string response;
        bool final = false;
        std::wstring readError;
        {
            std::lock_guard lock(controlMutex_);
            if (control_.ReadFrame(streamRid_, 100, response, final, readError)) {
                const std::string status = JsonStatus(response);
                LogRaat(L"stream rid=" + std::to_wstring(streamRid_) + L" frame: " + ToWide(status));
                if (status == "Ready" || status == "Playing") {
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    error = L"timed out waiting for Ready status";
    return false;
}

bool RaatSink::SendStart(bool resume) noexcept {
    // Official Roon uses min_offset = 300000000 (300 ms in nanoseconds).
    // The device computes time = raat.output.get_local_time(token) + min_offset.
    const std::uint64_t minOffsetNs = 300000000ull;
    const std::uint64_t streamSample = resume ? sampleIndex_ : 0;
    std::ostringstream json;
    json << "{\"request\":\"start\",\"stream_sample\":" << streamSample
         << ",\"min_offset\":" << minOffsetNs << "}";
    std::string response;
    bool final = false;
    std::wstring error;
    {
        std::lock_guard lock(controlMutex_);
        if (!control_.Request(json.str(), kRaatRequestTimeoutMs,
                              response, final, error)) {
            LogRaat(L"start failed: " + error);
            return false;
        }
    }
    const std::string status = JsonStatus(response);
    if (status == "Success") {
        LogRaat(std::wstring(resume ? L"resume start " : L"start ") +
                L"sample=" + std::to_wstring(streamSample) +
                L" min_offset=300ms");
        return true;
    }
    LogRaat(L"start status " + ToWide(status) +
            L" response=" + ToWide(response.substr(0, 400)));
    return false;
}

void RaatSink::QueryDeviceClock() noexcept {
    if (clockSocket_ == INVALID_SOCKET || deviceClockPort_ == 0 || target_.address.empty()) return;

    // RAATCLKQ packet: 12 bytes [RAATCLKQ (8 bytes) | rid (4 bytes BE)]
    std::uint8_t packet[12];
    std::memcpy(packet, "RAATCLKQ", 8);
    const std::uint32_t rid = clockRid_.fetch_add(1, std::memory_order_relaxed) + 1;
    packet[8] = static_cast<std::uint8_t>(rid >> 24u);
    packet[9] = static_cast<std::uint8_t>(rid >> 16u);
    packet[10] = static_cast<std::uint8_t>(rid >> 8u);
    packet[11] = static_cast<std::uint8_t>(rid);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(deviceClockPort_);
    std::string ip = ToUtf8(target_.address);
    if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) <= 0) return;

    lastClockQueryRid_.store(rid, std::memory_order_relaxed);
    lastClockQueryTimeNs_.store(NowNanoseconds(), std::memory_order_relaxed);
    (void)sendto(clockSocket_, reinterpret_cast<const char*>(packet), sizeof(packet), 0,
                 reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

void RaatSink::KeepaliveMain() noexcept {
    while (!keepaliveStop_.load(std::memory_order_acquire)) {
        for (int i = 0; i < 25 && !keepaliveStop_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (keepaliveStop_.load(std::memory_order_acquire)) break;

        if (!control_.IsOpen()) continue;

        std::string response;
        bool final = false;
        std::wstring error;
        {
            std::lock_guard lock(controlMutex_);
            if (control_.IsOpen()) {
                // Send lightweight echo request every 2.5 seconds to keep RAATServer's
                // 10-second inactivity watchdog refreshed without touching audio HW.
                (void)control_.Request("{\"request\":\"echo\"}", 1000,
                                       response, final, error);
            }
        }
    }
}

void RaatSink::StopKeepalive() noexcept {
    keepaliveStop_.store(true, std::memory_order_release);
    if (keepaliveThread_.joinable()) {
        keepaliveThread_.join();
    }
}

void RaatSink::PumpMain() noexcept {
    struct TimePeriodScope {
        TimePeriodScope() noexcept { timeBeginPeriod(1); }
        ~TimePeriodScope() noexcept { timeEndPeriod(1); }
    } timeScope;

    struct PowerScope {
        PowerScope() noexcept {
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
        }
        ~PowerScope() noexcept {
            SetThreadExecutionState(ES_CONTINUOUS);
        }
    } powerScope;

    bool reconnected = false;
    while (!stop_.load(std::memory_order_acquire)) {
        if (paused_.load(std::memory_order_acquire)) {
            paceStartTick_ = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (!source_) break;

        const HRESULT prepared = source_->Prepare(blockFrames_);
        if (prepared == S_FALSE) {
            FinishEndOfStream();
            return;
        }
        if (prepared == kAudioSourceWouldBlock) {
            waitingForSource_.store(true, std::memory_order_release);
            if (sourceReadyEvent_) {
                WaitForSingleObject(sourceReadyEvent_, 10);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // Keep the device buffer alive during a decode stall. This is the
            // same trade-off the original sender makes: a short silent gap is
            // better than an underrun that stalls the endpoint clock.
            if (GetTickCount64() - lastSendTick_ > 20) {
                const std::uint32_t silenceFrames = std::min<std::uint32_t>(
                    blockFrames_, static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(format_.sampleRate) / 100u));
                if (silenceFrames && SendSilence(silenceFrames)) {
                    underruns_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }
            continue;
        }
        if (FAILED(prepared)) {
            RecordError(ammod::audio::kRaatStreamFailed);
            return;
        }

        waitingForSource_.store(false, std::memory_order_release);
        std::uint32_t written = 0;
        bool endOfStream = false;
        const HRESULT filled = source_->Fill(block_.data(), blockFrames_, written, endOfStream);
        if (filled == kAudioSourceWouldBlock) {
            waitingForSource_.store(true, std::memory_order_release);
            continue;
        }
        if (FAILED(filled) || written == 0) {
            if (endOfStream) {
                endOfStreamSubmitted_.store(true, std::memory_order_release);
                endOfStreamComplete_.store(true, std::memory_order_release);
            }
            return;
        }

        if (!SendData(block_.data(), written)) {
            if (!reconnected) {
                reconnected = true;
                LogRaat(L"audio path lost; reconnecting");
                std::wstring error;
                {
                    std::lock_guard lock(audioMutex_);
                    audio_.Close();
                }
                if (control_.IsOpen()) {
                    std::lock_guard lock(controlMutex_);
                    std::string response;
                    bool final = false;
                    std::wstring stopError;
                    (void)control_.Request("{\"request\":\"stop\"}", 1000,
                                           response, final, stopError);
                }
                if (SendStream(error) && OpenAudio(error) && SendStart(true)) {
                    LogRaat(L"audio path re-established mid-stream");
                    reconnected = false;
                    paceStartTick_ = 0;
                    continue;
                }
                LogRaat(L"audio reconnect failed: " + error);
            }
            RecordError(ammod::audio::kRaatStreamFailed);
            return;
        }
        sampleIndex_ += written;

        // Real-time pacing gate. `sampleIndex_` counts everything handed to the
        // device, including the prefill. To prevent the device ring buffer (~2s)
        // from overrunning and discarding frames, we enforce a strict pacing gate:
        // when `sampleIndex_ > allowed`, loop and wait until the DAC's playback
        // catches up, keeping alive and observing stop/pause signals.
        if (paceStartTick_ == 0) {
            paceStartTick_ = GetTickCount64();
            paceStartSample_ = sampleIndex_;
            paceLastLogTick_ = paceStartTick_;
        }

        while (!stop_.load(std::memory_order_acquire) &&
               !paused_.load(std::memory_order_acquire)) {
            const std::uint64_t nowTick = GetTickCount64();
            const std::uint64_t elapsedMs = nowTick - paceStartTick_;
            const std::uint64_t allowed =
                paceStartSample_ +
                static_cast<std::uint64_t>(format_.sampleRate) * elapsedMs / 1000u;

            if (sampleIndex_ <= allowed) {
                break;
            }

            const std::uint64_t excessFrames = sampleIndex_ - allowed;
            const std::uint64_t waitMs = (excessFrames * 1000u) / format_.sampleRate;
            const std::uint32_t sleepMs = static_cast<std::uint32_t>(
                std::clamp<std::uint64_t>(waitMs, 1u, 10u));

            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }

        const std::uint64_t nowLogTick = GetTickCount64();
        if (nowLogTick - paceLastLogTick_ >= 5000) {
            paceLastLogTick_ = nowLogTick;
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
            const std::uint64_t elapsedMs = nowLogTick - paceStartTick_;
            const std::uint64_t allowed =
                paceStartSample_ +
                static_cast<std::uint64_t>(format_.sampleRate) * elapsedMs / 1000u;
            LogRaat(L"pace sent=" + std::to_wstring(sampleIndex_) +
                    L" allowed=" + std::to_wstring(allowed) +
                    L" lead=" +
                    std::to_wstring(static_cast<std::int64_t>(sampleIndex_) -
                                    static_cast<std::int64_t>(allowed)) + L" frames");
        }

        if (endOfStream) {
            FinishEndOfStream();
            return;
        }
    }
}

void RaatSink::FinishEndOfStream() noexcept {
    endOfStreamSubmitted_.store(true, std::memory_order_release);
    if (control_.IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_.Request("{\"request\":\"end_stream\"}", 1500,
                               response, final, error);
    }
    // The device still holds roughly one look-ahead of audio. Reporting
    // completion immediately would let the runtime tear the session down and
    // cut the tail; wait for that buffer to play out instead.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::min<std::uint32_t>(lookaheadMs_, 1000u) + 200u);
    while (!stop_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stop_.load(std::memory_order_acquire)) return;
    endOfStreamComplete_.store(true, std::memory_order_release);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    LogRaat(L"end of stream");
}

bool RaatSink::SendData(const std::int32_t* samples, std::uint32_t frames) noexcept {
    if (!samples || frames == 0) return true;
    const std::uint32_t samplesPerFrame = format_.channels;
    const std::size_t sampleCount = static_cast<std::size_t>(frames) * samplesPerFrame;
    const std::uint32_t bytesPerSample = wireBits_ / 8u;
    if (!PackCanonicalPcm32(samples, pcm_.data(), sampleCount, wireBits_)) return false;
    const std::uint32_t bodyBytes = static_cast<std::uint32_t>(
        sampleCount * bytesPerSample);

    frame_.clear();
    frame_.push_back(0); // DATA
    AppendBe32(frame_, frames);
    AppendBe32(frame_, bodyBytes);
    frame_.insert(frame_.end(), pcm_.begin(), pcm_.begin() + bodyBytes);

    std::lock_guard lock(audioMutex_);
    if (!audio_.SendAll(frame_.data(), frame_.size(), 2000)) return false;
    lastSendTick_ = GetTickCount64();
    submittedFrames_.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

bool RaatSink::SendSilence(std::uint32_t frames) noexcept {
    if (frames == 0) return true;
    std::fill(block_.begin(),
              block_.begin() + static_cast<std::size_t>(frames) * format_.channels,
              0);
    if (!SendData(block_.data(), frames)) return false;
    sampleIndex_ += frames;
    return true;
}

void RaatSink::ClockMain() noexcept {
    if (clockSocket_ == INVALID_SOCKET) return;
    auto lastQuery = std::chrono::steady_clock::now();
    while (!clockStop_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastQuery >= std::chrono::seconds(10)) {
            lastQuery = now;
            QueryDeviceClock();
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET(clockSocket_, &set);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        const int ready = select(0, &set, nullptr, nullptr, &tv);
        if (ready <= 0 || !FD_ISSET(clockSocket_, &set)) continue;

        std::uint8_t buffer[256]{};
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = recvfrom(clockSocket_, reinterpret_cast<char*>(buffer),
                                      sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received < 12) continue;
        if (std::memcmp(buffer, "RAATCLKQ", 8) == 0) {
            const std::uint32_t rid = ReadBe32(buffer + 8);
            std::vector<std::uint8_t> reply(28);
            std::memcpy(reply.data(), "RAATCLKR", 8);
            WriteBe32(reply.data() + 8, rid);
            const std::uint64_t nowNs = NowNanoseconds();
            for (int index = 0; index < 8; ++index) {
                reply[12 + index] = static_cast<std::uint8_t>(nowNs >> (56 - index * 8));
            }
            sendto(clockSocket_, reinterpret_cast<const char*>(reply.data()),
                   static_cast<int>(reply.size()), 0,
                   reinterpret_cast<const sockaddr*>(&from), fromLength);
            clockRepliesOut_.fetch_add(1, std::memory_order_relaxed);
        } else if (received >= 28 && std::memcmp(buffer, "RAATCLKR", 8) == 0) {
            clockRepliesIn_.fetch_add(1, std::memory_order_relaxed);
            const std::uint32_t respRid = ReadBe32(buffer + 8);
            const std::uint32_t expRid = lastClockQueryRid_.load(std::memory_order_relaxed);
            if (respRid == expRid || expRid == 0) {
                const std::uint64_t recvTimeNs = NowNanoseconds();
                const std::uint64_t sendTimeNs = lastClockQueryTimeNs_.load(std::memory_order_relaxed);
                if (sendTimeNs != 0 && recvTimeNs >= sendTimeNs) {
                    const std::uint64_t deviceTimeNs = ReadBe64(buffer + 12);
                    const std::uint64_t rttNs = recvTimeNs - sendTimeNs;
                    const std::int64_t offsetNs = static_cast<std::int64_t>(deviceTimeNs) -
                                                  static_cast<std::int64_t>(sendTimeNs + rttNs / 2);
                    deviceClockOffsetNs_.store(offsetNs, std::memory_order_release);
                    deviceClockRttUs_.store(static_cast<std::uint32_t>(rttNs / 1000ull), std::memory_order_release);
                    LogRaat(L"endpoint clock sync: rtt=" + std::to_wstring(rttNs / 1000ull) +
                            L"us offset=" + std::to_wstring(offsetNs / 1000ull) + L"us");
                }
            }
        }
    }
}

void RaatSink::StopClock() noexcept {
    clockStop_.store(true, std::memory_order_release);
    if (clockThread_.joinable()) clockThread_.join();
    if (clockSocket_ != INVALID_SOCKET) {
        closesocket(clockSocket_);
        clockSocket_ = INVALID_SOCKET;
    }
    clockPort_ = 0;
}

void RaatSink::RecordError(HRESULT error) noexcept {
    lastError_.store(error, std::memory_order_release);
    if (state_.load(std::memory_order_acquire) == AudioSinkState::Running) {
        state_.store(AudioSinkState::Faulted, std::memory_order_release);
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    std::wstringstream stream;
    stream << L"error hr=0x" << std::hex << std::uppercase
           << std::setw(8) << std::setfill(L'0')
           << static_cast<std::uint32_t>(error);
    LogRaat(stream.str());
}

bool RaatSink::ReadSettingInt(int& value, const wchar_t* key, int fallback) const noexcept {
    const std::wstring path = IniPath();
    if (path.empty()) {
        value = fallback;
        return false;
    }
    value = GetPrivateProfileIntW(L"roon", key, fallback, path.c_str());
    return true;
}

AudioSinkStats RaatSink::Stats() const noexcept {
    AudioSinkStats stats;
    stats.kind = AudioSinkKind::Raat;
    stats.state = state_.load(std::memory_order_acquire);
    stats.submittedFrames = submittedFrames_.load(std::memory_order_relaxed);
    stats.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
    stats.underruns = underruns_.load(std::memory_order_relaxed);
    stats.controlFailures = controlFailures_.load(std::memory_order_relaxed);
    stats.lastError = lastError_.load(std::memory_order_acquire);
    return stats;
}

int AudioCommandSelfTest() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* what) {
        if (!condition) {
            std::printf("audio-cmd-check FAIL: %s\n", what);
            ++failures;
        }
    };

    // Channel maps must match what the reference sender computes
    // (~(-1 << channels)) for the layouts a Roon endpoint exposes.
    check(ChannelMapFor(0) == 0x00000000u, "0 channels");
    check(ChannelMapFor(1) == 0x00000001u, "mono");
    check(ChannelMapFor(2) == 0x00000003u, "stereo");
    check(ChannelMapFor(6) == 0x0000003Fu, "5.1");
    check(ChannelMapFor(8) == 0x000000FFu, "7.1");

    // The exact byte sequence OpenAudio must put on the wire: STREAMID,
    // STREAMSAMPLE, CHMAP - and nothing else. A CMD_NORMALIZATION with
    // peak=0.0 here is what silenced playback, so its absence is asserted.
    std::vector<std::uint8_t> commands;
    commands.push_back(2);  // STREAMID
    AppendBe32(commands, 0x12345678u);
    commands.push_back(3);  // STREAMSAMPLE
    AppendBe64(commands, 0);
    commands.push_back(1);  // CHMAP
    AppendBe32(commands, ChannelMapFor(2));

    const std::uint8_t expected[] = {
        0x02, 0x12, 0x34, 0x56, 0x78,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x03,
    };
    check(commands.size() == sizeof(expected), "command frame length");
    if (commands.size() == sizeof(expected)) {
        check(std::memcmp(commands.data(), expected, sizeof(expected)) == 0,
              "command frame bytes");
    }
    for (const std::uint8_t byte : commands) {
        check(byte != 4, "no CMD_NORMALIZATION in the audio preamble");
    }

    if (failures == 0) std::printf("audio-cmd-check ok\n");
    return failures == 0 ? 0 : 9;
}

} // namespace ammod::audio_v2
