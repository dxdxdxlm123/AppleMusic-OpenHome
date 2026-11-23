#include "RaatSink.h"

#include "DlnaLog.h"
#include "LocalPlaybackTuning.h"
#include "PcmTypes.h"
#include "RaatDiscovery.h"
#include "../AudioErrors.h"
#include "../TrackMetadata.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <thread>

namespace ammod::audio_v2 {
namespace {

using namespace ammod::raat;

// Official Roon Core timeouts and sizing constants (Roon.Broker.Core.dll).
constexpr std::uint32_t kRaatConnectTimeoutMs = 10000;
constexpr std::uint32_t kRaatSetupTimeoutMs = 15000;
constexpr std::uint32_t kRaatReadyTimeoutMs = 3000;
constexpr std::uint32_t kRaatRequestTimeoutMs = 5000;
constexpr std::uint32_t kRaatDefaultBlockSizeBytes = 60000;
constexpr std::uint32_t kRaatDefaultLookaheadMs = 5000;
constexpr std::uint32_t kRaatDefaultStartLeadMs = 50;
constexpr std::uint32_t kRaatClockSyncIntervalMs = 2000;
constexpr int kRaatClockSyncIterations = 3;
constexpr int kRaatClockSyncMaxAttempts = 200;
constexpr std::uint64_t kRaatClockMaxRttNs = 30000000ull; // 30 ms
constexpr std::uint64_t kRaatGaplessParkMaxAgeMs = 5000ull;
constexpr std::size_t kRaatMaxArtworkBytes = 2u * 1024u * 1024u;

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

void AppendBe64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint64_t ReadBe64(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8u) | static_cast<std::uint64_t>(data[i]);
    }
    return value;
}

std::uint32_t ReadBe32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24u) |
           (static_cast<std::uint32_t>(data[1]) << 16u) |
           (static_cast<std::uint32_t>(data[2]) << 8u) |
           static_cast<std::uint32_t>(data[3]);
}

static void BindThreadToIsolatedCore(int coreOffset) noexcept {
    const unsigned int totalCores = std::thread::hardware_concurrency();
    if (totalCores <= 1) return;
    DWORD_PTR affinityMask = 0;
    if (totalCores >= 4) {
        const unsigned int targetCore = 1 + (coreOffset % (totalCores - 1));
        affinityMask = (static_cast<DWORD_PTR>(1) << targetCore);
    } else {
        affinityMask = (static_cast<DWORD_PTR>(1) << 1);
    }
    SetThreadAffinityMask(GetCurrentThread(), affinityMask);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

static inline bool IsDifferentClockFamily(std::uint32_t r1, std::uint32_t r2) noexcept {
    if (r1 == 0 || r2 == 0) return false;
    if (r1 == r2) return false;
    const bool fam1_44 = (r1 % 44100 == 0);
    const bool fam2_44 = (r2 % 44100 == 0);
    return fam1_44 != fam2_44;
}

bool ContainsFormatToken(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

bool ContainsWideCaseInsensitive(const std::wstring& haystack, const wchar_t* needle) {
    if (!needle || !needle[0]) return false;
    std::wstring lowerHay = haystack;
    std::wstring lowerNeedle(needle);
    for (auto& ch : lowerHay) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    for (auto& ch : lowerNeedle) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return lowerHay.find(lowerNeedle) != std::wstring::npos;
}

std::uint32_t Fnv1a32(const std::vector<std::uint8_t>& data) noexcept {
    std::uint32_t hash = 2166136261u;
    for (std::uint8_t b : data) {
        hash ^= b;
        hash *= 16777619u;
    }
    return hash;
}

void SendMediaKey(WORD vk) noexcept {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    (void)SendInput(2, inputs, sizeof(INPUT));
}

constexpr UINT kRaatArtworkMaxDim = 500u;

// Official Roon parity: scale artwork to max 500x500 JPEG (spec=500) via WIC
// before transmitting over RAAT LL 0x00000003. Falls back cleanly on any error.
bool ScaleArtworkToMax500Jpeg(const std::vector<std::uint8_t>& raw,
                              const std::string& rawMime,
                              std::vector<std::uint8_t>& outJpeg) noexcept {
    if (raw.empty()) return false;
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(coHr);
    struct CoGuard {
        bool uninit;
        ~CoGuard() { if (uninit) CoUninitialize(); }
    } coGuard{needUninit};

    auto release = [](IUnknown* p) noexcept { if (p) p->Release(); };
    using ComPtrFactory = std::unique_ptr<IWICImagingFactory, decltype(release)>;
    using ComPtrStream = std::unique_ptr<IStream, decltype(release)>;
    using ComPtrDecoder = std::unique_ptr<IWICBitmapDecoder, decltype(release)>;
    using ComPtrFrame = std::unique_ptr<IWICBitmapFrameDecode, decltype(release)>;
    using ComPtrScaler = std::unique_ptr<IWICBitmapScaler, decltype(release)>;
    using ComPtrEncoder = std::unique_ptr<IWICBitmapEncoder, decltype(release)>;
    using ComPtrEncFrame = std::unique_ptr<IWICBitmapFrameEncode, decltype(release)>;
    using ComPtrProps = std::unique_ptr<IPropertyBag2, decltype(release)>;

    IWICImagingFactory* rawFactory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&rawFactory))) || !rawFactory) {
        return false;
    }
    ComPtrFactory factory(rawFactory, release);

    HGLOBAL inMem = GlobalAlloc(GMEM_MOVEABLE, raw.size());
    if (!inMem) return false;
    if (void* dst = GlobalLock(inMem)) {
        std::memcpy(dst, raw.data(), raw.size());
        GlobalUnlock(inMem);
    } else {
        GlobalFree(inMem);
        return false;
    }
    IStream* rawInStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(inMem, TRUE, &rawInStream)) || !rawInStream) {
        GlobalFree(inMem);
        return false;
    }
    ComPtrStream inStream(rawInStream, release);

    IWICBitmapDecoder* rawDecoder = nullptr;
    if (FAILED(factory->CreateDecoderFromStream(
            inStream.get(), nullptr, WICDecodeMetadataCacheOnLoad, &rawDecoder)) || !rawDecoder) {
        return false;
    }
    ComPtrDecoder decoder(rawDecoder, release);

    IWICBitmapFrameDecode* rawFrame = nullptr;
    if (FAILED(decoder->GetFrame(0, &rawFrame)) || !rawFrame) return false;
    ComPtrFrame frame(rawFrame, release);

    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return false;

    const bool alreadySmallJpeg =
        (width <= kRaatArtworkMaxDim && height <= kRaatArtworkMaxDim &&
         rawMime == "image/jpeg" && raw.size() <= 160u * 1024u);
    if (alreadySmallJpeg) return false;

    UINT dstW = width;
    UINT dstH = height;
    if (width > kRaatArtworkMaxDim || height > kRaatArtworkMaxDim) {
        if (width >= height) {
            dstW = kRaatArtworkMaxDim;
            dstH = std::max<UINT>(1u, static_cast<UINT>(
                (static_cast<std::uint64_t>(height) * kRaatArtworkMaxDim + width / 2u) / width));
        } else {
            dstH = kRaatArtworkMaxDim;
            dstW = std::max<UINT>(1u, static_cast<UINT>(
                (static_cast<std::uint64_t>(width) * kRaatArtworkMaxDim + height / 2u) / height));
        }
    }

    IWICBitmapSource* encodeSrc = frame.get();
    ComPtrScaler scaler(nullptr, release);
    if (dstW != width || dstH != height) {
        IWICBitmapScaler* rawScaler = nullptr;
        if (FAILED(factory->CreateBitmapScaler(&rawScaler)) || !rawScaler) return false;
        scaler.reset(rawScaler);
        if (FAILED(scaler->Initialize(frame.get(), dstW, dstH,
                                      WICBitmapInterpolationModeFant))) {
            return false;
        }
        encodeSrc = scaler.get();
    }

    IStream* rawOutStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &rawOutStream)) || !rawOutStream) {
        return false;
    }
    ComPtrStream outStream(rawOutStream, release);

    IWICBitmapEncoder* rawEncoder = nullptr;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &rawEncoder)) ||
        !rawEncoder) {
        return false;
    }
    ComPtrEncoder encoder(rawEncoder, release);
    if (FAILED(encoder->Initialize(outStream.get(), WICBitmapEncoderNoCache))) return false;

    IWICBitmapFrameEncode* rawEncFrame = nullptr;
    IPropertyBag2* rawProps = nullptr;
    if (FAILED(encoder->CreateNewFrame(&rawEncFrame, &rawProps)) || !rawEncFrame) return false;
    ComPtrEncFrame encFrame(rawEncFrame, release);
    ComPtrProps props(rawProps, release);

    if (props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT var{};
        var.vt = VT_R4;
        var.fltVal = 0.85f;
        (void)props->Write(1, &opt, &var);
    }
    if (FAILED(encFrame->Initialize(props.get()))) return false;
    if (FAILED(encFrame->SetSize(dstW, dstH))) return false;
    WICPixelFormatGUID pixelFmt = GUID_WICPixelFormat24bppBGR;
    (void)encFrame->SetPixelFormat(&pixelFmt);
    if (FAILED(encFrame->WriteSource(encodeSrc, nullptr))) return false;
    if (FAILED(encFrame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;

    STATSTG stat{};
    if (FAILED(outStream->Stat(&stat, STATFLAG_NONAME))) return false;
    const std::uint64_t outSize = stat.cbSize.QuadPart;
    if (outSize == 0 || outSize > kRaatMaxArtworkBytes) return false;

    HGLOBAL outMem = nullptr;
    if (FAILED(GetHGlobalFromStream(outStream.get(), &outMem)) || !outMem) return false;
    const void* srcBytes = GlobalLock(outMem);
    if (!srcBytes) return false;
    outJpeg.resize(static_cast<std::size_t>(outSize));
    std::memcpy(outJpeg.data(), srcBytes, outJpeg.size());
    GlobalUnlock(outMem);
    return !outJpeg.empty();
}

// Lightweight SMTC watcher using the existing am_exclusive_media_control.exe
// helper (same TRACK2 protocol as DlnaSink) so RaatSink can push Now Playing
// title/artist/album/duration and binary artwork to Roon Ready displays.
class RaatTrackMonitor final {
public:
    struct Info final : metadata::TrackSnapshot {
        bool valid{};
        std::uint64_t stampTick{};
        std::uint32_t positionMs{};
        bool hasPosition{};
        std::uint64_t positionStampTick{};
    };

    static RaatTrackMonitor& Instance() noexcept {
        static RaatTrackMonitor* instance = new RaatTrackMonitor();
        return *instance;
    }

    void EnsureStarted() noexcept {
        std::lock_guard startLock(startMutex_);
        if (running_.load(std::memory_order_acquire) || GetTickCount64() < retryAfter_) return;
        if (reader_.joinable()) reader_.join();
        if (process_) { CloseHandle(process_); process_ = nullptr; }
        if (job_) { CloseHandle(job_); job_ = nullptr; }
        retryAfter_ = GetTickCount64() + 2000;
        { std::lock_guard lock(mutex_); info_ = {}; haveLine_ = false; }
        Start();
    }

    Info Snapshot(std::uint32_t waitMs) const noexcept {
        const auto deadline = GetTickCount64() + waitMs;
        for (;;) {
            {
                std::lock_guard lock(mutex_);
                if (haveLine_) return info_;
            }
            if (GetTickCount64() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard lock(mutex_);
        return info_;
    }

private:
    RaatTrackMonitor() = default;

    void Start() noexcept {
        std::vector<std::filesystem::path> candidates;
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) {
            candidates.push_back(std::filesystem::path(modulePath).parent_path() /
                                 L"am_exclusive_media_control.exe");
        }
        candidates.push_back(std::filesystem::path(IniPath()).parent_path() /
                             L"am_exclusive_media_control.exe");
        std::filesystem::path helper;
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                helper = candidate;
                break;
            }
        }
        if (helper.empty()) return;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0)) return;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + helper.wstring() + L"\" watch";
        const BOOL created = CreateProcessW(helper.c_str(), command.data(), nullptr,
                                            nullptr, TRUE, CREATE_NO_WINDOW,
                                            nullptr, nullptr, &startup, &process);
        CloseHandle(writeEnd);
        if (!created) {
            CloseHandle(readEnd);
            return;
        }
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                        &limits, sizeof(limits))) {
                AssignProcessToJobObject(job, process.hProcess);
            }
            job_ = job;
        }
        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this, readEnd] { ReaderMain(readEnd); });
    }

    void ReaderMain(HANDLE pipe) noexcept {
        std::string pending;
        char buffer[8192]{};
        bool discard = false;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
            for (DWORD i = 0; i < read; ++i) {
                if (buffer[i] == '\n') {
                    if (!discard) ParseLine(pending);
                    pending.clear();
                    discard = false;
                } else if (!discard) {
                    if (pending.size() >= metadata::kMaxTrackLineBytes) {
                        pending.clear();
                        discard = true;
                    } else {
                        pending.push_back(buffer[i]);
                    }
                }
            }
        }
        CloseHandle(pipe);
        { std::lock_guard lock(mutex_); info_ = {}; haveLine_ = false; }
        running_.store(false, std::memory_order_release);
    }

    void ParseLine(std::string line) noexcept {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "PULSE2") {
            std::lock_guard lock(mutex_);
            if (haveLine_) info_.stampTick = GetTickCount64();
            return;
        }
        if (line.rfind("POS2\t", 0) == 0) {
            std::uint32_t posMs = 0;
            if (metadata::detail::ParseUnsigned(std::string_view(line).substr(5), posMs)) {
                std::lock_guard lock(mutex_);
                const std::uint64_t now = GetTickCount64();
                if (!info_.hasPosition || info_.positionMs != posMs) {
                    info_.positionMs = posMs;
                    info_.positionStampTick = now;
                    info_.hasPosition = true;
                }
            }
            return;
        }
        metadata::TrackSnapshot track;
        if (!metadata::ParseTrackLine(line, track)) return;
        std::lock_guard lock(mutex_);
        const bool trackChanged = !haveLine_ || !metadata::SameTrack(info_, track);
        static_cast<metadata::TrackSnapshot&>(info_) = std::move(track);
        const std::uint64_t now = GetTickCount64();
        info_.stampTick = now;
        info_.valid = !info_.title.empty();
        if (trackChanged) {
            info_.positionMs = 0;
            info_.positionStampTick = now;
            info_.hasPosition = false;
        }
        haveLine_ = true;
    }

    std::mutex startMutex_;
    std::atomic<bool> running_{false};
    std::uint64_t retryAfter_{};
    HANDLE process_{};
    HANDLE job_{};
    std::thread reader_;
    mutable std::mutex mutex_;
    Info info_{};
    bool haveLine_{false};
};

} // namespace

RaatSink::~RaatSink() { Close(); }

void RaatSink::NotifyNextTrack() noexcept {
    metadataRefreshRequested_.store(true, std::memory_order_release);
}

void RaatSink::CancelNaturalEndOfStreamDelayForSuccessor() noexcept {
    successorPending_.store(true, std::memory_order_release);
}

void RaatSink::SetVolume(float volume) noexcept {
    pendingVolume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
    volumeDirty_.store(true, std::memory_order_release);
    volumeCv_.notify_one();
}

void RaatSink::SetMute(bool mute) noexcept {
    pendingMute_.store(mute, std::memory_order_release);
    muteDirty_.store(true, std::memory_order_release);
    volumeCv_.notify_one();
}

bool RaatSink::SupportsHardwareVolume() const noexcept {
    return volumeSupported_.load(std::memory_order_acquire);
}

