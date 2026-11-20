#include "BoundedLog.h"
#include "TrackMetadata.h"

#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

using namespace winrt;
using namespace Windows::Media::Control;

namespace {

void LogRecovery(const std::string& message) {
    static std::mutex logMutex;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    const auto logPath = std::filesystem::path(path).parent_path() / L"am-exclusive-recovery.log";
    std::wstring line = L"uptimeMs=" + std::to_wstring(GetTickCount64()) +
                        L" pid=" + std::to_wstring(GetCurrentProcessId()) + L" ";
    line.append(message.begin(), message.end());
    ammod::logging::AppendWideLine(logPath, logMutex, line, 1ull * 1024ull * 1024ull, 2);
}

struct MediaIdentity {
    std::wstring title;
    std::wstring artist;
    std::wstring albumTitle;
    std::int32_t trackNumber{};
    std::int64_t timelineEnd100ns{};
};

MediaIdentity ReadMediaIdentity(const GlobalSystemMediaTransportControlsSession& session) {
    const auto media = session.TryGetMediaPropertiesAsync().get();
    const auto timeline = session.GetTimelineProperties();
    return {
        media.Title().c_str(),
        media.Artist().c_str(),
        media.AlbumTitle().c_str(),
        media.TrackNumber(),
        timeline.EndTime().count(),
    };
}

bool HasConfirmedItemChange(const MediaIdentity& before, const MediaIdentity& current) {
    if (!before.title.empty() && current.title.empty()) return false;
    if (!before.title.empty() && current.title != before.title) return true;
    if (!before.artist.empty() && !current.artist.empty() && current.artist != before.artist) return true;
    if (!before.albumTitle.empty() && !current.albumTitle.empty() && current.albumTitle != before.albumTitle) return true;
    if (current.trackNumber != 0 && before.trackNumber != 0 && current.trackNumber != before.trackNumber) return true;
    return current.timelineEnd100ns > 0 && before.timelineEnd100ns > 0 &&
           current.timelineEnd100ns != before.timelineEnd100ns;
}

bool HasReturnedToItem(const MediaIdentity& expected, const MediaIdentity& current) {
    if (!expected.title.empty()) return current.title == expected.title;
    if (!expected.artist.empty() && current.artist != expected.artist) return false;
    if (!expected.albumTitle.empty() && current.albumTitle != expected.albumTitle) return false;
    if (expected.trackNumber != 0 && current.trackNumber != expected.trackNumber) return false;
    if (expected.timelineEnd100ns > 0 && current.timelineEnd100ns != expected.timelineEnd100ns) return false;
    return !expected.artist.empty() || !expected.albumTitle.empty() ||
           expected.trackNumber != 0 || expected.timelineEnd100ns > 0;
}

struct RoundTripResult {
    bool primaryAccepted{};
    bool itemChangeConfirmed{};
    std::uint32_t itemChangeWaitMs{};
    MediaIdentity visited{};
    bool compensatingAccepted{};
    bool returned{};
    std::uint32_t returnWaitMs{};
};

bool RestartCurrentItem(const GlobalSystemMediaTransportControlsSession& session,
                        const MediaIdentity& original,
                        RoundTripResult& result) {
    result = {};
    result.primaryAccepted = session.TrySkipPreviousAsync().get();
    if (!result.primaryAccepted) return false;

    constexpr std::uint32_t pollMs = 50;
    constexpr std::uint32_t timeoutMs = 6000;
    while (result.itemChangeWaitMs < timeoutMs) {
        Sleep(pollMs);
        result.itemChangeWaitMs += pollMs;
        result.visited = ReadMediaIdentity(session);
        if (HasConfirmedItemChange(original, result.visited)) {
            result.itemChangeConfirmed = true;
            break;
        }
    }
    if (!result.itemChangeConfirmed) return false;

    result.compensatingAccepted = session.TrySkipNextAsync().get();
    if (!result.compensatingAccepted) return false;

    while (result.returnWaitMs < timeoutMs) {
        Sleep(pollMs);
        result.returnWaitMs += pollMs;
        if (HasReturnedToItem(original, ReadMediaIdentity(session))) {
            result.returned = true;
            break;
        }
    }
    return result.returned;
}

} // namespace

