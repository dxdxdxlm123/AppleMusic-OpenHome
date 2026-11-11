#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ammod::remote {

struct TrackSnapshot {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::string trackId;
    int32_t trackNumber = 0;
    int64_t durationMs = 0;
    int64_t positionMs = 0;
    bool isPlaying = false;
    bool isPaused = false;
    std::vector<uint8_t> artworkBytes;
    std::string artworkMime = "image/jpeg";
};

class MediaController {
public:
    MediaController();
    ~MediaController();

    bool Start();
    void Stop();

    TrackSnapshot GetSnapshot();
    std::vector<uint8_t> GetArtwork(std::string& outMime);

    bool Play();
    bool Pause();
    bool TogglePlayPause();
    bool SkipNext();
    bool SkipPrevious();
    bool Seek(double seconds);
    bool PlayTrack(const std::string& albumUrl, const std::wstring& trackName = L"");
    bool PlayUrl(const std::string& url);
    bool PlayTrackId(const std::string& trackId);
    bool PlayAlbumId(const std::string& albumId);
    bool AddAlbumToLibrary(const std::string& albumId);

    void SetOnTrackChanged(std::function<void(const TrackSnapshot&)> callback) {
        std::lock_guard lock(mutex_);
        onTrackChanged_ = std::move(callback);
    }

    void SetOnPlaybackStateChanged(std::function<void(const TrackSnapshot&)> callback) {
        std::lock_guard lock(mutex_);
        onStateChanged_ = std::move(callback);
    }

    void SetOnTimeChanged(std::function<void(const TrackSnapshot&)> callback) {
        std::lock_guard lock(mutex_);
        onTimeChanged_ = std::move(callback);
    }

private:
    void WorkerLoop();
    void PollSession();

    std::mutex mutex_;
    TrackSnapshot currentSnapshot_;
    bool running_ = false;
    HANDLE workerThread_ = nullptr;

    std::function<void(const TrackSnapshot&)> onTrackChanged_;
    std::function<void(const TrackSnapshot&)> onStateChanged_;
    std::function<void(const TrackSnapshot&)> onTimeChanged_;
};

} // namespace ammod::remote
