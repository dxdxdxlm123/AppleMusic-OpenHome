#pragma once

#include "AudioSink.h"
#include "RaatProtocol.h"
#include "RaatScripts.h"
#include "RaatTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ammod::audio_v2 {

// Channel map carried by CMD_CHMAP on the audio channel: one bit per channel,
// so 2 channels -> 0x3 and 6 channels -> 0x3F.
inline std::uint32_t ChannelMapFor(std::uint32_t channels) noexcept {
    if (channels == 0) return 0;
    if (channels >= 32) return 0xFFFFFFFFu;
    return (1u << channels) - 1u;
}

// Offline check of the audio-channel command sequence. Returns 0 on success.
int AudioCommandSelfTest();

// Official-Roon-parity RAAT sender sink:
// - Native binary 0x80000003 LL_KEEPALIVE (2s / 10s watchdog) via ControlChannel
// - Signed official Lua corpus injection (module preserved across all variants)
// - Active 2s x 3 min-RTT UDP device clock sync (RaatClockSource parity)
// - Device-clock-driven pacing (endpoint stays 100% Clock Master)
// - Async 60 KB TCP send queue with 10s serverside buffer overrun protection
// - Dynamic COMMAND_CHMAP silence-channel stripping & dCS buffer quirk
// - 500ms endpoint Dropout telemetry (30s sliding window, >3s circuit breaker)
// - Same-format true Gapless cross-instance session handoff (zero-gap stream_id reuse)
// - Phase 5 SMTC Now Playing metadata + LL 0x00000003 artwork + front-panel transport control
class RaatSink final : public IAudioSink {
public:
    RaatSink() noexcept = default;
    ~RaatSink() override;

    RaatSink(const RaatSink&) = delete;
    RaatSink& operator=(const RaatSink&) = delete;

    AudioSinkKind Kind() const noexcept override { return AudioSinkKind::Raat; }
    HRESULT Open(const AudioSinkOpenParams& params) noexcept override;
    HRESULT Start(IIntegerPcmSource& source) noexcept override;
    void Pause() noexcept override;
    void Stop() noexcept override;
    void Close() noexcept override;
    void NotifyNextTrack() noexcept override;
    void CancelNaturalEndOfStreamDelayForSuccessor() noexcept override;

    AudioSinkState State() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AudioSinkStats Stats() const noexcept override;
    const PcmFormat& Format() const noexcept override { return format_; }
    std::uint32_t BufferFrames() const noexcept override { return blockFrames_; }
    bool WaitingForSource() const noexcept override {
        return waitingForSource_.load(std::memory_order_acquire);
    }
    bool EndOfStreamSubmitted() const noexcept override {
        return endOfStreamSubmitted_.load(std::memory_order_acquire);
    }
    bool EndOfStreamComplete() const noexcept override {
        return endOfStreamComplete_.load(std::memory_order_acquire);
    }
    void SetResumeBlocked(bool blocked) noexcept override { (void)blocked; }
    void SetVolume(float volume) noexcept override;
    void SetMute(bool mute) noexcept override;
    bool SupportsHardwareVolume() const noexcept override;

    // Official Roon persistent control session lifecycle:
    // - PrewarmFromIniAsync: connects & injects Lua VM in the background at host startup
    //   when output=r_device/roon, keeping 2s/10s binary keepalive running indefinitely.
    // - DiscardParkedSession: cleanly tears down the resident control session on shutdown
    //   or when switching away from RAAT output.
    static void PrewarmFromIniAsync() noexcept;
    static void DiscardParkedSession() noexcept;

private:
    struct AudioWriteCmd {
        std::vector<std::uint8_t> bytes;
        std::uint32_t frames{0};
    };

    struct DropoutRecord {
        std::uint64_t tickMs{0};
        std::uint64_t samples{0};
    };

    struct RaatVolumeCaps {
        bool isSupported{false};
        std::string type; // "db", "number", "incremental"
        double min{0.0};
        double max{100.0};
        double step{1.0};
        double value{100.0};
        bool mute{false};
    };

    // Parked live RAAT session handed off across track transitions:
    // - Same-format natural EOS: reuses both control TCP + live audio stream (zero-gap Gapless)
    // - Format change / manual skip / stop: keeps control TCP + Lua VM alive (official Roon parity),
    //   tearing down only the output plugin and issuing a fresh setup on the same control channel.
    struct ParkedSession {
        std::unique_ptr<raat::ControlChannel> control;
        std::unique_ptr<raat::TcpConnection> audio;
        bool audioLive{false};
        RaatTarget target{};
        PcmFormat format{};
        std::uint16_t wireBits{};
        std::uint32_t streamId{};
        std::uint64_t sampleIndex{};
        std::uint16_t audioPort{};
        std::uint16_t deviceClockPort{};
        std::uint32_t setupRid{};
        std::uint32_t streamRid{};
        std::uint32_t transportSubRid{};
        std::uint32_t controlsSubRid{};
        std::uint32_t lastSentChmap{};
        std::uint64_t expectedStreamSample{};
        std::string deviceVendor;
        bool hasWriteChmap{false};
        std::int64_t deviceClockOffsetNs{0};
        std::uint64_t deviceOutputDelayNs{0};
        std::uint64_t clockFirstLocalNs{0};
        std::int64_t clockFirstOffsetNs{0};
        std::uint32_t prefilledMs{0};
        std::uint32_t startLeadMs{300};
        std::uint64_t parkedAtMs{0};
    };

