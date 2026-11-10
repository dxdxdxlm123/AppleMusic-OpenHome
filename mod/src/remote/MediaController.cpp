#include "MediaController.h"

#include <windows.h>
#include <shellapi.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <iostream>
#include <chrono>

using namespace winrt;
using namespace Windows::Media::Control;

namespace ammod::remote {

namespace {

std::wstring ToWide(const std::string& str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), size);
    return result;
}

std::vector<uint8_t> ReadStreamBytes(const Windows::Storage::Streams::IRandomAccessStreamWithContentType& stream) {
    if (!stream) return {};
    uint64_t size64 = stream.Size();
    if (size64 == 0 || size64 > 10 * 1024 * 1024) return {};
    uint32_t size = static_cast<uint32_t>(size64);

    Windows::Storage::Streams::Buffer buffer(size);
    stream.ReadAsync(buffer, size, Windows::Storage::Streams::InputStreamOptions::None).get();
    auto reader = Windows::Storage::Streams::DataReader::FromBuffer(buffer);
    std::vector<uint8_t> bytes(size);
    reader.ReadBytes(winrt::array_view<uint8_t>(bytes));
    return bytes;
}

GlobalSystemMediaTransportControlsSession GetAppleSession(const GlobalSystemMediaTransportControlsSessionManager& manager) {
    if (!manager) return nullptr;
    for (const auto& s : manager.GetSessions()) {
        const std::wstring src = s.SourceAppUserModelId().c_str();
        if (src.find(L"AppleMusic") != std::wstring::npos) {
            return s;
        }
    }
    return nullptr;
}

} // namespace

MediaController::MediaController() = default;

MediaController::~MediaController() {
    Stop();
}

bool MediaController::Start() {
    if (running_) return true;
    running_ = true;

    workerThread_ = CreateThread(nullptr, 0, [](void* param) -> DWORD {
        auto* self = static_cast<MediaController*>(param);
        self->WorkerLoop();
        return 0;
    }, this, 0, nullptr);

    return workerThread_ != nullptr;
}

void MediaController::Stop() {
    if (!running_) return;
    running_ = false;
    if (workerThread_) {
        WaitForSingleObject(workerThread_, 3000);
        CloseHandle(workerThread_);
        workerThread_ = nullptr;
    }
}

TrackSnapshot MediaController::GetSnapshot() {
    std::lock_guard lock(mutex_);
    return currentSnapshot_;
}

std::vector<uint8_t> MediaController::GetArtwork(std::string& outMime) {
    std::lock_guard lock(mutex_);
    outMime = currentSnapshot_.artworkMime;
    return currentSnapshot_.artworkBytes;
}