void RaatSink::StartVolumeWorker() noexcept {
    StopVolumeWorker();
    stopVolumeWorker_.store(false, std::memory_order_release);
    volumeWorker_ = std::thread(&RaatSink::VolumeWorkerMain, this);
}

void RaatSink::StopVolumeWorker() noexcept {
    stopVolumeWorker_.store(true, std::memory_order_release);
    volumeCv_.notify_all();
    if (volumeWorker_.joinable()) {
        volumeWorker_.join();
    }
}

void RaatSink::VolumeWorkerMain() noexcept {
    while (!stopVolumeWorker_.load(std::memory_order_acquire)) {
        std::unique_lock lock(volumeWorkerMutex_);
        volumeCv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return stopVolumeWorker_.load(std::memory_order_acquire) ||
                   volumeDirty_.load(std::memory_order_acquire) ||
                   muteDirty_.load(std::memory_order_acquire);
        });

        if (stopVolumeWorker_.load(std::memory_order_acquire)) break;

        const std::uint64_t nowMs = GetTickCount64();
        const std::uint64_t lastMs = lastVolumeSendMs_.load(std::memory_order_acquire);
        if (nowMs >= lastMs && (nowMs - lastMs) < 60ull) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60ull - (nowMs - lastMs)));
        }

        if (stopVolumeWorker_.load(std::memory_order_acquire)) break;

        if (muteDirty_.exchange(false, std::memory_order_acq_rel)) {
            const bool mute = pendingMute_.load(std::memory_order_acquire);
            std::string req = "{\"request\":\"update_volume\",\"mute\":" +
                              std::string(mute ? "true" : "false") + "}";
            std::uint32_t rid = 0;
            std::lock_guard ctrlLock(controlMutex_);
            if (control_ && control_->IsOpen()) {
                control_->SendAsync(req, 500, rid);
                lastVolumeSendMs_.store(GetTickCount64(), std::memory_order_release);
            }
        }

        if (volumeDirty_.exchange(false, std::memory_order_acq_rel)) {
            const float V = pendingVolume_.load(std::memory_order_acquire);
            RaatVolumeCaps caps;
            {
                std::lock_guard vLock(volumeMutex_);
                caps = volumeCaps_;
            }

            if (caps.isSupported && caps.step > 0.0) {
                double targetVal = 0.0;
                if (caps.type == "db") {
                    const double span = caps.max - caps.min;
                    const double db = caps.min + span * std::pow(static_cast<double>(V), 2.0);
                    targetVal = std::round(db / caps.step) * caps.step;
                    if (targetVal < caps.min) targetVal = caps.min;
                    if (targetVal > caps.max) targetVal = caps.max;
                } else if (caps.type == "number") {
                    const double span = caps.max - caps.min;
                    const double num = caps.min + span * static_cast<double>(V);
                    targetVal = std::round(num / caps.step) * caps.step;
                    if (targetVal < caps.min) targetVal = caps.min;
                    if (targetVal > caps.max) targetVal = caps.max;
                } else {
                    const double span = caps.max - caps.min;
                    const double num = caps.min + span * static_cast<double>(V);
                    targetVal = std::round(num / caps.step) * caps.step;
                    if (targetVal < caps.min) targetVal = caps.min;
                    if (targetVal > caps.max) targetVal = caps.max;
                }

                char valBuf[32]{};
                std::snprintf(valBuf, sizeof(valBuf), "%.2f", targetVal);
                std::string valStr = valBuf;
                while (valStr.find('.') != std::string::npos &&
                       (valStr.back() == '0' || valStr.back() == '.')) {
                    const bool dot = (valStr.back() == '.');
                    valStr.pop_back();
                    if (dot) break;
                }

                std::string req = "{\"request\":\"update_volume\",\"value\":" + valStr + "}";
                std::uint32_t rid = 0;
                std::lock_guard ctrlLock(controlMutex_);
                if (control_ && control_->IsOpen()) {
                    control_->SendAsync(req, 500, rid);
                    lastVolumeSendMs_.store(GetTickCount64(), std::memory_order_release);
                }
            }
        }
    }
}

double RaatSink::RequestedBufferSeconds() const noexcept {
    if (deviceVendor_.find("Data Conversion Systems") != std::string::npos) {
        return format_.sampleRate >= 352800u ? 2.0 : 4.0;
    }
    const double configured = static_cast<double>(lookaheadMs_) / 1000.0;
    return std::clamp(configured, 1.0, 10.0);
}