    enum class AdoptResult { None, FullGapless, ControlOnly };

    AdoptResult TryAdoptParkedSession() noexcept;
    void ParkSessionForGapless() noexcept;
    void ParkControlForReuse() noexcept;
    static void TeardownControlStream(raat::ControlChannel& control,
                                      std::uint32_t setupRid,
                                      bool waitTeardown,
                                      bool sendStop = true,
                                      bool sendEndStream = true) noexcept;

    bool EnsureSession(bool reuseControl = false) noexcept;
    bool OpenControl(std::wstring& error) noexcept;
    bool LoadScripts(std::wstring& error) noexcept;
    bool LoadOneScript(const raat::ScriptRecord& record, int variant,
                       std::wstring& error) noexcept;
    bool SendInfo(std::wstring& error) noexcept;
    void SendClientType() noexcept;
    void SendSubscriptions() noexcept;
    std::string GetPersistentControllerId() const noexcept;
    void SendRequestSource() noexcept;
    bool SendSetup(std::wstring& error) noexcept;
    bool OpenAudio(std::wstring& error) noexcept;
    bool SendStream(std::wstring& error) noexcept;
    bool WaitForReady(std::uint32_t timeoutMs) noexcept;
    bool Prefill() noexcept;
    bool SendStart(bool resume) noexcept;
    bool SendSync() noexcept;
    void OnControlEvent(std::uint32_t rid, const std::string& json, bool isFinal) noexcept;
    void PushNowPlayingMetadata(bool force, bool isPlaying) noexcept;
    void DispatchFrontPanelControl(const std::string& controlAction) noexcept;

    bool StartClockThread(bool preserveBaseline = false) noexcept;
    void StartAudioSender() noexcept;
    void StopAudioSender(bool flushQueue) noexcept;
    void AudioSendMain() noexcept;
    bool EnqueueAudioBytes(std::vector<std::uint8_t> bytes,
                           std::uint32_t frames) noexcept;

    void PumpMain() noexcept;
    void ClockMain() noexcept;
    bool RunDeviceClockSyncRound() noexcept;
    std::uint64_t GetRemoteClockNs() const noexcept;

    bool SendData(const std::int32_t* samples, std::uint32_t frames) noexcept;
    bool SendSilence(std::uint32_t frames) noexcept;
    bool SendAudioCommands(std::uint64_t startSample) noexcept;
    void FinishEndOfStream() noexcept;
    void StopClock() noexcept;
    void RecordError(HRESULT error) noexcept;
    bool ReadSettingInt(int& value, const wchar_t* key, int fallback) const noexcept;
    bool ReadSettingString(std::wstring& value, const wchar_t* key) const noexcept;
    bool CheckSupportedFormat(const std::string& supported) const noexcept;
    double RequestedBufferSeconds() const noexcept;

    static int GetPreferredVariant(const std::wstring& endpoint) noexcept;
    static void SetPreferredVariant(const std::wstring& endpoint, int variant) noexcept;

    AudioSinkOpenParams params_{};
    RaatTarget target_{};
    PcmFormat format_{};
    std::uint16_t wireBits_{};
    std::uint32_t blockFrames_{1024};
    std::uint32_t blockSizeBytes_{60000};
    std::uint32_t lookaheadMs_{5000};
    std::uint32_t startLeadMs_{50};
    std::uint32_t prefilledMs_{0};
    std::uint32_t resyncDelayMs_{0};
    std::uint32_t streamId_{1};
    std::wstring bindInterface_;
    std::string deviceVendor_;
    bool hasWriteChmap_{false};

    IIntegerPcmSource* source_{};
    HANDLE sourceReadyEvent_{};

    std::unique_ptr<raat::ControlChannel> control_;
    std::unique_ptr<raat::TcpConnection> audio_;
    std::mutex controlMutex_;
    std::mutex audioMutex_;
    std::wstring localAddress_;
    std::atomic<std::uint32_t> setupRid_{0};
    std::atomic<std::uint32_t> streamRid_{0};
    std::atomic<std::uint32_t> transportSubRid_{0};
    std::atomic<std::uint32_t> controlsSubRid_{0};
    std::atomic<bool> streamReady_{false};
    std::atomic<bool> teardownObserved_{false};
    std::atomic<bool> tooManyDropouts_{false};
    std::atomic<bool> successorPending_{false};
    std::atomic<bool> gaplessHandoffReady_{false};
    std::atomic<bool> metadataRefreshRequested_{false};
    std::mutex dropoutMutex_;
    std::deque<DropoutRecord> dropoutWindow_;