namespace {

// TRACK2 text is polled every 200ms, including equal-duration identity changes.
// Thumbnail I/O runs on one bounded worker, never on the audio process/thread.
constexpr std::uint32_t kPollMs = 200;
constexpr std::uint64_t kRefreshMs = 3000;
constexpr std::uint64_t kArtworkRefreshMs = 600;
using ammod::metadata::TrackSnapshot;
using Windows::Storage::Streams::IRandomAccessStreamReference;

template<typename Operation>
auto AwaitBounded(const Operation& operation, std::chrono::milliseconds timeout) {
    if (operation.wait_for(timeout) != Windows::Foundation::AsyncStatus::Completed) {
        operation.Cancel();
        throw hresult_error(E_ABORT);
    }
    return operation.get();
}

std::wstring BoundedText(const hstring& text) {
    if (text.size() > ammod::metadata::kMaxTrackTextBytes) throw std::runtime_error("oversize metadata");
    return {text.data(), text.size()};
}

struct MediaRead {
    TrackSnapshot track;
    IRandomAccessStreamReference thumbnail{nullptr};
};

MediaRead ReadTrack(const GlobalSystemMediaTransportControlsSession& session) {
    const auto media = AwaitBounded(session.TryGetMediaPropertiesAsync(), std::chrono::milliseconds(200));
    const auto timeline = session.GetTimelineProperties();
    const auto start = timeline.StartTime().count(), end = timeline.EndTime().count();
    if (start < 0 || end < start) throw std::runtime_error("invalid media timeline");
    const auto duration = static_cast<std::uint64_t>(end - start) / 10000;
    if (duration > (std::numeric_limits<std::uint32_t>::max)()) throw std::runtime_error("oversize media timeline");
    MediaRead result;
    result.track.durationMs = static_cast<std::uint32_t>(duration);
    result.track.title = BoundedText(media.Title());
    result.track.artist = BoundedText(media.Artist());
    result.track.album = BoundedText(media.AlbumTitle());
    result.track.trackNumber = media.TrackNumber() > 0 ? static_cast<std::uint32_t>(media.TrackNumber()) : 0;
    result.thumbnail = media.Thumbnail();
    // ReadTrack is used only for FindAppleSession results, including worker verification.
    ammod::metadata::NormalizeAppleMusicMetadata(result.track);
    if (ammod::metadata::SerializeTrackLine(result.track).empty()) throw std::runtime_error("invalid media text");
    return result;
}

GlobalSystemMediaTransportControlsSession FindAppleSession(
    const GlobalSystemMediaTransportControlsSessionManager& manager) {
    for (const auto& candidate : manager.GetSessions()) {
        const std::wstring source = candidate.SourceAppUserModelId().c_str();
        if (source.find(L"AppleMusic") != std::wstring::npos) return candidate;
    }
    return nullptr;
}

// Diagnostic values are raw provider fields, not NormalizeAppleMusicMetadata output.
// Encode bytes as hex so embedded whitespace/control characters cannot frame new lines.
std::string RawFieldHex(const hstring& value) {
    if (value.size() > ammod::metadata::kMaxTrackTextBytes) throw std::runtime_error("oversize diagnostic field");
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0 || static_cast<std::size_t>(length) > ammod::metadata::kMaxTrackTextBytes)
        throw std::runtime_error("invalid diagnostic field");
    std::string utf8(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                           utf8.data(), length, nullptr, nullptr) != length)
        throw std::runtime_error("invalid diagnostic field");
    std::string hex;
    ammod::metadata::detail::AppendHex(hex, reinterpret_cast<const std::uint8_t*>(utf8.data()), utf8.size());
    return hex;
}

int InspectMedia() {
    try {
        init_apartment(apartment_type::multi_threaded);
        const auto manager = AwaitBounded(GlobalSystemMediaTransportControlsSessionManager::RequestAsync(),
                                         std::chrono::milliseconds(300));
        const auto session = FindAppleSession(manager);
        if (!session) { std::puts("INSPECT2\tNO_SESSION"); return 3; }
        const auto media = AwaitBounded(session.TryGetMediaPropertiesAsync(), std::chrono::milliseconds(200));
        const char* labels[] = {"Title", "Artist", "AlbumTitle", "AlbumArtist", "Subtitle"};
        const hstring values[] = {media.Title(), media.Artist(), media.AlbumTitle(), media.AlbumArtist(), media.Subtitle()};
        std::string output = "INSPECT2\n";
        for (std::size_t i = 0; i < 5; ++i)
            output += std::string(labels[i]) + "_UTF8_HEX=" + RawFieldHex(values[i]) + "\n";
        output += "TrackNumber=" + std::to_string(media.TrackNumber()) + "\n";
        std::fwrite(output.data(), 1, output.size(), stdout);
        std::fflush(stdout);
        return 0;
    } catch (...) {
        std::puts("INSPECT2\tREAD_FAILED");
        return 4;
    }
}