void RaatSink::TeardownControlStream(raat::ControlChannel& control,
                                     std::uint32_t setupRid,
                                     bool waitTeardown,
                                     bool sendStop,
                                     bool sendEndStream) noexcept {
    if (!control.IsOpen()) return;
    std::atomic<bool> tornDown{false};
    std::atomic<std::uint32_t> teardownRidAtomic{0};
    if (waitTeardown) {
        control.SetEventCallback(
            [setupRid, &tornDown, &teardownRidAtomic](
                std::uint32_t rid, const std::string& json, bool isFinal) {
                (void)isFinal;
                const std::string status = JsonStatus(json);
                if ((setupRid != 0 && rid == setupRid &&
                     (status == "Teardown" || status == "Lost")) ||
                    (rid != 0 && rid == teardownRidAtomic.load(std::memory_order_acquire))) {
                    tornDown.store(true, std::memory_order_release);
                }
            });
    }
    std::string resp;
    bool fin = false;
    std::wstring err;
    if (sendStop) {
        (void)control.Request("{\"request\":\"stop\"}", 1500, resp, fin, err);
    }
    if (sendEndStream) {
        (void)control.Request("{\"request\":\"end_stream\"}", 1500, resp, fin, err);
    }
    std::uint32_t teardownRid = 0;
    if (control.SendAsync("{\"request\":\"teardown\"}", 1000, teardownRid) && waitTeardown) {
        teardownRidAtomic.store(teardownRid, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(1200);
        while (!tornDown.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline &&
               control.IsOpen()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (waitTeardown) {
        control.SetEventCallback({});
    }
}

void RaatSink::DiscardParkedSession() noexcept {
    std::unique_ptr<ParkedSession> parked;
    {
        std::lock_guard lock(s_gaplessMutex);
        parked = std::move(s_gaplessParking);
    }
    if (!parked) return;
    if (parked->audio) parked->audio->Close();
    if (parked->control && parked->control->IsOpen()) {
        if (parked->audioLive) {
            TeardownControlStream(*parked->control, parked->setupRid, true);
        }
        parked->control->Close();
    }
}

void RaatSink::ParkSessionForGapless() noexcept {
    if (!control_ || !control_->IsOpen() || !audio_ || !audio_->IsOpen()) return;
    control_->SetEventCallback({});
    auto parked = std::make_unique<ParkedSession>();
    parked->control = std::move(control_);
    parked->audio = std::move(audio_);
    parked->audioLive = true;
    parked->target = target_;
    parked->format = format_;
    parked->wireBits = wireBits_;
    parked->streamId = streamId_;
    parked->sampleIndex = sampleIndex_;
    parked->audioPort = audioPort_;
    parked->deviceClockPort = deviceClockPort_;
    parked->setupRid = setupRid_.load(std::memory_order_acquire);
    parked->streamRid = streamRid_.load(std::memory_order_acquire);
    parked->transportSubRid = transportSubRid_.load(std::memory_order_acquire);
    parked->controlsSubRid = controlsSubRid_.load(std::memory_order_acquire);
    parked->lastSentChmap = lastSentChmap_;
    parked->expectedStreamSample = expectedStreamSample_;
    parked->deviceVendor = deviceVendor_;
    parked->hasWriteChmap = hasWriteChmap_;
    parked->deviceClockOffsetNs = deviceClockOffsetNs_.load(std::memory_order_acquire);
    parked->deviceOutputDelayNs = deviceOutputDelayNs_.load(std::memory_order_acquire);
    parked->clockFirstLocalNs = clockFirstLocalNs_;
    parked->clockFirstOffsetNs = clockFirstOffsetNs_;
    parked->prefilledMs = prefilledMs_;
    parked->startLeadMs = startLeadMs_;
    parked->parkedAtMs = GetTickCount64();

    DiscardParkedSession();
    {
        std::lock_guard lock(s_gaplessMutex);
        s_gaplessParking = std::move(parked);
    }
    LogRaat(L"gapless session parked streamId=" + std::to_wstring(streamId_) +
            L" sample=" + std::to_wstring(sampleIndex_));
}

void RaatSink::ParkControlForReuse() noexcept {
    if (!control_ || !control_->IsOpen()) return;
    {
        // Notify endpoint display that playback is stopped while keeping the
        // control TCP connection + Lua VM + 2s/10s binary keepalive resident.
        std::uint32_t rid = 0;
        (void)control_->SendAsync(
            "{\"request\":\"update_transport\",\"quiet\":true,\"status\":{"
            "\"state\":\"stopped\",\"seek\":0,\"seek_position\":0,"
            "\"is_previous_allowed\":false,\"is_next_allowed\":false,"
            "\"is_play_allowed\":false,\"is_pause_allowed\":false,"
            "\"is_seek_allowed\":false,\"loop\":\"disabled\",\"shuffle\":false}}",
            500, rid);
    }
    control_->SetEventCallback({});
    auto parked = std::make_unique<ParkedSession>();
    parked->control = std::move(control_);
    parked->audio.reset();
    parked->audioLive = false;
    parked->target = target_;
    parked->format = {};
    parked->wireBits = 0;
    parked->streamId = 0;
    parked->sampleIndex = 0;
    parked->audioPort = 0;
    parked->deviceClockPort = 0;
    parked->setupRid = 0;
    parked->streamRid = 0;
    parked->transportSubRid = transportSubRid_.load(std::memory_order_acquire);
    parked->controlsSubRid = controlsSubRid_.load(std::memory_order_acquire);
    parked->deviceVendor = deviceVendor_;
    parked->hasWriteChmap = hasWriteChmap_;
    parked->parkedAtMs = GetTickCount64();

    DiscardParkedSession();
    {
        std::lock_guard lock(s_gaplessMutex);
        s_gaplessParking = std::move(parked);
    }
    LogRaat(L"control session parked for Lua VM reuse " + target_.address + L":" +
            std::to_wstring(target_.controlPort));
}

RaatSink::AdoptResult RaatSink::TryAdoptParkedSession() noexcept {
    std::unique_ptr<ParkedSession> parked;
    bool fullGapless = false;
    {
        std::lock_guard lock(s_gaplessMutex);
        if (!s_gaplessParking) return AdoptResult::None;
        const auto& p = *s_gaplessParking;
        const std::uint64_t ageMs = GetTickCount64() - p.parkedAtMs;
        const bool samePort =
            target_.controlPort == 0 || p.target.controlPort == 0 ||
            target_.controlPort == p.target.controlPort;
        const bool sameDevice =
            samePort &&
            ((!target_.deviceId.empty() && target_.deviceId == p.target.deviceId) ||
             (!target_.address.empty() && target_.address == p.target.address));
        const bool sameFormat =
            p.audioLive &&
            p.format.sampleRate == format_.sampleRate &&
            p.format.channels == format_.channels &&
            p.wireBits == wireBits_;
        // Official Roon parity: no idle expiration! ControlChannel's 2s/10s
        // binary LL_KEEPALIVE keeps the session alive indefinitely (hours/days)
        // and automatically marks IsOpen()=false if the endpoint drops/reboots.
        const bool controlAlive = p.control && p.control->IsOpen();
        const bool audioAlive = p.audioLive && p.audio && p.audio->IsOpen() &&
                                ageMs <= kRaatGaplessParkMaxAgeMs;
        if (sameDevice && sameFormat && controlAlive && audioAlive) {
            parked = std::move(s_gaplessParking);
            fullGapless = true;
        } else if (sameDevice && controlAlive) {
            parked = std::move(s_gaplessParking);
            fullGapless = false;
        }
    }
    if (!parked) {
        // Different device or expired/closed: tear down parked session cleanly.
        DiscardParkedSession();
        return AdoptResult::None;
    }

    if (fullGapless) {
        control_ = std::move(parked->control);
        audio_ = std::move(parked->audio);
        target_.controlPort = parked->target.controlPort;
        target_.address = parked->target.address;
        target_.raatVersion = parked->target.raatVersion;
        streamId_ = parked->streamId;
        sampleIndex_ = parked->sampleIndex;
        audioPort_ = parked->audioPort;
        deviceClockPort_ = parked->deviceClockPort;
        setupRid_.store(parked->setupRid, std::memory_order_release);
        streamRid_.store(parked->streamRid, std::memory_order_release);
        transportSubRid_.store(parked->transportSubRid, std::memory_order_release);
        controlsSubRid_.store(parked->controlsSubRid, std::memory_order_release);
        lastSentChmap_ = parked->lastSentChmap;
        expectedStreamSample_ = parked->expectedStreamSample;
        streamSampleInitialized_ = true;
        deviceVendor_ = std::move(parked->deviceVendor);
        hasWriteChmap_ = parked->hasWriteChmap;
        deviceClockOffsetNs_.store(parked->deviceClockOffsetNs, std::memory_order_release);
        deviceOutputDelayNs_.store(parked->deviceOutputDelayNs, std::memory_order_release);
        clockFirstLocalNs_ = parked->clockFirstLocalNs;
        clockFirstOffsetNs_ = parked->clockFirstOffsetNs;
        prefilledMs_ = parked->prefilledMs;
        teardownObserved_.store(false, std::memory_order_release);
        clockLocked_.store(parked->deviceClockOffsetNs != 0, std::memory_order_release);
        localAddress_ = control_->LocalAddress();

        control_->SetEventCallback(
            [this](std::uint32_t rid, const std::string& json, bool isFinal) {
                OnControlEvent(rid, json, isFinal);
            });
        (void)StartClockThread(true);
        StartAudioSender();
        s_lastRaatSampleRate.store(format_.sampleRate, std::memory_order_release);
        LogRaat(L"gapless session adopted seamlessly streamId=" +
                std::to_wstring(streamId_) + L" sample=" + std::to_wstring(sampleIndex_));
        return AdoptResult::FullGapless;
    }

    // Same device, different format (or previous stream already stopped):
    // If previous track ended naturally (audioLive==true), let its buffered tail
    // finish playing via end_stream + remaining drain before teardown (no abrupt stop!).
    if (parked->format.sampleRate > 0) {
        s_lastRaatSampleRate.store(parked->format.sampleRate, std::memory_order_release);
    }
    if (parked->audioLive && parked->control && parked->control->IsOpen()) {
        std::string resp;
        bool fin = false;
        std::wstring err;
        (void)parked->control->Request("{\"request\":\"end_stream\"}", 1500, resp, fin, err);
        const std::uint64_t outputDelayMs = parked->deviceOutputDelayNs / 1000000ull;
        const std::uint64_t totalDrainMs = std::clamp<std::uint64_t>(
            static_cast<std::uint64_t>(parked->prefilledMs ? parked->prefilledMs : 1000u) +
                parked->startLeadMs + outputDelayMs + 250ull,
            400ull, 6000ull);
        const std::uint64_t elapsedMs = GetTickCount64() - parked->parkedAtMs;
        if (elapsedMs < totalDrainMs) {
            const std::uint64_t waitMs = totalDrainMs - elapsedMs;
            LogRaat(L"cross-format natural EOS tail drain " + std::to_wstring(waitMs) + L"ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        }
        if (parked->audio) parked->audio->Close();
        TeardownControlStream(*parked->control, parked->setupRid, true, false, false);
    } else if (parked->audio) {
        parked->audio->Close();
    }
    if (!parked->control || !parked->control->IsOpen()) {
        return AdoptResult::None;
    }

    control_ = std::move(parked->control);
    audio_.reset();
    target_.controlPort = parked->target.controlPort;
    target_.address = parked->target.address;
    target_.raatVersion = parked->target.raatVersion;
    transportSubRid_.store(parked->transportSubRid, std::memory_order_release);
    controlsSubRid_.store(parked->controlsSubRid, std::memory_order_release);
    deviceVendor_ = std::move(parked->deviceVendor);
    hasWriteChmap_ = parked->hasWriteChmap;
    localAddress_ = control_->LocalAddress();

    control_->SetEventCallback(
        [this](std::uint32_t rid, const std::string& json, bool isFinal) {
            OnControlEvent(rid, json, isFinal);
        });
    LogRaat(L"reusing persistent control session & Lua VM on " +
            target_.address + L":" + std::to_wstring(target_.controlPort));
    return AdoptResult::ControlOnly;
}

void RaatSink::PrewarmFromIniAsync() noexcept {
    if (s_prewarmInFlight.exchange(true, std::memory_order_acq_rel)) return;
    bool spawned = false;
    struct PrewarmLaunchGuard {
        bool& launched;
        ~PrewarmLaunchGuard() {
            if (!launched) s_prewarmInFlight.store(false, std::memory_order_release);
        }
    } launchGuard{spawned};
    const std::wstring ini = IniPath();
    wchar_t outputBuf[64]{};
    GetPrivateProfileStringW(L"mod", L"output", L"", outputBuf,
                             static_cast<DWORD>(std::size(outputBuf)), ini.c_str());
    const std::wstring output(outputBuf);
    if (output != L"r_device" && output != L"roon") return;

    auto readSec = [&](const wchar_t* key) -> std::wstring {
        for (const wchar_t* sec : {L"r_device", L"roon"}) {
            wchar_t buf[256]{};
            const DWORD n = GetPrivateProfileStringW(
                sec, key, L"", buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
            if (n > 0) return std::wstring(buf, n);
        }
        return {};
    };

    RaatTarget target{};
    target.deviceId = readSec(L"device_id");
    target.friendlyName = readSec(L"device_name");
    target.address = readSec(L"address");
    target.raatVersion = readSec(L"raat_version");
    const std::wstring portStr = readSec(L"port");
    if (!portStr.empty()) {
        const long parsed = std::wcstol(portStr.c_str(), nullptr, 10);
        if (parsed > 0 && parsed <= 65535) {
            target.controlPort = static_cast<std::uint16_t>(parsed);
        }
    }
    if (target.deviceId.empty() && target.address.empty()) return;

    spawned = true;
    std::thread([target]() noexcept {
        struct InFlightGuard {
            ~InFlightGuard() { s_prewarmInFlight.store(false, std::memory_order_release); }
        } inFlightGuard;
        if (s_activeStartCount.load(std::memory_order_acquire) > 0) return;
        {
            std::lock_guard lock(s_gaplessMutex);
            if (s_gaplessParking && s_gaplessParking->control &&
                s_gaplessParking->control->IsOpen()) {
                return;
            }
        }
        if (!EnsureWinsock()) return;

        RaatSink warm;
        warm.target_ = target;
        warm.format_.sampleRate = 44100;
        warm.format_.channels = 2;
        warm.wireBits_ = 16;
        (void)warm.ReadSettingString(warm.bindInterface_, L"bind_interface");
        warm.control_ = std::make_unique<raat::ControlChannel>();

        // Refresh port via SOOD in case the device rebooted while Apple Music was closed.
        {
            RaatDevice fresh;
            std::wstring qerr;
            if (RaatDiscovery::QueryOnce(warm.target_.deviceId, 1200, fresh, qerr,
                                         warm.target_.address) &&
                fresh.controlPort != 0) {
                warm.target_.controlPort = fresh.controlPort;
                if (!fresh.address.empty()) warm.target_.address = fresh.address;
                if (!fresh.raatVersion.empty()) warm.target_.raatVersion = fresh.raatVersion;
                if (warm.target_.friendlyName.empty()) warm.target_.friendlyName = fresh.friendlyName;
            }
        }
        if (s_activeStartCount.load(std::memory_order_acquire) > 0) return;

        std::wstring error;
        if (!warm.OpenControl(error)) {
            LogRaat(L"startup prewarm skipped (endpoint unreachable: " + error + L")");
            return;
        }
        if (s_activeStartCount.load(std::memory_order_acquire) > 0) {
            warm.control_->Close();
            return;
        }
        if (!warm.LoadScripts(error)) {
            LogRaat(L"startup prewarm scripts failed: " + error);
            warm.control_->Close();
            return;
        }
        (void)warm.SendInfo(error);
        warm.SendClientType();
        warm.SendSubscriptions();

        if (s_activeStartCount.load(std::memory_order_acquire) > 0) {
            warm.control_->Close();
            return;
        }
        {
            std::lock_guard lock(s_gaplessMutex);
            if (s_gaplessParking && s_gaplessParking->control &&
                s_gaplessParking->control->IsOpen()) {
                warm.control_->Close();
                return;
            }
        }
        warm.ParkControlForReuse();
        s_lastRaatSampleRate.store(44100, std::memory_order_release);
        LogRaat(L"startup prewarm ready: persistent control TCP + Lua VM active on " +
                warm.target_.address + L":" + std::to_wstring(warm.target_.controlPort));
    }).detach();
}

HRESULT RaatSink::Open(const AudioSinkOpenParams& params) noexcept {
    Close();
    if (!IsValidFormat(params.format)) return E_INVALIDARG;
    if (params.raat.deviceId.empty() && params.raat.address.empty()) {
        return ammod::audio::kRaatDeviceUnavailable;
    }

    control_ = std::make_unique<raat::ControlChannel>();
    audio_ = std::make_unique<raat::TcpConnection>();

    params_ = params;
    format_ = params.format;
    target_ = params.raat;
    source_ = nullptr;
    sourceReadyEvent_ = params.sourceReadyEvent;
    sampleIndex_ = 0;
    lastSendTick_ = 0;
    deviceVendor_.clear();
    hasWriteChmap_ = false;
    lastSentChmap_ = 0;
    expectedStreamSample_ = 0;
    streamSampleInitialized_ = false;
    successorPending_.store(false, std::memory_order_release);
    gaplessHandoffReady_.store(false, std::memory_order_release);
    metadataRefreshRequested_.store(false, std::memory_order_release);
    lastMetaTitle_.clear();
    lastMetaArtist_.clear();
    lastMetaAlbum_.clear();
    lastMetaDurationMs_ = 0;
    lastMetaSeekSec_ = UINT32_MAX;
    teardownObserved_.store(false, std::memory_order_release);
    lastMetaArtworkSize_ = 0;
    lastMetaArtworkHash_ = 0;
    lastMetaHasArtwork_ = false;
    lastMetaPlaying_ = false;
    lastMetaPollMs_ = 0;

    RaatTrackMonitor::Instance().EnsureStarted();

    const std::uint16_t sourceBits = EffectiveSourceValidBits(format_);
    int configuredSoodPort = 0;
    ReadSettingInt(configuredSoodPort, L"sood_port", 0);
    SetSoodPort(configuredSoodPort > 0 && configuredSoodPort <= 65535
                    ? static_cast<std::uint16_t>(configuredSoodPort) : 9003);

    bindInterface_.clear();
    (void)ReadSettingString(bindInterface_, L"bind_interface");
    SetBindInterface(bindInterface_);

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

    int configuredBufferMs = 0;
    if (ReadSettingInt(configuredBufferMs, L"buffer_ms",
                       static_cast<int>(kRaatDefaultLookaheadMs)) &&
        configuredBufferMs > 0) {
        lookaheadMs_ = static_cast<std::uint32_t>(configuredBufferMs);
    } else if (params.streamBufferMs > 0) {
        lookaheadMs_ = params.streamBufferMs;
    } else {
        lookaheadMs_ = kRaatDefaultLookaheadMs;
    }
    lookaheadMs_ = std::clamp<std::uint32_t>(lookaheadMs_, 1000u, 10000u);

    int lead = 0;
    ReadSettingInt(lead, L"start_lead_ms", static_cast<int>(kRaatDefaultStartLeadMs));
    startLeadMs_ = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(lead, 50)), 50u, 4000u);

    int configuredTestMode = 0;
    ReadSettingInt(configuredTestMode, L"test_mode", 0);
    testMode_ = (configuredTestMode != 0);

    int configuredBlockBytes = static_cast<int>(kRaatDefaultBlockSizeBytes);
    ReadSettingInt(configuredBlockBytes, L"block_size_bytes",
                   static_cast<int>(kRaatDefaultBlockSizeBytes));
    blockSizeBytes_ = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(configuredBlockBytes, 512)),
        512u, 131072u);

    int configuredSliceMs = 50;
    ReadSettingInt(configuredSliceMs, L"slice_ms", 50);
    sliceMs_ = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(configuredSliceMs, 10)), 10u, 200u);

    int debugPace = 0;
    ReadSettingInt(debugPace, L"debug_pace", 0);
    debugPaceLogging_ = (debugPace != 0);

    int configuredResyncMs = 200;
    ReadSettingInt(configuredResyncMs, L"resync_delay_ms", 200);
    resyncDelayMs_ = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(configuredResyncMs, 0)), 0u, 5000u);

    std::wstring displayModeSetting;
    if (ReadSettingString(displayModeSetting, L"display_mode") && !displayModeSetting.empty()) {
        if (_wcsicmp(displayModeSetting.c_str(), L"pure") == 0 || displayModeSetting == L"1") {
            displayMode_ = DisplayMode::Pure;
        } else {
            displayMode_ = DisplayMode::Standard;
        }
    } else {
        int pureInt = 0;
        ReadSettingInt(pureInt, L"pure_display", 0);
        displayMode_ = (pureInt == 1) ? DisplayMode::Pure : DisplayMode::Standard;
    }

    if (testMode_) {
        // Test mode forces Pure listening mode (zero periodic transport packets) and zero pace disk logging
        displayMode_ = DisplayMode::Pure;
        debugPaceLogging_ = false;
    }

    int intervalMs = 3000;
    ReadSettingInt(intervalMs, L"transport_interval_ms", 3000);
    transportIntervalMs_ = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(intervalMs, 500)), 500u, 10000u);

    const std::uint32_t bytesPerFrame =
        std::max<std::uint32_t>(1u, format_.channels * (wireBits_ / 8u));
    if (testMode_) {
        blockFrames_ = std::max<std::uint32_t>(16u, blockSizeBytes_ / bytesPerFrame);
    } else {
        const std::uint32_t sliceFrames = (format_.sampleRate > 0)
            ? static_cast<std::uint32_t>((static_cast<std::uint64_t>(format_.sampleRate) * sliceMs_) / 1000u)
            : 2048u;
        const std::uint32_t maxFramesByBytes = blockSizeBytes_ / bytesPerFrame;
        blockFrames_ = std::clamp<std::uint32_t>(sliceFrames, 256u, maxFramesByBytes);
    }
    block_.assign(static_cast<std::size_t>(blockFrames_) * format_.channels, 0);
    packedActive_.reserve(block_.size());

    streamId_ = static_cast<std::uint32_t>((GetTickCount64() & 0x7fffffffu) | 1u);
    lastError_.store(S_OK, std::memory_order_release);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    StartVolumeWorker();
    LogRaat(L"open device=" + target_.friendlyName + L" id=" + target_.deviceId +
            L" addr=" + target_.address + L":" + std::to_wstring(target_.controlPort) +
            L" raat=" + target_.raatVersion + L" rate=" +
            std::to_wstring(format_.sampleRate) + L" bits=" +
            std::to_wstring(wireBits_) + L" ch=" + std::to_wstring(format_.channels) +
            L" buffer=" + std::to_wstring(lookaheadMs_) + L"ms lead=" +
            std::to_wstring(startLeadMs_) + L"ms blockBytes=" +
            std::to_wstring(blockSizeBytes_) +
            (bindInterface_.empty() ? L"" : (L" bind=" + bindInterface_)));
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
    successorPending_.store(false, std::memory_order_release);
    gaplessHandoffReady_.store(false, std::memory_order_release);

    if (startedOnce_.load(std::memory_order_acquire) &&
        control_ && control_->IsOpen() &&
        audio_ && audio_->IsOpen() &&
        !audioSendFault_.load(std::memory_order_acquire)) {
        paused_.store(false, std::memory_order_release);
        paceStartRemoteNs_ = 0;
        streamSampleInitialized_ = false;
        if (!SendStart(true)) {
            RecordError(ammod::audio::kRaatControlFailed);
            return ammod::audio::kRaatControlFailed;
        }
        PushNowPlayingMetadata(true, true);
        state_.store(AudioSinkState::Running, std::memory_order_release);
        LogRaat(L"resume sample=" + std::to_wstring(sampleIndex_));
        return S_OK;
    }

    stop_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    startedOnce_.store(false, std::memory_order_release);
    tooManyDropouts_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(dropoutMutex_);
        dropoutWindow_.clear();
    }
    sampleIndex_ = 0;
    paceStartRemoteNs_ = 0;
    paceStartSample_ = 0;
    paceLastLogTick_ = 0;
    lastError_.store(S_OK, std::memory_order_release);

    s_activeStartCount.fetch_add(1, std::memory_order_acq_rel);
    struct ActiveStartGuard {
        ~ActiveStartGuard() { s_activeStartCount.fetch_sub(1, std::memory_order_acq_rel); }
    } activeStartGuard;

    const AdoptResult adopt = TryAdoptParkedSession();
    const bool adoptedGapless = (adopt == AdoptResult::FullGapless);
    const bool reuseControl = (adopt == AdoptResult::ControlOnly);
    if (!adoptedGapless && !EnsureSession(reuseControl)) {
        const HRESULT error = lastError_.load(std::memory_order_acquire);
        RecordError(error == S_OK ? ammod::audio::kRaatStreamFailed : error);
        StopAudioSender(true);
        StopClock();
        if (control_) control_->Close();
        if (audio_) audio_->Close();
        state_.store(AudioSinkState::Faulted, std::memory_order_release);
        return error == S_OK ? ammod::audio::kRaatStreamFailed : error;
    }

    trackStartSampleIndex_ = adoptedGapless ? sampleIndex_ : 0;
    PushNowPlayingMetadata(true, true);

    try {
        pumpThread_ = std::thread(&RaatSink::PumpMain, this);
    } catch (...) {
        RecordError(ammod::audio::kRaatStreamFailed);
        StopAudioSender(true);
        StopClock();
        if (control_) control_->Close();
        if (audio_) audio_->Close();
        return ammod::audio::kRaatStreamFailed;
    }
    startedOnce_.store(true, std::memory_order_release);
    state_.store(AudioSinkState::Running, std::memory_order_release);
    LogRaat(std::wstring(adoptedGapless ? L"gapless live " : L"session live ") +
            target_.address + L":" + std::to_wstring(target_.controlPort));
    return S_OK;
}

void RaatSink::Pause() noexcept {
    if (state_.load(std::memory_order_acquire) != AudioSinkState::Running) return;
    paused_.store(true, std::memory_order_release);
    if (control_ && control_->IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_->Request("{\"request\":\"stop\"}", 1500, response, final, error);
    }
    PushNowPlayingMetadata(true, false);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    LogRaat(L"pause");
}