void MediaController::WorkerLoop() {
    init_apartment(apartment_type::multi_threaded);

    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    try {
        manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    } catch (...) {}

    GlobalSystemMediaTransportControlsSession session{nullptr};

    while (running_) {
        try {
            if (!manager) {
                manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            }

            if (manager) {
                // Find Apple Music session
                session = nullptr;
                for (const auto& candidate : manager.GetSessions()) {
                    const std::wstring source = candidate.SourceAppUserModelId().c_str();
                    if (source.find(L"AppleMusic") != std::wstring::npos) {
                        session = candidate;
                        break;
                    }
                }
            }

            if (session) {
                auto mediaProps = session.TryGetMediaPropertiesAsync().get();
                auto timeline = session.GetTimelineProperties();
                auto playbackInfo = session.GetPlaybackInfo();

                std::wstring title = mediaProps.Title().c_str();
                std::wstring artist = mediaProps.Artist().c_str();
                std::wstring album = mediaProps.AlbumTitle().c_str();
                int32_t trackNum = mediaProps.TrackNumber();

                int64_t end100ns = timeline.EndTime().count();
                int64_t pos100ns = timeline.Position().count();
                int64_t durationMs = end100ns > 0 ? (end100ns / 10000) : 0;
                int64_t positionMs = pos100ns > 0 ? (pos100ns / 10000) : 0;

                auto status = playbackInfo.PlaybackStatus();
                bool isPlaying = (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                bool isPaused = (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused);

                bool trackChanged = false;
                bool stateChanged = false;
                bool timeChanged = false;

                {
                    std::lock_guard lock(mutex_);
                    if (title != currentSnapshot_.title || artist != currentSnapshot_.artist || album != currentSnapshot_.album) {
                        trackChanged = true;
                        currentSnapshot_.title = title;
                        currentSnapshot_.artist = artist;
                        currentSnapshot_.album = album;
                        currentSnapshot_.trackNumber = trackNum;
                        currentSnapshot_.durationMs = durationMs;

                        // Fetch thumbnail on track change
                        try {
                            auto thumbRef = mediaProps.Thumbnail();
                            if (thumbRef) {
                                auto stream = thumbRef.OpenReadAsync().get();
                                currentSnapshot_.artworkBytes = ReadStreamBytes(stream);
                                currentSnapshot_.artworkMime = "image/jpeg";
                            } else {
                                currentSnapshot_.artworkBytes.clear();
                            }
                        } catch (...) {
                            currentSnapshot_.artworkBytes.clear();
                        }
                    }

                    if (isPlaying != currentSnapshot_.isPlaying || isPaused != currentSnapshot_.isPaused) {
                        stateChanged = true;
                        currentSnapshot_.isPlaying = isPlaying;
                        currentSnapshot_.isPaused = isPaused;
                    }

                    if (std::abs(positionMs - currentSnapshot_.positionMs) > 250 || durationMs != currentSnapshot_.durationMs) {
                        timeChanged = true;
                        currentSnapshot_.positionMs = positionMs;
                        currentSnapshot_.durationMs = durationMs;
                    }
                }

                if (trackChanged && onTrackChanged_) {
                    onTrackChanged_(GetSnapshot());
                }
                if (stateChanged && onStateChanged_) {
                    onStateChanged_(GetSnapshot());
                }
                if (timeChanged && onTimeChanged_) {
                    onTimeChanged_(GetSnapshot());
                }
            } else {
                std::lock_guard lock(mutex_);
                currentSnapshot_.isPlaying = false;
                currentSnapshot_.isPaused = false;
            }
        } catch (...) {
            session = nullptr;
        }

        Sleep(300);
    }
}

bool MediaController::Play() {
    init_apartment(apartment_type::multi_threaded);
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = GetAppleSession(manager);
        if (session) return session.TryPlayAsync().get();
    } catch (...) {}
    // If Apple Music is not running yet, auto-launch it!
    ShellExecuteW(nullptr, L"open", L"music://", nullptr, nullptr, SW_SHOWMINNOACTIVE);
    return true;
}

bool MediaController::Pause() {
    init_apartment(apartment_type::multi_threaded);
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = GetAppleSession(manager);
        if (session) return session.TryPauseAsync().get();
    } catch (...) {}
    return false;
}

bool MediaController::TogglePlayPause() {
    init_apartment(apartment_type::multi_threaded);
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = GetAppleSession(manager);
        if (session) return session.TryTogglePlayPauseAsync().get();
    } catch (...) {}
    // If Apple Music is not running yet, auto-launch it!
    ShellExecuteW(nullptr, L"open", L"music://", nullptr, nullptr, SW_SHOWMINNOACTIVE);
    return true;
}

bool MediaController::SkipNext() {
    init_apartment(apartment_type::multi_threaded);
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = GetAppleSession(manager);
        if (session) return session.TrySkipNextAsync().get();
    } catch (...) {}
    return false;
}

bool MediaController::SkipPrevious() {
    init_apartment(apartment_type::multi_threaded);
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = GetAppleSession(manager);
        if (session) return session.TrySkipPreviousAsync().get();
    } catch (...) {}
    return false;
}

bool MediaController::Seek(double seconds) {
    init_apartment(apartment_type::multi_threaded);
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = GetAppleSession(manager);
        if (session) {
            int64_t pos100ns = static_cast<int64_t>(seconds * 10000000.0);
            return session.TryChangePlaybackPositionAsync(pos100ns).get();
        }
    } catch (...) {}
    return false;
}

bool MediaController::PlayUrl(const std::string& url) {
    if (url.empty()) return false;
    std::string playUri = url;
    // If it's https://music.apple.com/..., convert to music://music.apple.com/...
    if (playUri.rfind("https://", 0) == 0) {
        playUri = "music://" + playUri.substr(8);
    }
    std::wstring wUri = ToWide(playUri);
    HINSTANCE res = ShellExecuteW(nullptr, L"open", wUri.c_str(), nullptr, nullptr, SW_SHOW);
    return reinterpret_cast<intptr_t>(res) > 32;
}

bool MediaController::PlayTrackId(const std::string& trackId) {
    if (trackId.empty()) return false;
    std::string playUri = "music://music.apple.com/song/" + trackId;
    return PlayUrl(playUri);
}

bool MediaController::PlayAlbumId(const std::string& albumId) {
    if (albumId.empty()) return false;
    std::string playUri = "music://music.apple.com/album/" + albumId;
    return PlayUrl(playUri);
}

} // namespace ammod::remote