    // Last pushed Now Playing metadata (to avoid redundant updates).
    std::wstring lastMetaTitle_;
    std::wstring lastMetaArtist_;
    std::wstring lastMetaAlbum_;
    std::uint32_t lastMetaDurationMs_{0};
    std::uint32_t lastMetaSeekSec_{UINT32_MAX};
    std::size_t lastMetaArtworkSize_{0};
    std::uint32_t lastMetaArtworkHash_{0};
    bool lastMetaHasArtwork_{false};
    bool lastMetaPlaying_{false};
    std::uint64_t lastMetaPollMs_{0};
    std::uint64_t trackStartSampleIndex_{0};

    enum class DisplayMode : std::uint8_t {
        Standard = 0,
        Pure = 1,
    };
    DisplayMode displayMode_{DisplayMode::Standard};
    std::uint32_t transportIntervalMs_{3000};
    std::uint32_t sliceMs_{50};
    bool debugPaceLogging_{false};
    bool testMode_{false};

    // Zero-heap allocation buffer pool for audio streaming
    std::vector<std::uint8_t> AcquireBuffer(std::size_t minCapacity) noexcept;
    void ReleaseBuffer(std::vector<std::uint8_t>&& buf) noexcept;
    std::mutex bufferPoolMutex_;
    std::vector<std::vector<std::uint8_t>> freeBuffers_;

    // Async 60 KB audio send queue (RaatTcpAudioSource parity).
    std::mutex sendQueueMutex_;
    std::condition_variable sendQueueCv_;
    std::deque<AudioWriteCmd> sendQueue_;
    std::atomic<std::uint64_t> queuedFrames_{0};
    std::atomic<bool> audioSendStop_{false};
    std::atomic<bool> audioSendFault_{false};
    std::atomic<int> lastAudioWsaError_{0};
    std::thread audioSendThread_;
    std::uint32_t lastSentChmap_{0};
    std::uint64_t expectedStreamSample_{0};
    bool streamSampleInitialized_{false};

    // UDP device clock sync state (RaatClockSource parity).
    SOCKET clockSocket_{INVALID_SOCKET};
    std::uint16_t clockPort_{};
    std::uint16_t audioPort_{};
    std::uint16_t deviceClockPort_{};
    std::thread clockThread_;
    std::atomic<bool> clockStop_{false};
    std::atomic<bool> clockLocked_{false};
    std::atomic<std::int64_t> deviceClockOffsetNs_{0};
    std::atomic<std::uint64_t> deviceOutputDelayNs_{0};
    std::atomic<std::uint64_t> deviceClockRttUs_{0};
    std::uint32_t nextClockQueryRid_{1};
    std::uint64_t clockFirstLocalNs_{0};
    std::int64_t clockFirstOffsetNs_{0};
    std::uint64_t clockLastTraceMs_{0};
    std::atomic<std::uint32_t> clockProbesIn_{0};
    std::atomic<std::uint32_t> clockRepliesOut_{0};

    // Device-clock-driven pacing state.
    std::uint64_t paceStartRemoteNs_{0};
    std::uint64_t paceStartSample_{0};
    std::uint64_t paceLastLogTick_{0};

    std::thread pumpThread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> startedOnce_{false};
    std::atomic<bool> waitingForSource_{false};
    std::atomic<bool> endOfStreamSubmitted_{false};
    std::atomic<bool> endOfStreamComplete_{false};
    std::atomic<AudioSinkState> state_{AudioSinkState::Closed};
    std::atomic<HRESULT> lastError_{S_OK};

    std::atomic<std::uint64_t> submittedFrames_{0};
    std::atomic<std::uint64_t> droppedFrames_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> controlFailures_{0};
    std::uint64_t sampleIndex_{0};
    std::uint64_t lastSendTick_{0};
    std::vector<std::int32_t> block_;
    std::vector<std::int32_t> packedActive_;

    void StartVolumeWorker() noexcept;
    void StopVolumeWorker() noexcept;
    void VolumeWorkerMain() noexcept;

    mutable std::mutex volumeMutex_;
    RaatVolumeCaps volumeCaps_;
    std::atomic<bool> volumeSupported_{false};
    std::atomic<bool> volumeDirty_{false};
    std::atomic<float> pendingVolume_{1.0f};
    std::atomic<bool> muteDirty_{false};
    std::atomic<bool> pendingMute_{false};
    std::thread volumeWorker_;
    std::condition_variable volumeCv_;
    std::mutex volumeWorkerMutex_;
    std::atomic<bool> stopVolumeWorker_{false};
    std::atomic<std::uint64_t> lastVolumeSendMs_{0};

    static inline std::mutex s_variantMutex;
    static inline std::unordered_map<std::wstring, int> s_preferredVariants;
    static inline std::mutex s_gaplessMutex;
    static inline std::unique_ptr<ParkedSession> s_gaplessParking;
    static inline std::atomic<int> s_activeStartCount{0};
    static inline std::atomic<bool> s_prewarmInFlight{false};
    static inline std::atomic<std::uint32_t> s_lastRaatSampleRate{0};
};

} // namespace ammod::audio_v2