class ThumbnailWorker {
public:
    struct Request {
        std::uint64_t generation{};
        GlobalSystemMediaTransportControlsSession session{nullptr};
        TrackSnapshot identity;
        IRandomAccessStreamReference thumbnail{nullptr};
    };
    struct Result {
        std::uint64_t generation{};
        TrackSnapshot track;
    };

    ThumbnailWorker() : thread_([this] { Run(); }) {}
    ~ThumbnailWorker() {
        { std::lock_guard lock(mutex_); stop_ = true; }
        ready_.notify_one();
        thread_.join();
    }
    void Submit(Request request) {
        { std::lock_guard lock(mutex_); pending_ = std::move(request); }
        ready_.notify_one();
    }
    std::optional<Result> Take() {
        std::lock_guard lock(mutex_);
        auto result = std::move(result_);
        result_.reset();
        return result;
    }
private:
    void Run() {
        try {
            init_apartment(apartment_type::multi_threaded);
            for (;;) {
                Request request;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock, [this] { return stop_ || pending_.has_value(); });
                    if (stop_) break;
                    request = std::move(*pending_);
                    pending_.reset();
                }
                TrackSnapshot track = request.identity; // Never seed from the previous item's art.
                try {
                    if (request.thumbnail) {
                        auto stream = AwaitBounded(request.thumbnail.OpenReadAsync(), std::chrono::milliseconds(300));
                        const auto size = stream.Size();
                        if (!size || size > ammod::metadata::kMaxArtworkBytes) throw std::runtime_error("oversize thumbnail");
                        Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
                        const auto loaded = AwaitBounded(reader.LoadAsync(static_cast<std::uint32_t>(size)),
                                                         std::chrono::milliseconds(300));
                        if (loaded != size) throw std::runtime_error("truncated thumbnail");
                        track.artwork.resize(static_cast<std::size_t>(size));
                        reader.ReadBytes(winrt::array_view<std::uint8_t>(track.artwork));
                        track.artworkMime = ammod::metadata::ArtworkMime(track.artwork);
                        if (track.artworkMime.empty()) track.artwork.clear();
                        // Discard a read that straddled a media-property transition.
                        if (!ammod::metadata::SameTrack(track, ReadTrack(request.session).track)) continue;
                    }
                } catch (...) {
                    track.artwork.clear();
                    track.artworkMime.clear();
                }
                {
                    std::lock_guard lock(mutex_);
                    result_ = Result{request.generation, std::move(track)};
                }
            }
            uninit_apartment();
        } catch (...) {
            // Main polling still emits fresh text/empty art if worker initialization fails.
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Request> pending_; // One running, one replaceable pending request, one result.
    std::optional<Result> result_;
    bool stop_{};
    std::thread thread_;
};