void RaatSink::Stop() noexcept {
    stop_.store(true, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    if (sourceReadyEvent_) SetEvent(sourceReadyEvent_);
    if (pumpThread_.joinable()) pumpThread_.join();

    const bool canParkGapless =
        gaplessHandoffReady_.load(std::memory_order_acquire) &&
        !audioSendFault_.load(std::memory_order_acquire) &&
        !tooManyDropouts_.load(std::memory_order_acquire) &&
        control_ && control_->IsOpen() &&
        audio_ && audio_->IsOpen();

    StopAudioSender(!canParkGapless);
    StopClock();

    if (canParkGapless) {
        ParkSessionForGapless();
        startedOnce_.store(false, std::memory_order_release);
        if (state_.load(std::memory_order_acquire) != AudioSinkState::Faulted) {
            state_.store(AudioSinkState::Open, std::memory_order_release);
        }
        return;
    }

    {
        std::lock_guard lock(audioMutex_);
        if (audio_) audio_->Close();
    }

    const bool healthyControl =
        control_ && control_->IsOpen() &&
        !audioSendFault_.load(std::memory_order_acquire) &&
        !tooManyDropouts_.load(std::memory_order_acquire) &&
        state_.load(std::memory_order_acquire) != AudioSinkState::Faulted;

    const bool naturalEos = endOfStreamComplete_.load(std::memory_order_acquire);
    const bool hadControl = (control_ != nullptr);
    if (control_ && control_->IsOpen()) {
        std::lock_guard lock(controlMutex_);
        TeardownControlStream(*control_, setupRid_.load(std::memory_order_acquire),
                              true, !naturalEos);
    }
    if (healthyControl && control_ && control_->IsOpen()) {
        ParkControlForReuse();
    } else if (hadControl) {
        // Only discard parked session if THIS sink held an unhealthy control;
        // a redundant second Stop() with control_==nullptr must NOT wipe out
        // the healthy persistent control we just parked!
        DiscardParkedSession();
        control_->Close();
    }
    if (audio_) audio_->Close();
    startedOnce_.store(false, std::memory_order_release);
    sampleIndex_ = 0;
    if (state_.load(std::memory_order_acquire) != AudioSinkState::Faulted) {
        state_.store(AudioSinkState::Open, std::memory_order_release);
    }
    LogRaat(L"stop");
}

void RaatSink::Close() noexcept {
    StopVolumeWorker();
    stop_.store(true, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    if (sourceReadyEvent_) SetEvent(sourceReadyEvent_);
    if (pumpThread_.joinable()) pumpThread_.join();

    const bool canParkGapless =
        gaplessHandoffReady_.load(std::memory_order_acquire) &&
        !audioSendFault_.load(std::memory_order_acquire) &&
        !tooManyDropouts_.load(std::memory_order_acquire) &&
        control_ && control_->IsOpen() &&
        audio_ && audio_->IsOpen();

    StopAudioSender(!canParkGapless);
    StopClock();

    if (canParkGapless) {
        ParkSessionForGapless();
        startedOnce_.store(false, std::memory_order_release);
        source_ = nullptr;
        state_.store(AudioSinkState::Closed, std::memory_order_release);
        return;
    }

    {
        std::lock_guard lock(audioMutex_);
        if (audio_) audio_->Close();
    }

    const bool healthyControl =
        control_ && control_->IsOpen() &&
        !audioSendFault_.load(std::memory_order_acquire) &&
        !tooManyDropouts_.load(std::memory_order_acquire) &&
        state_.load(std::memory_order_acquire) != AudioSinkState::Faulted;

    const bool naturalEos = endOfStreamComplete_.load(std::memory_order_acquire);
    if (control_ && control_->IsOpen()) {
        std::lock_guard lock(controlMutex_);
        TeardownControlStream(*control_, setupRid_.load(std::memory_order_acquire),
                              true, !naturalEos);
    }
    if (healthyControl && control_ && control_->IsOpen()) {
        ParkControlForReuse();
    } else {
        if (control_) control_->Close();
    }
    if (audio_) audio_->Close();
    startedOnce_.store(false, std::memory_order_release);
    sampleIndex_ = 0;
    source_ = nullptr;
    state_.store(AudioSinkState::Closed, std::memory_order_release);
}

void RaatSink::DispatchFrontPanelControl(const std::string& controlAction) noexcept {
    if (controlAction.empty()) return;
    LogRaat(L"front-panel transport control=" + ToWide(controlAction));
    if (controlAction == "play" || controlAction == "pause" ||
        controlAction == "playpause" || controlAction == "play_pause") {
        SendMediaKey(VK_MEDIA_PLAY_PAUSE);
    } else if (controlAction == "next") {
        SendMediaKey(VK_MEDIA_NEXT_TRACK);
    } else if (controlAction == "previous" || controlAction == "prev") {
        SendMediaKey(VK_MEDIA_PREV_TRACK);
    } else if (controlAction == "stop") {
        SendMediaKey(VK_MEDIA_STOP);
    }
}

void RaatSink::OnControlEvent(std::uint32_t rid, const std::string& json,
                              bool isFinal) noexcept {
    (void)isFinal;
    const std::string status = JsonStatus(json);
    if (rid == setupRid_.load(std::memory_order_acquire)) {
        if (status == "OutputMessage") {
            if (json.find("signal_path") != std::string::npos) {
                LogRaat(L"signal_path=" + ToWide(json.substr(0, 320)));
            }
        } else if (status == "Teardown" || status == "Lost") {
            teardownObserved_.store(true, std::memory_order_release);
            if (status == "Lost") {
                LogRaat(L"endpoint reported output Lost");
            }
        }
    }
    if (rid == transportSubRid_.load(std::memory_order_acquire)) {
        const std::string ctrl = JsonGetString(json, "control");
        if (!ctrl.empty()) {
            DispatchFrontPanelControl(ctrl);
        }
    }
    if (rid == controlsSubRid_.load(std::memory_order_acquire)) {
        const std::size_t volPos = json.find("\"volume\"");
        if (volPos != std::string::npos && json.find("\"volume\":null") == std::string::npos) {
            const std::string volJson = json.substr(volPos);
            std::string type = JsonGetString(volJson, "type");
            double min = JsonGetDouble(volJson, "min", 0.0);
            double max = JsonGetDouble(volJson, "max", 100.0);
            double step = JsonGetDouble(volJson, "step", 1.0);
            double val = JsonGetDouble(volJson, "value", 100.0);
            bool mute = JsonGetBool(volJson, "mute", false);

            if (step <= 0.0 || step > 10.0) step = 1.0;
            if (type.empty()) type = (min < 0.0) ? "db" : "number";
            const bool supported = (type != "none");

            {
                std::lock_guard lock(volumeMutex_);
                volumeCaps_.isSupported = supported;
                volumeCaps_.type = type;
                volumeCaps_.min = min;
                volumeCaps_.max = max;
                volumeCaps_.step = step;
                volumeCaps_.value = val;
                volumeCaps_.mute = mute;
                volumeSupported_.store(supported, std::memory_order_release);
            }

            LogRaat(L"endpoint control volume caps: type=" + ToWide(type) +
                    L" min=" + std::to_wstring(min) +
                    L" max=" + std::to_wstring(max) +
                    L" step=" + std::to_wstring(step) +
                    L" val=" + std::to_wstring(val) +
                    L" mute=" + (mute ? L"1" : L"0"));
        } else if (json.find("\"controls\"") != std::string::npos) {
            std::lock_guard lock(volumeMutex_);
            volumeCaps_.isSupported = false;
            volumeSupported_.store(false, std::memory_order_release);
            LogRaat(L"endpoint does not advertise volume controls (fixed output)");
        }
    }
    if (rid == streamRid_.load(std::memory_order_acquire)) {
        if (status == "Ready" || status == "Playing") {
            streamReady_.store(true, std::memory_order_release);
        } else if (status == "Dropout") {
            const std::int64_t delta = JsonGetInt(json, "samples", 0);
            if (delta > 0) {
                droppedFrames_.fetch_add(static_cast<std::uint64_t>(delta),
                                         std::memory_order_relaxed);
                const std::uint64_t nowMs = GetTickCount64();
                std::uint64_t totalWindow = 0;
                {
                    std::lock_guard lock(dropoutMutex_);
                    dropoutWindow_.push_back(
                        DropoutRecord{nowMs, static_cast<std::uint64_t>(delta)});
                    while (!dropoutWindow_.empty() &&
                           nowMs > dropoutWindow_.front().tickMs &&
                           (nowMs - dropoutWindow_.front().tickMs) > 30000ull) {
                        dropoutWindow_.pop_front();
                    }
                    for (const auto& rec : dropoutWindow_) totalWindow += rec.samples;
                }
                LogRaat(L"endpoint dropout +" + std::to_wstring(delta) +
                        L" frames (30s total=" + std::to_wstring(totalWindow) + L")");
                if (totalWindow > static_cast<std::uint64_t>(format_.sampleRate) * 3ull) {
                    tooManyDropouts_.store(true, std::memory_order_release);
                    LogRaat(L"too many dropouts (>3s in last 30s); triggering recovery");
                }
            }
        }
    }
}

void RaatSink::PushNowPlayingMetadata(bool force, bool isPlaying) noexcept {
    if (!control_ || !control_->IsOpen()) return;
    const auto info = RaatTrackMonitor::Instance().Snapshot(force ? 120u : 0u);
    if (!info.valid && !force) return;

    const std::uint64_t nowMs = GetTickCount64();
    const std::uint32_t durationSec = info.durationMs / 1000u;
    std::uint32_t seekPosSec = 0;
    if (info.hasPosition) {
        std::uint64_t estMs = info.positionMs;
        if (isPlaying && nowMs > info.positionStampTick) {
            estMs += (nowMs - info.positionStampTick);
        }
        seekPosSec = static_cast<std::uint32_t>(estMs / 1000ull);
    } else if (format_.sampleRate > 0 && sampleIndex_ >= trackStartSampleIndex_) {
        const std::uint64_t qFrames = queuedFrames_.load(std::memory_order_acquire);
        const std::uint64_t trackFrames = sampleIndex_ - trackStartSampleIndex_;
        const std::uint64_t playedFrames = trackFrames > qFrames ? (trackFrames - qFrames) : 0;
        seekPosSec = static_cast<std::uint32_t>(playedFrames / format_.sampleRate);
    }
    if (durationSec > 0 && seekPosSec > durationSec) seekPosSec = durationSec;

    const std::uint32_t artHash = info.artwork.empty() ? 0u : Fnv1a32(info.artwork);
    const bool trackOrStateChanged =
        force ||
        info.title != lastMetaTitle_ ||
        info.artist != lastMetaArtist_ ||
        info.album != lastMetaAlbum_ ||
        info.durationMs != lastMetaDurationMs_ ||
        isPlaying != lastMetaPlaying_;
    const bool seekChanged = (seekPosSec != lastMetaSeekSec_);
    const bool statusChanged = trackOrStateChanged || seekChanged;
    const bool artChanged =
        !info.artwork.empty() &&
        info.artwork.size() <= kRaatMaxArtworkBytes &&
        (force || info.artwork.size() != lastMetaArtworkSize_ ||
         artHash != lastMetaArtworkHash_);
    const bool artClear =
        info.valid && info.artwork.empty() && (force || lastMetaHasArtwork_);

    if (!statusChanged && !artChanged && !artClear) return;

    if (statusChanged && info.valid) {
        if (!trackOrStateChanged) {
            std::ostringstream json;
            json << "{\"request\":\"update_transport\",\"quiet\":true,\"status\":{"
                 << "\"state\":\"" << (isPlaying ? "playing" : "paused") << "\""
                 << ",\"seek\":" << seekPosSec
                 << ",\"seek_position\":" << seekPosSec
                 << "}}";
            std::lock_guard lock(controlMutex_);
            if (control_ && control_->IsOpen()) {
                std::uint32_t rid = 0;
                (void)control_->SendAsync(json.str(), 1000, rid);
            }
            lastMetaSeekSec_ = seekPosSec;
            lastMetaPlaying_ = isPlaying;
            return;
        }

        const std::string titleUtf8 = ToUtf8(info.title);
        const std::string artistUtf8 = ToUtf8(info.artist);
        const std::string albumUtf8 = ToUtf8(info.album);
        const std::string oneLine =
            (!titleUtf8.empty() && !artistUtf8.empty())
                ? (titleUtf8 + " - " + artistUtf8)
                : (!titleUtf8.empty() ? titleUtf8 : artistUtf8);
        const std::string escTitle = JsonEscape(titleUtf8);
        const std::string escArtist = JsonEscape(artistUtf8);
        const std::string escAlbum = JsonEscape(albumUtf8);
        const std::string escOneLine = JsonEscape(oneLine);

        const std::uint32_t sRate = (format_.sampleRate > 0) ? format_.sampleRate : 44100;
        const std::uint32_t bSample = (wireBits_ > 0) ? wireBits_ : ((format_.validBits > 0) ? format_.validBits : 16);
        const std::uint32_t nCh = (format_.channels > 0) ? format_.channels : 2;

        std::ostringstream json;
        json << "{\"request\":\"update_transport\",\"quiet\":true,\"status\":{"
             << "\"state\":\"" << (isPlaying ? "playing" : "paused") << "\""
             << ",\"seek\":" << seekPosSec
             << ",\"seek_position\":" << seekPosSec
             << ",\"is_previous_allowed\":true"
             << ",\"is_next_allowed\":true"
             << ",\"is_play_allowed\":" << (isPlaying ? "false" : "true")
             << ",\"is_pause_allowed\":" << (isPlaying ? "true" : "false")
             << ",\"is_seek_allowed\":false"
             << ",\"loop\":\"disabled\""
             << ",\"shuffle\":false"
             << ",\"stream_format\":{"
             << "\"sample_type\":\"pcm\","
             << "\"sample_rate\":" << sRate << ","
             << "\"bits_per_sample\":" << bSample << ","
             << "\"channels\":" << nCh
             << "}"
             << ",\"title\":\"" << escTitle << "\""
             << ",\"artist\":\"" << escArtist << "\""
             << ",\"album\":\"" << escAlbum << "\"";
        if (durationSec > 0) {
            json << ",\"length\":" << durationSec
                 << ",\"duration\":" << durationSec;
        }
        json << ",\"now_playing\":{"
             << "\"title\":\"" << escTitle << "\""
             << ",\"artist\":\"" << escArtist << "\""
             << ",\"album\":\"" << escAlbum << "\"";
        if (durationSec > 0) {
            json << ",\"length\":" << durationSec;
        }
        json << ",\"seek\":" << seekPosSec
             << ",\"seek_position\":" << seekPosSec
             << ",\"one_line\":\"" << escOneLine << "\""
             << ",\"two_line_title\":\"" << escTitle << "\""
             << ",\"two_line_subtitle\":\"" << escArtist << "\""
             << ",\"three_line_title\":\"" << escTitle << "\""
             << ",\"three_line_subtitle\":\"" << escArtist << "\""
             << ",\"three_line_subsubtitle\":\"" << escAlbum << "\""
             << ",\"two_line\":{\"line1\":\"" << escTitle << "\",\"line2\":\"" << escArtist << "\"}"
             << ",\"three_line\":{\"line1\":\"" << escTitle << "\",\"line2\":\"" << escArtist << "\",\"line3\":\"" << escAlbum << "\"}"
             << "}}}";
        std::lock_guard lock(controlMutex_);
        if (control_ && control_->IsOpen()) {
            std::uint32_t rid = 0;
            (void)control_->SendAsync(json.str(), 1000, rid);
        }
        lastMetaTitle_ = info.title;
        lastMetaArtist_ = info.artist;
        lastMetaAlbum_ = info.album;
        lastMetaDurationMs_ = info.durationMs;
        lastMetaSeekSec_ = seekPosSec;
        lastMetaPlaying_ = isPlaying;
        if (trackOrStateChanged) {
            LogRaat(L"update_transport title=\"" + info.title + L"\" state=" +
                    (isPlaying ? L"playing" : L"paused") +
                    L" pos=" + std::to_wstring(seekPosSec) + L"/" +
                    std::to_wstring(durationSec) + L"s");
        }
    }

    if (artChanged) {
        std::string mime = info.artworkMime.empty()
                               ? "image/jpeg"
                               : ToUtf8(info.artworkMime);
        std::vector<std::uint8_t> scaledJpeg;
        const bool didScale = ScaleArtworkToMax500Jpeg(info.artwork, mime, scaledJpeg);
        const std::vector<std::uint8_t>& wireArt = didScale ? scaledJpeg : info.artwork;
        if (didScale) mime = "image/jpeg";
        std::ostringstream json;
        json << "{\"request\":\"update_artwork\",\"mimetype\":\""
             << JsonEscape(mime) << "\"}";
        std::lock_guard lock(controlMutex_);
        if (control_ && control_->IsOpen()) {
            if (control_->SendArtwork(wireArt.data(), wireArt.size(), 2000)) {
                std::uint32_t rid = 0;
                (void)control_->SendAsync(json.str(), 1000, rid);
                lastMetaArtworkSize_ = info.artwork.size();
                lastMetaArtworkHash_ = artHash;
                lastMetaHasArtwork_ = true;
                LogRaat(L"update_artwork bytes=" +
                        std::to_wstring(wireArt.size()) +
                        (didScale ? (L" (scaled from " + std::to_wstring(info.artwork.size()) + L")") : L"") +
                        L" mime=" + ToWide(mime));
            }
        }
    } else if (artClear) {
        std::lock_guard lock(controlMutex_);
        if (control_ && control_->IsOpen()) {
            std::uint32_t rid = 0;
            (void)control_->SendAsync("{\"request\":\"update_artwork\"}", 1000, rid);
            lastMetaArtworkSize_ = 0;
            lastMetaArtworkHash_ = 0;
            lastMetaHasArtwork_ = false;
            LogRaat(L"update_artwork cleared");
        }
    }
}

bool RaatSink::StartClockThread(bool preserveBaseline) noexcept {
    StopClock();
    clockPort_ = 0;
    if (!preserveBaseline || clockFirstLocalNs_ == 0) {
        clockFirstLocalNs_ = 0;
        clockFirstOffsetNs_ = 0;
    }
    clockLastTraceMs_ = GetTickCount64();
    clockProbesIn_.store(0, std::memory_order_relaxed);
    clockRepliesOut_.store(0, std::memory_order_relaxed);
    clockStop_.store(false, std::memory_order_release);

    clockSocket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (clockSocket_ == INVALID_SOCKET) return false;

    int dscp = 0xB8; // DSCP EF (46) << 2
    (void)::setsockopt(clockSocket_, IPPROTO_IP, IP_TOS,
                       reinterpret_cast<const char*>(&dscp), sizeof(dscp));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    if (!bindInterface_.empty()) {
        const std::string bindUtf8 = ToUtf8(bindInterface_);
        (void)inet_pton(AF_INET, bindUtf8.c_str(), &local.sin_addr);
    }
    local.sin_port = 0;
    if (bind(clockSocket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0) {
        sockaddr_in bound{};
        int length = sizeof(bound);
        if (getsockname(clockSocket_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
            clockPort_ = ntohs(bound.sin_port);
        }
    }
    (void)RunDeviceClockSyncRound();
    try {
        clockThread_ = std::thread(&RaatSink::ClockMain, this);
        return true;
    } catch (...) {
        clockStop_.store(true, std::memory_order_release);
        return false;
    }
}

bool RaatSink::EnsureSession(bool reuseControl) noexcept {
    std::wstring error;
    if (!EnsureWinsock()) {
        lastError_.store(ammod::audio::kRaatControlFailed, std::memory_order_release);
        return false;
    }
    if (!control_) control_ = std::make_unique<raat::ControlChannel>();
    if (!audio_) audio_ = std::make_unique<raat::TcpConnection>();

    if (!reuseControl) {
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

        control_->SetEventCallback(
            [this](std::uint32_t rid, const std::string& json, bool isFinal) {
                OnControlEvent(rid, json, isFinal);
            });

        if (!OpenControl(error)) {
            LogRaat(L"control open failed: " + error);
            lastError_.store(target_.address.empty() ? ammod::audio::kRaatDeviceUnavailable
                                                     : ammod::audio::kRaatControlFailed,
                             std::memory_order_release);
            return false;
        }

        if (!LoadScripts(error)) {
            LogRaat(L"scripts failed: " + error);
            lastError_.store(ammod::audio::kRaatScriptRejected, std::memory_order_release);
            return false;
        }

        if (!SendInfo(error)) {
            LogRaat(L"info check rejected format: " + error);
            lastError_.store(ammod::audio::kRaatControlFailed, std::memory_order_release);
            return false;
        }
        SendClientType();
        SendSubscriptions();
    }

    SendRequestSource();

    const std::uint32_t prevRate = s_lastRaatSampleRate.exchange(format_.sampleRate, std::memory_order_acq_rel);
    const bool crossFamily = (prevRate > 0 && IsDifferentClockFamily(prevRate, format_.sampleRate));
    const std::uint32_t effectiveDelay = crossFamily ? resyncDelayMs_ : 0u;
    if (effectiveDelay > 0) {
        LogRaat(L"cross clock-family resync delay (" + std::to_wstring(prevRate) +
                L" -> " + std::to_wstring(format_.sampleRate) + L"): " +
                std::to_wstring(effectiveDelay) + L"ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(effectiveDelay));
    }

    if (!SendSetup(error)) {
        if (reuseControl) {
            LogRaat(L"setup on reused control failed (" + error +
                    L"); falling back to fresh RAAT session");
            control_->Close();
            return EnsureSession(false);
        }
        LogRaat(L"setup failed: " + error);
        return false;
    }
    localAddress_ = control_->LocalAddress();

    clockLocked_.store(false, std::memory_order_release);
    deviceClockOffsetNs_.store(0, std::memory_order_release);
    deviceOutputDelayNs_.store(0, std::memory_order_release);
    deviceClockRttUs_.store(0, std::memory_order_release);
    (void)StartClockThread();
    LogRaat(L"clock udp/" + std::to_wstring(clockPort_) +
            L" device_udp/" + std::to_wstring(deviceClockPort_));

    if (!SendStream(error)) {
        LogRaat(L"stream failed: " + error);
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }
    if (!OpenAudio(error)) {
        LogRaat(L"audio connect failed: " + error);
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }
    StartAudioSender();
    if (!Prefill()) {
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }
    (void)WaitForReady(kRaatReadyTimeoutMs);

    int sendSyncSetting = 0;
    ReadSettingInt(sendSyncSetting, L"sync", 0);
    if (sendSyncSetting != 0 && clockPort_ != 0 && !localAddress_.empty()) {
        if (!SendSync()) LogRaat(L"sync did not complete (non-fatal)");
    }

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
        if (port == 0 || !control_) return false;
        const auto started = std::chrono::steady_clock::now();
        if (!control_->Connect(target_.address, port, kRaatConnectTimeoutMs,
                               failure, bindInterface_)) {
            LogRaat(L"control connect " + target_.address + L":" +
                    std::to_wstring(port) + L" failed: " + failure);
            return false;
        }
        target_.controlPort = port;
        LogRaat(L"control connected on :" + std::to_wstring(port) +
                L" in " + std::to_wstring(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count()) + L"ms");
        return true;
    };

    for (std::uint16_t port : ports) {
        std::wstring failure;
        if (tryConnect(port, failure)) return true;
        error = failure;
    }

    std::uint16_t wakePort = 0;
    std::wstring wakeError;
    if (!target_.address.empty() &&
        WakeDevice(target_.address, target_.deviceId, wakePort, wakeError, bindInterface_)) {
        LogRaat(L"wake reported fresh tcp_port " + std::to_wstring(wakePort));
        std::wstring failure;
        if (tryConnect(wakePort, failure)) return true;
        error = failure;
    } else if (!wakeError.empty()) {
        LogRaat(L"wake failed: " + wakeError);
    }

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
    const bool isVegaOrAuralic =
        ContainsWideCaseInsensitive(target_.friendlyName, L"VEGA") ||
        ContainsWideCaseInsensitive(target_.friendlyName, L"AURALIC") ||
        ContainsWideCaseInsensitive(target_.deviceId, L"VEGA") ||
        ContainsWideCaseInsensitive(target_.deviceId, L"AURALIC");
    const bool forbidCleanroom = newGen || isVegaOrAuralic;

    if (forbidCleanroom) {
        LogRaat(L"signed-only endpoint: injecting official raat.dat corpus");
    } else {
        LogRaat(L"legacy endpoint: official corpus first, clean-room fallback allowed");
    }

    const auto runCorpus = [&](const ScriptRecord* records, std::size_t count,
                               bool official) -> bool {
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
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            std::string response;
            bool final = false;
            std::wstring canaryError;
            bool ready = false;
            {
                std::lock_guard lock(controlMutex_);
                if (control_ &&
                    control_->Request("{\"request\":\"echo\"}", 1000,
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

    {
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"RAAT_CORPUS", env, MAX_PATH) > 0 && env[0]) {
            std::ifstream input(std::filesystem::path(env), std::ios::binary);
            if (input) {
                int injected = 0;
                std::string line;
                bool ok = true;
                while (std::getline(input, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty() || line.find("\"request\"") == std::string::npos) continue;
                    std::string response;
                    bool final = false;
                    std::wstring requestError;
                    std::lock_guard lock(controlMutex_);
                    if (!control_ ||
                        !control_->Request(line, 8000, response, final, requestError) ||
                        (!JsonStatus(response).empty() && JsonStatus(response) != "Success")) {
                        ok = false;
                        break;
                    }
                    ++injected;
                }
                if (ok && injected > 0) {
                    std::string response;
                    bool final = false;
                    std::wstring canaryError;
                    std::lock_guard lock(controlMutex_);
                    if (control_ &&
                        control_->Request("{\"request\":\"echo\"}", 1000,
                                          response, final, canaryError) &&
                        JsonStatus(response) == "Success") {
                        LogRaat(L"dispatcher ready after RAAT_CORPUS replay");
                        return true;
                    }
                }
            }
        }
    }

    std::size_t count = 0;
    const ScriptRecord* records = OfficialScripts(count);
    if (runCorpus(records, count, true)) return true;

    if (forbidCleanroom) {
        error = L"official signed corpus failed on signed-only endpoint";
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
    if (!module.empty()) {
        json += ",\"module\":\"" + JsonEscape(module) + "\"";
    }
    if (!keypair.empty() && variant == 0) {
        json += ",\"keypair\":\"" + JsonEscape(keypair) + "\"";
    }
    json += ",\"script\":\"" + JsonEscape(script) + "\"";
    if (variant == 0) {
        if (!signature.empty()) {
            json += ",\"signature\":\"" + signature + "\"";
        } else {
            json += ",\"signature\":\"0\"";
        }
    } else if (variant == 1) {
        json += ",\"signature\":\"0\"";
    }
    json += "}";

    std::lock_guard lock(controlMutex_);
    std::string response;
    bool final = false;
    if (!control_ || !control_->Request(json, kRaatRequestTimeoutMs, response, final, error)) {
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
        if (!control_ || !control_->Request("{\"request\":\"info\"}", 1500,
                                            response, final, error)) {
            LogRaat(L"info not answered (continuing)");
            error.clear();
            return true;
        }
    }
    const std::string status = JsonStatus(response);
    if (status != "Success") {
        LogRaat(L"info status=" + ToWide(status) + L" (continuing)");
        error.clear();
        return true;
    }
    deviceVendor_ = JsonGetString(response, "vendor");
    hasWriteChmap_ = (response.find("\"has_write_chmap\":true") != std::string::npos ||
                      response.find("\"has_write_chmap\": true") != std::string::npos);

    const std::string supported = JsonGetString(response, "supported_formats");
    if (!CheckSupportedFormat(supported)) {
        error = L"device does not advertise " + std::to_wstring(format_.sampleRate) +
                L"/" + std::to_wstring(wireBits_) + L"/" +
                std::to_wstring(format_.channels);
        return false;
    }
    LogRaat(L"info vendor=\"" + ToWide(deviceVendor_) + L"\" has_write_chmap=" +
            (hasWriteChmap_ ? L"1" : L"0"));
    return true;
}

void RaatSink::SendClientType() noexcept {
    std::lock_guard lock(controlMutex_);
    if (!control_) return;
    std::string response;
    bool final = false;
    std::wstring error;
    (void)control_->Request("{\"request\":\"set_client_type\",\"client_type\":\"Roon\"}",
                            1000, response, final, error);
}

std::string RaatSink::GetPersistentControllerId() const noexcept {
    static std::string s_cachedId;
    static std::mutex s_cidMutex;
    std::lock_guard lock(s_cidMutex);
    if (!s_cachedId.empty()) return s_cachedId;

    std::wstring cidW;
    if (ReadSettingString(cidW, L"controller_id") && !cidW.empty()) {
        s_cachedId = ToUtf8(cidW);
        return s_cachedId;
    }

    HKEY hKey = nullptr;
    wchar_t guidBuf[128]{};
    DWORD guidSize = sizeof(guidBuf);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"MachineGuid", nullptr, nullptr, reinterpret_cast<LPBYTE>(guidBuf), &guidSize) == ERROR_SUCCESS && guidBuf[0]) {
            cidW = guidBuf;
        }
        RegCloseKey(hKey);
    }

    if (cidW.empty()) {
        cidW = NewUuidString();
    }

    const auto path = IniPath();
    if (!path.empty()) {
        (void)WritePrivateProfileStringW(L"raat", L"controller_id", cidW.c_str(), path.c_str());
    }

    s_cachedId = ToUtf8(cidW);
    LogRaat(L"persistent controller_id: " + cidW);
    return s_cachedId;
}

void RaatSink::SendSubscriptions() noexcept {
    std::lock_guard lock(controlMutex_);
    if (!control_) return;
    std::uint32_t tRid = 0;
    if (control_->SendAsync("{\"request\":\"subscribe_transport\"}", 1000, tRid)) {
        transportSubRid_.store(tRid, std::memory_order_release);
    }
    std::uint32_t cRid = 0;
    const std::string ctrlJson =
        "{\"request\":\"subscribe_controls\",\"controller_id\":\"" +
        JsonEscape(GetPersistentControllerId()) + "\"}";
    if (control_->SendAsync(ctrlJson, 1000, cRid)) {
        controlsSubRid_.store(cRid, std::memory_order_release);
    }
}

void RaatSink::SendRequestSource() noexcept {
    std::lock_guard lock(controlMutex_);
    if (!control_) return;
    std::string response;
    bool final = false;
    std::wstring error;
    if (control_->Request("{\"request\":\"request_source\"}",
                          1500, response, final, error)) {
        const std::string status = JsonStatus(response);
        if (!status.empty() && status != "NotSupported" && status != "NotImplemented") {
            LogRaat(L"request_source status=" + ToWide(status));
        }
    }
}

bool RaatSink::CheckSupportedFormat(const std::string& supported) const noexcept {
    if (supported.empty()) return true;
    if (supported.find('/') == std::string::npos) return true;
    std::stringstream entries(supported);
    std::string entry;
    bool sawAnyValidEntry = false;
    while (std::getline(entries, entry, ',')) {
        std::stringstream fields(entry);
        std::string rateText;
        std::string bitsText;
        std::string channelsText;
        if (!std::getline(fields, rateText, '/') ||
            !std::getline(fields, bitsText, '/')) {
            continue;
        }
        sawAnyValidEntry = true;
        std::getline(fields, channelsText);
        const int rate = std::atoi(rateText.c_str());
        const int bits = std::atoi(bitsText.c_str());
        if (rate != static_cast<int>(format_.sampleRate) ||
            bits != static_cast<int>(wireBits_)) {
            continue;
        }
        std::stringstream channelList(channelsText);
        std::string channel;
        while (std::getline(channelList, channel, '|')) {
            if (std::atoi(channel.c_str()) == static_cast<int>(format_.channels)) {
                return true;
            }
        }
    }
    return !sawAnyValidEntry;
}

bool RaatSink::SendSetup(std::wstring& error) noexcept {
    teardownObserved_.store(false, std::memory_order_release);
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
    if (sourceBits == 16) {
        add(16);
        add(24);
        add(32);
    } else {
        add(24);
        add(32);
        add(16);
    }

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
        std::uint32_t rid = 0;
        {
            std::lock_guard lock(controlMutex_);
            if (!control_ || !control_->Request(json.str(), kRaatSetupTimeoutMs,
                                                response, final, requestError)) {
                error = requestError;
                continue;
            }
            rid = control_->LastRequestRid();
            setupRid_.store(rid, std::memory_order_release);
            if (JsonStatus(response) == "OutputMessage") {
                LogRaat(L"setup output message received; waiting for port frame");
                const auto waitDeadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(kRaatSetupTimeoutMs);
                for (int attempt = 0; attempt < 8; ++attempt) {
                    if (std::chrono::steady_clock::now() >= waitDeadline) break;
                    std::string follow;
                    bool followFinal = false;
                    std::wstring followError;
                    if (!control_->ReadFrame(rid, 3000, follow, followFinal, followError)) {
                        if (followError.find(L"timed out") != std::wstring::npos &&
                            std::chrono::steady_clock::now() < waitDeadline) {
                            continue;
                        }
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
            deviceClockPort_ = static_cast<std::uint16_t>(clockPort);
            audioPort_ = static_cast<std::uint16_t>(audioPort);
            wireBits_ = bits;
            format_.validBits = bits;
            format_.containerBits = bits;
            const std::uint32_t bytesPerFrame =
                std::max<std::uint32_t>(1u, format_.channels * (wireBits_ / 8u));
            if (testMode_) {
                blockFrames_ = std::max<std::uint32_t>(16u, blockSizeBytes_ / bytesPerFrame);
            } else {
                const std::uint32_t sliceFrames = (format_.sampleRate > 0)
                    ? static_cast<std::uint32_t>((static_cast<std::uint64_t>(format_.sampleRate) * sliceMs_) / 1000u)
                    : 2048u;
                const std::uint32_t maxFramesByBytes = blockSizeBytes_ / bytesPerFrame;
                blockFrames_ = std::clamp<std::uint32_t>(sliceFrames, 256u, maxFramesByBytes);
            }
            block_.assign(static_cast<std::size_t>(blockFrames_) * format_.channels, 0);
            packedActive_.reserve(block_.size());
            const std::uint32_t actualSliceMs = (format_.sampleRate > 0)
                ? (blockFrames_ * 1000u) / format_.sampleRate : 0u;
            LogRaat(L"setup OK: device_clock udp/" + std::to_wstring(deviceClockPort_) +
                    L", audio tcp/" + std::to_wstring(audioPort_) +
                    L", wire=" + std::to_wstring(bits) + L"bit, packetFrames=" +
                    std::to_wstring(blockFrames_) + L" (~" + std::to_wstring(actualSliceMs) + L"ms, " +
                    std::to_wstring(blockFrames_ * bytesPerFrame) + L" bytes, testMode=" +
                    std::to_wstring(testMode_ ? 1 : 0) + L")");
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
    if (audioPort_ == 0 || !audio_) {
        error = L"audio port not announced";
        return false;
    }
    if (!audio_->Connect(target_.address, audioPort_, kRaatConnectTimeoutMs,
                         error, bindInterface_)) {
        return false;
    }
    lastSentChmap_ = 0;
    expectedStreamSample_ = sampleIndex_;
    streamSampleInitialized_ = false;
    return SendAudioCommands(sampleIndex_);
}

bool RaatSink::SendAudioCommands(std::uint64_t startSample) noexcept {
    std::vector<std::uint8_t> commands;
    commands.push_back(2); // COMMAND_STREAMID
    AppendBe32(commands, streamId_);
    commands.push_back(3); // COMMAND_STREAMSAMPLE
    AppendBe64(commands, startSample);
    const std::uint32_t fullMask = ChannelMapFor(format_.channels);
    commands.push_back(1); // COMMAND_CHMAP
    AppendBe32(commands, fullMask);
    std::lock_guard lock(audioMutex_);
    if (!audio_ || !audio_->SendAll(commands.data(), commands.size(), 5000)) {
        lastAudioWsaError_.store(audio_ ? audio_->LastWsaError() : WSAENOTCONN,
                                 std::memory_order_release);
        return false;
    }
    lastSentChmap_ = fullMask;
    expectedStreamSample_ = startSample;
    streamSampleInitialized_ = true;
    lastSendTick_ = GetTickCount64();
    return true;
}

bool RaatSink::SendStream(std::wstring& error) noexcept {
    const double bufferSec = RequestedBufferSeconds();
    // Official Roon parity: ready_threshold=0.45 for 5s buffer (strictly below
    // Prefill()'s 0.50*bufferSec target so endpoints fire Ready immediately).
    const double readyThreshold =
        bufferSec >= 4.0 ? 0.45 : std::min(1.50 / bufferSec, 0.70);
    streamReady_.store(false, std::memory_order_release);

    std::ostringstream json;
    json << std::fixed << std::setprecision(2)
         << "{\"request\":\"stream\",\"stream_id\":" << streamId_
         << ",\"first_seq\":-1,\"nak_port\":-1,\"buffer_size\":" << bufferSec
         << ",\"ready_threshold\":" << readyThreshold << "}";
    std::string response;
    bool final = false;
    {
        std::lock_guard lock(controlMutex_);
        if (!control_ || !control_->Request(json.str(), kRaatRequestTimeoutMs,
                                            response, final, error)) {
            return false;
        }
        streamRid_.store(control_->LastRequestRid(), std::memory_order_release);
    }
    const std::string status = JsonStatus(response);
    if (status == "Ready" || status == "Playing") {
        streamReady_.store(true, std::memory_order_release);
    }
    if (status == "Success" || status == "Buffering" || status == "Ready") {
        LogRaat(L"stream id=" + std::to_wstring(streamId_) + L" status=" +
                ToWide(status) + L" buf=" +
                std::to_wstring(static_cast<int>(bufferSec * 1000)) + L"ms");
        return true;
    }
    error = L"stream status " + ToWide(status);
    return false;
}

bool RaatSink::WaitForReady(std::uint32_t timeoutMs) noexcept {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (!streamReady_.load(std::memory_order_acquire) &&
           !stop_.load(std::memory_order_acquire) &&
           control_ && control_->IsOpen() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (streamReady_.load(std::memory_order_acquire)) {
        LogRaat(L"endpoint reported Ready");
        return true;
    }
    LogRaat(L"WaitForReady timed out (continuing like official Roon)");
    return false;
}

void RaatSink::StartAudioSender() noexcept {
    StopAudioSender(true);
    audioSendStop_.store(false, std::memory_order_release);
    audioSendFault_.store(false, std::memory_order_release);
    lastAudioWsaError_.store(0, std::memory_order_release);
    try {
        audioSendThread_ = std::thread(&RaatSink::AudioSendMain, this);
    } catch (...) {
        audioSendFault_.store(true, std::memory_order_release);
    }
}

void RaatSink::StopAudioSender(bool flushQueue) noexcept {
    audioSendStop_.store(true, std::memory_order_release);
    sendQueueCv_.notify_all();
    if (audioSendThread_.joinable()) audioSendThread_.join();
    if (flushQueue) {
        std::lock_guard lock(sendQueueMutex_);
        sendQueue_.clear();
        queuedFrames_.store(0, std::memory_order_release);
    }
}

std::vector<std::uint8_t> RaatSink::AcquireBuffer(std::size_t minCapacity) noexcept {
    std::vector<std::uint8_t> buf;
    {
        std::lock_guard lock(bufferPoolMutex_);
        if (!freeBuffers_.empty()) {
            buf = std::move(freeBuffers_.back());
            freeBuffers_.pop_back();
        }
    }
    buf.clear();
    if (buf.capacity() < minCapacity) {
        buf.reserve(std::max<std::size_t>(minCapacity, 65536u));
    }
    return buf;
}

void RaatSink::ReleaseBuffer(std::vector<std::uint8_t>&& buf) noexcept {
    if (buf.capacity() > 0) {
        buf.clear();
        std::lock_guard lock(bufferPoolMutex_);
        if (freeBuffers_.size() < 64) {
            freeBuffers_.push_back(std::move(buf));
        }
    }
}

void RaatSink::AudioSendMain() noexcept {
    LocalPlaybackTuningRuntime::Instance().ApplyToCurrentThread();
    BindThreadToIsolatedCore(1);
    while (!audioSendStop_.load(std::memory_order_acquire)) {
        AudioWriteCmd cmd;
        {
            std::unique_lock lock(sendQueueMutex_);
            sendQueueCv_.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return audioSendStop_.load(std::memory_order_acquire) ||
                       !sendQueue_.empty();
            });
            if (audioSendStop_.load(std::memory_order_acquire)) break;
            if (sendQueue_.empty()) continue;
            cmd = std::move(sendQueue_.front());
            sendQueue_.pop_front();
        }

        bool sent = false;
        {
            std::lock_guard lock(audioMutex_);
            sent = audio_ && audio_->SendAll(cmd.bytes.data(), cmd.bytes.size(), 5000);
            if (!sent) {
                lastAudioWsaError_.store(audio_ ? audio_->LastWsaError() : WSAENOTCONN,
                                         std::memory_order_release);
            }
        }
        ReleaseBuffer(std::move(cmd.bytes));
        const std::uint64_t prev =
            queuedFrames_.fetch_sub(cmd.frames, std::memory_order_acq_rel);
        if (prev < cmd.frames) queuedFrames_.store(0, std::memory_order_release);

        if (!sent) {
            audioSendFault_.store(true, std::memory_order_release);
            break;
        }
        lastSendTick_ = GetTickCount64();
        submittedFrames_.fetch_add(cmd.frames, std::memory_order_relaxed);
    }
}

bool RaatSink::EnqueueAudioBytes(std::vector<std::uint8_t> bytes,
                                 std::uint32_t frames) noexcept {
    if (audioSendFault_.load(std::memory_order_acquire)) {
        ReleaseBuffer(std::move(bytes));
        return false;
    }

    const std::uint64_t maxQueuedFrames =
        static_cast<std::uint64_t>(format_.sampleRate) * 10ull;
    const std::uint64_t currentQueued =
        queuedFrames_.load(std::memory_order_acquire);
    if (currentQueued + frames > maxQueuedFrames) {
        LogRaat(L"serverside buffer overrun: queuedFrames=" +
                std::to_wstring(currentQueued) + L" > " +
                std::to_wstring(maxQueuedFrames));
        audioSendFault_.store(true, std::memory_order_release);
        {
            std::lock_guard lock(audioMutex_);
            if (audio_) audio_->Close();
        }
        ReleaseBuffer(std::move(bytes));
        return false;
    }

    {
        std::lock_guard lock(sendQueueMutex_);
        sendQueue_.push_back(AudioWriteCmd{std::move(bytes), frames});
        queuedFrames_.fetch_add(frames, std::memory_order_acq_rel);
    }
    sendQueueCv_.notify_one();
    return true;
}

bool RaatSink::Prefill() noexcept {
    if (!source_) return false;
    const double bufferSec = RequestedBufferSeconds();
    const double readySec = std::min(std::max(2.40, bufferSec * 0.50), bufferSec * 0.90);
    const std::uint64_t targetFrames = static_cast<std::uint64_t>(
        static_cast<double>(format_.sampleRate) * readySec);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(3000);

    std::uint64_t produced = 0;
    while (produced < targetFrames && !stop_.load(std::memory_order_acquire)) {
        const std::uint32_t wantFrames = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(blockFrames_, targetFrames - produced));
        std::uint32_t filledTotal = 0;
        bool endOfStream = false;

        while (filledTotal < wantFrames && !stop_.load(std::memory_order_acquire)) {
            const std::uint32_t step = std::min<std::uint32_t>(
                kMaxBlockFrames, wantFrames - filledTotal);
            const HRESULT prep = source_->Prepare(step);
            if (prep == kAudioSourceWouldBlock) {
                if (filledTotal > 0) break;
                if (std::chrono::steady_clock::now() < deadline) {
                    if (sourceReadyEvent_) {
                        WaitForSingleObject(sourceReadyEvent_, 10);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    continue;
                }
                if (!SendSilence(step)) return false;
                produced += step;
                break;
            }
            if (prep == S_FALSE) {
                endOfStream = true;
                break;
            }
            if (FAILED(prep)) break;

            std::uint32_t written = 0;
            bool eos = false;
            const HRESULT fill = source_->Fill(
                block_.data() + static_cast<std::size_t>(filledTotal) * format_.channels,
                step, written, eos);
            if (fill == kAudioSourceWouldBlock) {
                if (filledTotal > 0) break;
                continue;
            }
            if (FAILED(fill) || written == 0) {
                if (eos) endOfStream = true;
                break;
            }
            filledTotal += written;
            if (eos) {
                endOfStream = true;
                break;
            }
        }

        if (filledTotal > 0) {
            if (!SendData(block_.data(), filledTotal)) return false;
            produced += filledTotal;
            sampleIndex_ += filledTotal;
        }
        if (endOfStream) {
            endOfStreamSubmitted_.store(true, std::memory_order_release);
            break;
        }
        if (filledTotal == 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }
    prefilledMs_ = format_.sampleRate > 0
        ? static_cast<std::uint32_t>((produced * 1000ull) / format_.sampleRate)
        : 0u;
    // Flush async send queue so prefilled PCM actually reaches the endpoint
    // before WaitForReady() begins waiting for the Ready event.
    const auto flushDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(1000);
    while (!stop_.load(std::memory_order_acquire) &&
           queuedFrames_.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < flushDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LogRaat(L"prefill " + std::to_wstring(produced) + L"/" +
            std::to_wstring(targetFrames) + L" frames (" +
            std::to_wstring(prefilledMs_) + L"ms)");
    return true;
}

bool RaatSink::SendStart(bool resume) noexcept {
    const std::uint64_t delayWithMarginNs =
        deviceOutputDelayNs_.load(std::memory_order_acquire) + 20000000ull;
    const std::uint64_t userLeadNs =
        static_cast<std::uint64_t>(startLeadMs_) * 1000000ull;
    const std::uint64_t minOffsetNs =
        std::max<std::uint64_t>({50000000ull, delayWithMarginNs, userLeadNs});

    const std::uint64_t streamSample = resume ? sampleIndex_ : 0;
    std::ostringstream json;
    json << "{\"request\":\"start\",\"stream_sample\":" << streamSample
         << ",\"min_offset\":" << minOffsetNs << "}";
    std::string response;
    bool final = false;
    std::wstring error;
    {
        std::lock_guard lock(controlMutex_);
        if (!control_ || !control_->Request(json.str(), kRaatRequestTimeoutMs,
                                            response, final, error)) {
            LogRaat(L"start failed: " + error);
            return false;
        }
    }
    const std::string status = JsonStatus(response);
    if (status == "Success") {
        LogRaat(std::wstring(resume ? L"resume start " : L"start ") +
                L"sample=" + std::to_wstring(streamSample) +
                L" min_offset=" + std::to_wstring(minOffsetNs / 1000000ull) +
                L"ms (outputDelay=" +
                std::to_wstring(deviceOutputDelayNs_.load(std::memory_order_acquire) / 1000000ull) +
                L"ms)");
        return true;
    }
    LogRaat(L"start status " + ToWide(status) +
            L" response=" + ToWide(response.substr(0, 400)));
    return false;
}

bool RaatSink::SendSync() noexcept {
    std::ostringstream json;
    json << "{\"request\":\"sync\",\"host\":\"" << JsonEscape(ToUtf8(localAddress_))
         << "\",\"port\":" << clockPort_
         << ",\"quiet\":true,\"new_source\":true}";
    std::string response;
    bool final = false;
    std::wstring error;
    std::lock_guard lock(controlMutex_);
    if (!control_ || !control_->Request(json.str(), kRaatRequestTimeoutMs,
                                        response, final, error)) {
        return false;
    }
    const std::string status = JsonStatus(response);
    return status == "Success" || status == "Ready";
}

bool RaatSink::RunDeviceClockSyncRound() noexcept {
    if (clockSocket_ == INVALID_SOCKET || deviceClockPort_ == 0 || target_.address.empty()) {
        return false;
    }
    const std::string addrUtf8 = ToUtf8(target_.address);
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(deviceClockPort_);
    if (inet_pton(AF_INET, addrUtf8.c_str(), &peer.sin_addr) != 1) return false;

    std::unordered_map<std::uint32_t, std::uint64_t> xmitTimes;
    int gotReplies = 0;
    std::uint64_t bestHalfRttNs = UINT64_MAX;
    std::int64_t bestOffsetNs = 0;
    std::uint64_t bestOutputDelayNs = 0;
    std::uint64_t bestNominalLocalNs = 0;

    for (int attempt = 0;
         attempt < kRaatClockSyncMaxAttempts &&
         gotReplies < kRaatClockSyncIterations &&
         !clockStop_.load(std::memory_order_acquire);
         ++attempt) {
        const std::uint32_t rid = nextClockQueryRid_++;
        std::uint8_t query[12]{};
        std::memcpy(query, "RAATCLKQ", 8);
        query[8] = static_cast<std::uint8_t>(rid >> 24u);
        query[9] = static_cast<std::uint8_t>(rid >> 16u);
        query[10] = static_cast<std::uint8_t>(rid >> 8u);
        query[11] = static_cast<std::uint8_t>(rid);

        const std::uint64_t sendNs = NowNanoseconds();
        xmitTimes[rid] = sendNs;
        (void)sendto(clockSocket_, reinterpret_cast<const char*>(query),
                     sizeof(query), 0,
                     reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));

        const auto waitEnd = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(5);
        while (std::chrono::steady_clock::now() < waitEnd &&
               gotReplies < kRaatClockSyncIterations) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(clockSocket_, &set);
            timeval tv{0, 2000};
            if (select(0, &set, nullptr, nullptr, &tv) <= 0 ||
                !FD_ISSET(clockSocket_, &set)) {
                break;
            }
            std::uint8_t buf[256]{};
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int n = recvfrom(clockSocket_, reinterpret_cast<char*>(buf),
                                   sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            const std::uint64_t recvNs = NowNanoseconds();
            if (n >= 28 && std::memcmp(buf, "RAATCLKR", 8) == 0) {
                const std::uint32_t respRid = ReadBe32(buf + 8);
                const auto it = xmitTimes.find(respRid);
                if (it == xmitTimes.end()) continue;
                const std::uint64_t xmitNs = it->second;
                xmitTimes.erase(it);
                const std::uint64_t remoteNs = ReadBe64(buf + 12);
                const std::uint64_t outputDelayNs = ReadBe64(buf + 20);
                const std::uint64_t rttNs = recvNs > xmitNs ? (recvNs - xmitNs) : 0ull;
                const std::uint64_t halfRttNs = rttNs / 2ull;
                const std::uint64_t nominalLocalNs = xmitNs + halfRttNs;
                const std::int64_t offsetNs =
                    static_cast<std::int64_t>(remoteNs) -
                    static_cast<std::int64_t>(nominalLocalNs);
                ++gotReplies;
                if (halfRttNs < bestHalfRttNs) {
                    bestHalfRttNs = halfRttNs;
                    bestOffsetNs = offsetNs;
                    bestOutputDelayNs = outputDelayNs;
                    bestNominalLocalNs = nominalLocalNs;
                }
            } else if (n >= 12 && std::memcmp(buf, "RAATCLKQ", 8) == 0) {
                std::uint8_t reply[28]{};
                std::memcpy(reply, "RAATCLKR", 8);
                std::memcpy(reply + 8, buf + 8, 4);
                const std::uint64_t nowNs = NowNanoseconds();
                for (int i = 0; i < 8; ++i) {
                    reply[12 + i] = static_cast<std::uint8_t>(nowNs >> (56 - i * 8));
                }
                if (sendto(clockSocket_, reinterpret_cast<const char*>(reply),
                           sizeof(reply), 0,
                           reinterpret_cast<const sockaddr*>(&from), fromLen) > 0) {
                    clockRepliesOut_.fetch_add(1, std::memory_order_relaxed);
                }
                clockProbesIn_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (gotReplies == 0 || bestHalfRttNs == UINT64_MAX) return false;
    const std::uint64_t bestRttNs = bestHalfRttNs * 2ull;
    deviceClockRttUs_.store(bestRttNs / 1000ull, std::memory_order_release);
    if (bestRttNs > kRaatClockMaxRttNs && clockLocked_.load(std::memory_order_acquire)) {
        LogRaat(L"clock sync high RTT=" + std::to_wstring(bestRttNs / 1000ull) +
                L"us (>30ms), keeping previous offset");
        return false;
    }

    std::int64_t finalOffsetNs = bestOffsetNs;
    std::uint64_t finalOutputDelayNs = bestOutputDelayNs;
    const bool wasLocked = clockLocked_.exchange(true, std::memory_order_acq_rel);
    if (!wasLocked || clockFirstLocalNs_ == 0) {
        clockFirstLocalNs_ = bestNominalLocalNs;
        clockFirstOffsetNs_ = bestOffsetNs;
        deviceClockOffsetNs_.store(bestOffsetNs, std::memory_order_release);
        deviceOutputDelayNs_.store(bestOutputDelayNs, std::memory_order_release);
        if (!wasLocked) {
            LogRaat(L"clock locked rtt=" + std::to_wstring(bestRttNs / 1000ull) +
                    L"us outputDelay=" + std::to_wstring(bestOutputDelayNs / 1000000ull) + L"ms");
        }
    } else {
        // Low-pass EMA filter (0.85 old + 0.15 new) to reject Wi-Fi transit jitter
        const std::int64_t prevOffset = deviceClockOffsetNs_.load(std::memory_order_relaxed);
        const std::int64_t offsetDiff = bestOffsetNs - prevOffset;
        if (std::abs(offsetDiff) > 50000000LL) {
            finalOffsetNs = bestOffsetNs;
        } else {
            finalOffsetNs = prevOffset + static_cast<std::int64_t>(offsetDiff * 0.15);
        }
        deviceClockOffsetNs_.store(finalOffsetNs, std::memory_order_release);

        const std::uint64_t prevDelay = deviceOutputDelayNs_.load(std::memory_order_relaxed);
        const std::int64_t delayDiff = static_cast<std::int64_t>(bestOutputDelayNs) - static_cast<std::int64_t>(prevDelay);
        if (std::abs(delayDiff) > 50000000LL) {
            finalOutputDelayNs = bestOutputDelayNs;
        } else {
            finalOutputDelayNs = static_cast<std::uint64_t>(static_cast<std::int64_t>(prevDelay) + static_cast<std::int64_t>(delayDiff * 0.15));
        }
        deviceOutputDelayNs_.store(finalOutputDelayNs, std::memory_order_release);

        const std::uint64_t nowMs = GetTickCount64();
        if (nowMs - clockLastTraceMs_ >= 30000ull &&
            bestNominalLocalNs > clockFirstLocalNs_) {
            clockLastTraceMs_ = nowMs;
            const double elapsedSec =
                static_cast<double>(bestNominalLocalNs - clockFirstLocalNs_) / 1e9;
            const double deltaNs =
                static_cast<double>(finalOffsetNs - clockFirstOffsetNs_);
            const double driftPpm =
                elapsedSec > 1.0 ? (deltaNs / (elapsedSec * 1e3)) : 0.0;
            wchar_t msg[160]{};
            std::swprintf(msg, std::size(msg),
                          L"clock sync rtt=%lluus drift=%.2fppm outputDelay=%llums",
                          static_cast<unsigned long long>(bestRttNs / 1000ull),
                          driftPpm,
                          static_cast<unsigned long long>(finalOutputDelayNs / 1000000ull));
            if (debugPaceLogging_ || std::abs(driftPpm) > 500.0) {
                LogRaat(msg);
            }
        }
    }
    return true;
}

std::uint64_t RaatSink::GetRemoteClockNs() const noexcept {
    const std::uint64_t localNs = NowNanoseconds();
    if (!clockLocked_.load(std::memory_order_acquire)) return localNs;
    const std::int64_t offset = deviceClockOffsetNs_.load(std::memory_order_acquire);
    const std::int64_t mapped = static_cast<std::int64_t>(localNs) + offset;
    return mapped > 0 ? static_cast<std::uint64_t>(mapped) : localNs;
}

void RaatSink::ClockMain() noexcept {
    if (clockSocket_ == INVALID_SOCKET) return;
    LocalPlaybackTuningRuntime::Instance().ApplyToCurrentThread();
    BindThreadToIsolatedCore(2);
    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::uint64_t lastSyncMs = GetTickCount64();
    while (!clockStop_.load(std::memory_order_acquire)) {
        const std::uint64_t nowMs = GetTickCount64();
        if (nowMs - lastSyncMs >= kRaatClockSyncIntervalMs) {
            lastSyncMs = nowMs;
            (void)RunDeviceClockSyncRound();
            continue;
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET(clockSocket_, &set);
        timeval tv{0, 100000};
        if (select(0, &set, nullptr, nullptr, &tv) <= 0 ||
            !FD_ISSET(clockSocket_, &set)) {
            continue;
        }

        std::uint8_t buffer[256]{};
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = recvfrom(clockSocket_, reinterpret_cast<char*>(buffer),
                                      sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received < 12 || std::memcmp(buffer, "RAATCLKQ", 8) != 0) continue;

        std::uint8_t reply[28]{};
        std::memcpy(reply, "RAATCLKR", 8);
        std::memcpy(reply + 8, buffer + 8, 4);
        const std::uint64_t nowNs = NowNanoseconds();
        for (int index = 0; index < 8; ++index) {
            reply[12 + index] = static_cast<std::uint8_t>(nowNs >> (56 - index * 8));
        }
        if (sendto(clockSocket_, reinterpret_cast<const char*>(reply),
                   static_cast<int>(sizeof(reply)), 0,
                   reinterpret_cast<const sockaddr*>(&from), fromLength) > 0) {
            clockRepliesOut_.fetch_add(1, std::memory_order_relaxed);
        }
        clockProbesIn_.fetch_add(1, std::memory_order_relaxed);
    }
}

void RaatSink::PumpMain() noexcept {
    LocalPlaybackTuningRuntime::Instance().ApplyToCurrentThread();
    BindThreadToIsolatedCore(3);

    while (!stop_.load(std::memory_order_acquire)) {
        if (paused_.load(std::memory_order_acquire)) {
            paceStartRemoteNs_ = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (!source_) break;

        // Refresh Now Playing metadata & artwork on track transitions or periodic intervals.
        const std::uint64_t nowMetaMs = GetTickCount64();
        const bool isPure = (displayMode_ == DisplayMode::Pure);
        const bool refreshNeeded = metadataRefreshRequested_.exchange(false, std::memory_order_acq_rel);
        const bool periodicTick = !isPure && (nowMetaMs - lastMetaPollMs_ >= transportIntervalMs_);
        if (refreshNeeded || periodicTick) {
            lastMetaPollMs_ = nowMetaMs;
            PushNowPlayingMetadata(refreshNeeded, true);
        }

        std::uint32_t packetFrames = 0;
        bool endOfStream = false;
        while (packetFrames < blockFrames_ && !stop_.load(std::memory_order_acquire)) {
            const std::uint32_t step = std::min<std::uint32_t>(
                kMaxBlockFrames, blockFrames_ - packetFrames);
            const HRESULT prepared = source_->Prepare(step);
            if (prepared == S_FALSE) {
                endOfStream = true;
                break;
            }
            if (prepared == kAudioSourceWouldBlock) {
                if (packetFrames > 0) break;
                waitingForSource_.store(true, std::memory_order_release);
                if (sourceReadyEvent_) {
                    WaitForSingleObject(sourceReadyEvent_, 20);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                break;
            }
            if (FAILED(prepared)) {
                RecordError(ammod::audio::kRaatStreamFailed);
                return;
            }

            waitingForSource_.store(false, std::memory_order_release);
            std::uint32_t written = 0;
            bool eos = false;
            const HRESULT filled = source_->Fill(
                block_.data() + static_cast<std::size_t>(packetFrames) * format_.channels,
                step, written, eos);
            if (filled == kAudioSourceWouldBlock) {
                if (packetFrames > 0) break;
                waitingForSource_.store(true, std::memory_order_release);
                break;
            }
            if (FAILED(filled) || written == 0) {
                if (eos) endOfStream = true;
                break;
            }
            packetFrames += written;
            if (eos) {
                endOfStream = true;
                break;
            }
        }

        if (packetFrames == 0) {
            if (endOfStream) {
                FinishEndOfStream();
                return;
            }
            continue;
        }

        const bool needRecovery =
            tooManyDropouts_.load(std::memory_order_acquire) ||
            teardownObserved_.load(std::memory_order_acquire) ||
            !control_ || !control_->IsOpen() ||
            !SendData(block_.data(), packetFrames);

        if (needRecovery) {
            LogRaat(L"audio path lost or dropout threshold hit: wsa=" +
                    std::to_wstring(lastAudioWsaError_.load(std::memory_order_acquire)) +
                    L" qdepth=" + std::to_wstring(queuedFrames_.load(std::memory_order_acquire)) +
                    L" rttUs=" + std::to_wstring(deviceClockRttUs_.load(std::memory_order_acquire)) +
                    L"; recovering");

            std::vector<std::int32_t> savedBlock(
                block_.begin(),
                block_.begin() + static_cast<std::size_t>(packetFrames) * format_.channels);

            StopAudioSender(true);
            {
                std::lock_guard lock(audioMutex_);
                if (audio_) audio_->Close();
            }

            bool recovered = false;
            const bool endpointTornDown = teardownObserved_.load(std::memory_order_acquire);
            if (control_ && control_->IsOpen() &&
                !tooManyDropouts_.load(std::memory_order_acquire) &&
                !endpointTornDown) {
                {
                    std::lock_guard lock(controlMutex_);
                    std::string response;
                    bool final = false;
                    std::wstring stopError;
                    (void)control_->Request("{\"request\":\"stop\"}", 1000,
                                            response, final, stopError);
                }
                streamId_ = static_cast<std::uint32_t>((GetTickCount64() & 0x7fffffffu) | 1u);
                std::wstring err;
                if (SendStream(err) && OpenAudio(err)) {
                    StartAudioSender();
                    if (SendData(savedBlock.data(), packetFrames)) {
                        sampleIndex_ += packetFrames;
                        if (Prefill()) {
                            const bool isReady = WaitForReady(kRaatReadyTimeoutMs);
                            if ((isReady || prefilledMs_ > 0) && SendStart(true)) {
                                recovered = true;
                                LogRaat(L"audio stream recovered with new streamId=" +
                                        std::to_wstring(streamId_));
                            }
                        }
                    }
                }
            }

            if (!recovered && endpointTornDown && control_ && control_->IsOpen() &&
                !tooManyDropouts_.load(std::memory_order_acquire)) {
                // Official Roon parity: when endpoint reports output Lost/Teardown on an
                // open control channel, re-run setup+stream on the live control TCP first.
                StopAudioSender(true);
                StopClock();
                sampleIndex_ = 0;
                streamId_ = static_cast<std::uint32_t>((GetTickCount64() & 0x7fffffffu) | 1u);
                if (EnsureSession(true) && SendData(savedBlock.data(), packetFrames)) {
                    sampleIndex_ += packetFrames;
                    PushNowPlayingMetadata(true, true);
                    recovered = true;
                    LogRaat(L"output rebuilt on persistent control channel");
                }
            }

            if (!recovered) {
                StopAudioSender(true);
                StopClock();
                {
                    std::lock_guard lock(controlMutex_);
                    if (control_) control_->Close();
                }
                tooManyDropouts_.store(false, std::memory_order_release);
                teardownObserved_.store(false, std::memory_order_release);
                {
                    std::lock_guard lock(dropoutMutex_);
                    dropoutWindow_.clear();
                }
                for (int attempt = 1;
                     attempt <= 5 && !stop_.load(std::memory_order_acquire);
                     ++attempt) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(attempt * 500));
                    sampleIndex_ = 0;
                    streamId_ = static_cast<std::uint32_t>(
                        (GetTickCount64() & 0x7fffffffu) | 1u);
                    if (EnsureSession() &&
                        SendData(savedBlock.data(), packetFrames)) {
                        sampleIndex_ += packetFrames;
                        PushNowPlayingMetadata(true, true);
                        recovered = true;
                        LogRaat(L"full RAAT session rebuilt on attempt " +
                                std::to_wstring(attempt));
                        break;
                    }
                    StopAudioSender(true);
                    StopClock();
                    {
                        std::lock_guard lock(controlMutex_);
                        if (control_) control_->Close();
                    }
                    {
                        std::lock_guard lock(audioMutex_);
                        if (audio_) audio_->Close();
                    }
                }
            }

            if (!recovered) {
                LogRaat(L"audio reconnect failed after 5 attempts; giving up");
                RecordError(ammod::audio::kRaatStreamFailed);
                return;
            }
            paceStartRemoteNs_ = 0;
            continue;
        }

        sampleIndex_ += packetFrames;

        const std::uint64_t nowRemoteNs = GetRemoteClockNs();
        if (paceStartRemoteNs_ == 0) {
            paceStartRemoteNs_ = nowRemoteNs;
            paceStartSample_ = sampleIndex_;
            paceLastLogTick_ = GetTickCount64();
        }

        const auto allowedFramesAt = [&](std::uint64_t remoteNs) noexcept -> std::uint64_t {
            const std::uint64_t elapsedNs =
                remoteNs > paceStartRemoteNs_ ? (remoteNs - paceStartRemoteNs_) : 0ull;
            const std::uint64_t wholeSec = elapsedNs / 1000000000ull;
            const std::uint64_t remNs = elapsedNs % 1000000000ull;
            return paceStartSample_ +
                   wholeSec * format_.sampleRate +
                   (remNs * format_.sampleRate) / 1000000000ull;
        };

        const double bufferSec = RequestedBufferSeconds();
        const std::uint64_t targetLeadFrames = static_cast<std::uint64_t>(
            format_.sampleRate * std::min(static_cast<double>(lookaheadMs_) * 0.85 / 1000.0, bufferSec * 0.85));
        const std::uint64_t prefillFrames = (static_cast<std::uint64_t>(prefilledMs_) * format_.sampleRate) / 1000ull;
        const std::uint64_t hydrationLeadFrames = (targetLeadFrames > prefillFrames)
            ? (targetLeadFrames - prefillFrames)
            : 0ull;

        // Ensure async TCP send queue doesn't build excessive backlog
        while (!stop_.load(std::memory_order_acquire) &&
               queuedFrames_.load(std::memory_order_acquire) > format_.sampleRate * 2u) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        while (!stop_.load(std::memory_order_acquire) &&
               !paused_.load(std::memory_order_acquire)) {
            const std::uint64_t allowed = allowedFramesAt(GetRemoteClockNs()) + hydrationLeadFrames;
            if (sampleIndex_ <= allowed) break;
            const std::uint64_t excessFrames = sampleIndex_ - allowed;
            const std::uint64_t waitNs =
                (excessFrames * 1000000000ull) / std::max<std::uint32_t>(1u, format_.sampleRate);
            const std::uint64_t waitMs = waitNs / 1000000ull;

            if (testMode_) {
                // High-precision QPC microstep pacing (USS style)
                if (waitMs > 2u) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs - 1u));
                } else {
                    static const std::int64_t s_qpcFreq = []() {
                        LARGE_INTEGER f;
                        QueryPerformanceFrequency(&f);
                        return f.QuadPart;
                    }();
                    LARGE_INTEGER nowLi;
                    QueryPerformanceCounter(&nowLi);
                    const std::int64_t targetQpc = nowLi.QuadPart + static_cast<std::int64_t>((waitNs * s_qpcFreq) / 1000000000ull);
                    while (!stop_.load(std::memory_order_acquire) && !paused_.load(std::memory_order_acquire)) {
                        QueryPerformanceCounter(&nowLi);
                        if (nowLi.QuadPart >= targetQpc) break;
                        const std::int64_t remainingTicks = targetQpc - nowLi.QuadPart;
                        const std::int64_t remainingUs = (remainingTicks * 1000000LL) / s_qpcFreq;
                        if (remainingUs > 500) {
                            std::this_thread::yield();
                        } else {
                            YieldProcessor();
                        }
                    }
                    break;
                }
            } else {
                // Standard coarse pacing
                const std::uint32_t sleepMs = static_cast<std::uint32_t>(
                    std::clamp<std::uint64_t>(waitMs, 5u, 50u));
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            }
        }

        const std::uint64_t nowTickMs = GetTickCount64();
        if (nowTickMs - paceLastLogTick_ >= 5000ull) {
            paceLastLogTick_ = nowTickMs;
            const std::uint64_t nominalAllowed = allowedFramesAt(GetRemoteClockNs());
            const std::int64_t currentLeadFrames = static_cast<std::int64_t>(sampleIndex_) -
                                                   static_cast<std::int64_t>(nominalAllowed);
            const std::uint64_t leadMs = (format_.sampleRate > 0 && currentLeadFrames > 0)
                ? (static_cast<std::uint64_t>(currentLeadFrames) * 1000ull) / format_.sampleRate
                : 0ull;
            if (debugPaceLogging_ || currentLeadFrames < 0) {
                LogRaat(L"pace sent=" + std::to_wstring(sampleIndex_) +
                        L" nominal=" + std::to_wstring(nominalAllowed) +
                        L" lead=" + std::to_wstring(currentLeadFrames) +
                        L" (" + std::to_wstring(leadMs) + L"ms)" +
                        L" qdepth=" + std::to_wstring(queuedFrames_.load(std::memory_order_acquire)));
            }
        }

        if (endOfStream) {
            FinishEndOfStream();
            return;
        }
    }
}

void RaatSink::FinishEndOfStream() noexcept {
    endOfStreamSubmitted_.store(true, std::memory_order_release);
    // Drain local async send queue so every PCM frame reaches the TCP socket.
    const auto queueDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(5000);
    while (!stop_.load(std::memory_order_acquire) &&
           queuedFrames_.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < queueDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // If a successor track is already armed, keep the live RAAT stream open
    // and mark gaplessHandoffReady_ so Close()/Stop() parks the session for
    // zero-gap adoption by the next RaatSink (same format) or clean teardown
    // (different format).
    if (successorPending_.load(std::memory_order_acquire) &&
        control_ && control_->IsOpen() &&
        audio_ && audio_->IsOpen() &&
        !audioSendFault_.load(std::memory_order_acquire)) {
        gaplessHandoffReady_.store(true, std::memory_order_release);
        endOfStreamComplete_.store(true, std::memory_order_release);
        state_.store(AudioSinkState::Open, std::memory_order_release);
        LogRaat(L"end of stream -> gapless handoff armed");
        return;
    }

    const std::uint64_t outputDelayMs =
        deviceOutputDelayNs_.load(std::memory_order_acquire) / 1000000ull;
    const std::uint64_t bufferedMs = prefilledMs_ > 0 ? prefilledMs_ : 1000ull;
    const std::uint32_t drainMs = static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(bufferedMs + startLeadMs_ + outputDelayMs + 250ull,
                                  400u, 6000u));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(drainMs);
    while (!stop_.load(std::memory_order_acquire) &&
           !successorPending_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stop_.load(std::memory_order_acquire)) return;

    if (successorPending_.load(std::memory_order_acquire) &&
        control_ && control_->IsOpen() &&
        audio_ && audio_->IsOpen() &&
        !audioSendFault_.load(std::memory_order_acquire)) {
        gaplessHandoffReady_.store(true, std::memory_order_release);
        endOfStreamComplete_.store(true, std::memory_order_release);
        state_.store(AudioSinkState::Open, std::memory_order_release);
        LogRaat(L"end of stream -> late successor gapless handoff armed");
        return;
    }

    if (control_ && control_->IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_->Request("{\"request\":\"end_stream\"}", 1500,
                                response, final, error);
    }
    endOfStreamComplete_.store(true, std::memory_order_release);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    LogRaat(L"end of stream");
}

bool RaatSink::SendData(const std::int32_t* samples, std::uint32_t frames) noexcept {
    if (!samples || frames == 0) return true;
    const std::uint32_t channels = format_.channels;
    const std::uint32_t bytesPerSample = wireBits_ / 8u;

    std::uint32_t activeMask = ChannelMapFor(channels);
    std::uint32_t activeChannels = channels;
    const std::int32_t* packSource = samples;

    if (hasWriteChmap_ && channels > 1 && channels <= 32) {
        std::uint32_t mask = 0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            for (std::uint32_t f = 0; f < frames; ++f) {
                if (samples[static_cast<std::size_t>(f) * channels + ch] != 0) {
                    mask |= (1u << ch);
                    break;
                }
            }
        }
        if (mask != 0 && mask != activeMask) {
            activeMask = mask;
            activeChannels = 0;
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                if ((activeMask & (1u << ch)) != 0) ++activeChannels;
            }
            packedActive_.resize(static_cast<std::size_t>(frames) * activeChannels);
            std::size_t dst = 0;
            for (std::uint32_t f = 0; f < frames; ++f) {
                const std::size_t base = static_cast<std::size_t>(f) * channels;
                for (std::uint32_t ch = 0; ch < channels; ++ch) {
                    if ((activeMask & (1u << ch)) != 0) {
                        packedActive_[dst++] = samples[base + ch];
                    }
                }
            }
            packSource = packedActive_.data();
        }
    }

    const std::size_t sampleCount = static_cast<std::size_t>(frames) * activeChannels;
    const std::uint32_t bodyBytes =
        static_cast<std::uint32_t>(sampleCount * bytesPerSample);

    std::vector<std::uint8_t> packet = AcquireBuffer(24u + bodyBytes);

    if (!streamSampleInitialized_ || expectedStreamSample_ != sampleIndex_) {
        packet.push_back(3);
        AppendBe64(packet, sampleIndex_);
        expectedStreamSample_ = sampleIndex_;
        streamSampleInitialized_ = true;
    }

    if (activeMask != lastSentChmap_) {
        packet.push_back(1);
        AppendBe32(packet, activeMask);
        lastSentChmap_ = activeMask;
    }

    const std::size_t dataOffset = packet.size();
    packet.resize(dataOffset + 9u + bodyBytes);
    packet[dataOffset + 0] = 0;
    packet[dataOffset + 1] = static_cast<std::uint8_t>(frames >> 24u);
    packet[dataOffset + 2] = static_cast<std::uint8_t>(frames >> 16u);
    packet[dataOffset + 3] = static_cast<std::uint8_t>(frames >> 8u);
    packet[dataOffset + 4] = static_cast<std::uint8_t>(frames);
    packet[dataOffset + 5] = static_cast<std::uint8_t>(bodyBytes >> 24u);
    packet[dataOffset + 6] = static_cast<std::uint8_t>(bodyBytes >> 16u);
    packet[dataOffset + 7] = static_cast<std::uint8_t>(bodyBytes >> 8u);
    packet[dataOffset + 8] = static_cast<std::uint8_t>(bodyBytes);

    if (!PackCanonicalPcm32(packSource, packet.data() + dataOffset + 9u,
                            sampleCount, wireBits_)) {
        ReleaseBuffer(std::move(packet));
        return false;
    }
    expectedStreamSample_ += frames;
    return EnqueueAudioBytes(std::move(packet), frames);
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
    wchar_t probe[32]{};
    if (GetPrivateProfileStringW(L"raat", key, L"", probe,
                                 static_cast<DWORD>(std::size(probe)),
                                 path.c_str()) > 0) {
        value = GetPrivateProfileIntW(L"raat", key, fallback, path.c_str());
        return true;
    }
    if (GetPrivateProfileStringW(L"r_device", key, L"", probe,
                                 static_cast<DWORD>(std::size(probe)),
                                 path.c_str()) > 0) {
        value = GetPrivateProfileIntW(L"r_device", key, fallback, path.c_str());
        return true;
    }
    if (GetPrivateProfileStringW(L"roon", key, L"", probe,
                                 static_cast<DWORD>(std::size(probe)),
                                 path.c_str()) > 0) {
        value = GetPrivateProfileIntW(L"roon", key, fallback, path.c_str());
        return true;
    }
    value = fallback;
    return false;
}

bool RaatSink::ReadSettingString(std::wstring& value, const wchar_t* key) const noexcept {
    value.clear();
    const std::wstring path = IniPath();
    if (path.empty()) return false;
    wchar_t buffer[128]{};
    if (GetPrivateProfileStringW(L"raat", key, L"", buffer,
                                 static_cast<DWORD>(std::size(buffer)),
                                 path.c_str()) > 0) {
        value = buffer;
        return true;
    }
    if (GetPrivateProfileStringW(L"r_device", key, L"", buffer,
                                 static_cast<DWORD>(std::size(buffer)),
                                 path.c_str()) > 0) {
        value = buffer;
        return true;
    }
    if (GetPrivateProfileStringW(L"roon", key, L"", buffer,
                                 static_cast<DWORD>(std::size(buffer)),
                                 path.c_str()) > 0) {
        value = buffer;
        return true;
    }
    return false;
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

    check(ChannelMapFor(0) == 0x00000000u, "0 channels");
    check(ChannelMapFor(1) == 0x00000001u, "mono");
    check(ChannelMapFor(2) == 0x00000003u, "stereo");
    check(ChannelMapFor(6) == 0x0000003Fu, "5.1");
    check(ChannelMapFor(8) == 0x000000FFu, "7.1");

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