void WriteTrackLine(const TrackSnapshot& track) {
    auto line = ammod::metadata::SerializeTrackLine(track);
    if (line.empty()) line = ammod::metadata::SerializeTrackLine({});
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int WatchTracks(bool once = false) {
    init_apartment(apartment_type::multi_threaded);
    ThumbnailWorker thumbnails;
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    GlobalSystemMediaTransportControlsSession previousSession{nullptr};
    TrackSnapshot current, printed;
    std::uint64_t generation = 0, lastPrint = 0, lastArtworkRequest = 0;
    bool havePrinted = false;
    const auto started = GetTickCount64();
    for (;;) {
        try {
            if (!manager) manager = AwaitBounded(GlobalSystemMediaTransportControlsSessionManager::RequestAsync(),
                                                std::chrono::milliseconds(300));
            const auto session = FindAppleSession(manager); // Re-enumerate: removed sessions must clear immediately.
            if (!session) {
                if (previousSession) ++generation;
                previousSession = nullptr;
                current = {};
            } else {
                const auto media = ReadTrack(session);
                const bool changed = get_abi(previousSession) != get_abi(session) ||
                    !ammod::metadata::SameTrack(current, media.track);
                if (changed) {
                    ++generation;
                    current = media.track; // Publish text immediately; no old-cover carry-over.
                    lastArtworkRequest = 0;
                }
                previousSession = session;
                if (!media.thumbnail) {
                    // Invalidate any outstanding result as well as a previously loaded image.
                    ++generation;
                    current.artwork.clear();
                    current.artworkMime.clear();
                }
                if (auto result = thumbnails.Take(); result && result->generation == generation &&
                    ammod::metadata::SameTrack(current, result->track)) {
                    current.artwork = std::move(result->track.artwork);
                    current.artworkMime = std::move(result->track.artworkMime);
                }
                const auto now = GetTickCount64();
                if (!havePrinted || changed) {
                    WriteTrackLine(current);
                    printed = current;
                    havePrinted = true;
                    lastPrint = now;
                }
                if (media.thumbnail && (lastArtworkRequest == 0 || now - lastArtworkRequest >= kArtworkRefreshMs)) {
                    thumbnails.Submit({generation, session, media.track, media.thumbnail});
                    lastArtworkRequest = now;
                }
            }
        } catch (...) {
            ++generation;
            previousSession = nullptr;
            manager = nullptr;
            current = {}; // Failed/disappeared sessions must not leave stale text or cover.
        }
        const auto now = GetTickCount64();
        if (!havePrinted || !ammod::metadata::SameTrack(current, printed) ||
            current.artwork != printed.artwork || current.artworkMime != printed.artworkMime ||
            now - lastPrint >= kRefreshMs) {
            WriteTrackLine(current);
            printed = current;
            havePrinted = true;
            lastPrint = now;
        }
        // Ordered lightweight freshness proof without retransmitting large images.
        std::fputs("PULSE2\n", stdout);
        std::fflush(stdout);
        // Read-only finite diagnostic: initial text plus any art resolved within 1.2s.
        if (once && now - started >= 1200) return 0;
        Sleep(kPollMs);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && _wcsicmp(argv[1], L"inspect") == 0) return InspectMedia();
    if (argc == 2 && (_wcsicmp(argv[1], L"watch") == 0 ||
                      _wcsicmp(argv[1], L"watch-once") == 0)) {
        return WatchTracks(_wcsicmp(argv[1], L"watch-once") == 0);
    }
    if (argc != 2 || (_wcsicmp(argv[1], L"restart-current") != 0 &&
                      _wcsicmp(argv[1], L"reject-current") != 0)) return 2;

    init_apartment(apartment_type::multi_threaded);
    const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    GlobalSystemMediaTransportControlsSession appleSession{nullptr};
    for (const auto& session : manager.GetSessions()) {
        const std::wstring source = session.SourceAppUserModelId().c_str();
        if (source.find(L"AppleMusic") != std::wstring::npos) {
            appleSession = session;
            break;
        }
    }
    if (!appleSession) return 3;

    if (_wcsicmp(argv[1], L"reject-current") == 0) {
        const bool stopAccepted = appleSession.TryStopAsync().get();
        bool stopConfirmed = false;
        if (stopAccepted) {
            constexpr std::uint32_t pollMs = 25;
            constexpr std::uint32_t timeoutMs = 750;
            for (std::uint32_t waited = 0; waited <= timeoutMs; waited += pollMs) {
                if (appleSession.GetPlaybackInfo().PlaybackStatus() ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped) {
                    stopConfirmed = true;
                    break;
                }
                Sleep(pollMs);
            }
        }

        // Stop is the strongest public GSMTC teardown for the current media
        // session. If Apple refuses it, fail closed with pause + rewind while
        // AME has already retired its own captured/load state.
        bool paused = stopConfirmed;
        bool reset = stopConfirmed;
        if (!stopConfirmed) {
            paused = appleSession.TryPauseAsync().get();
            const auto timeline = appleSession.GetTimelineProperties();
            reset = appleSession.TryChangePlaybackPositionAsync(
                timeline.StartTime().count()).get();
        }
        LogRecovery("reject-current stopAccepted=" + std::to_string(stopAccepted) +
                    " stopConfirmed=" + std::to_string(stopConfirmed) +
                    " paused=" + std::to_string(paused) +
                    " reset=" + std::to_string(reset));
        return (stopConfirmed || (paused && reset)) ? 0 : 5;
    }

    const MediaIdentity original = ReadMediaIdentity(appleSession);
    const auto controls = appleSession.GetPlaybackInfo().Controls();
    if (!controls.IsPreviousEnabled()) return 4;

    RoundTripResult result{};
    LogRecovery("begin strategy=previous-confirm-next originalEnd100ns=" +
                std::to_string(original.timelineEnd100ns));
    const bool succeeded = RestartCurrentItem(appleSession, original, result);
    LogRecovery("complete primaryAccepted=" + std::to_string(result.primaryAccepted) +
                " changed=" + std::to_string(result.itemChangeConfirmed) +
                " compensatingAccepted=" + std::to_string(result.compensatingAccepted) +
                " returned=" + std::to_string(result.returned));
    return succeeded ? 0 : 5;
}
