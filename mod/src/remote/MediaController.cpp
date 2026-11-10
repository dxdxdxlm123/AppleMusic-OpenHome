#include "MediaController.h"

#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <uiautomation.h>
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

bool MediaController::PlayTrack(const std::string& albumUrl, const std::wstring& trackName) {
    if (albumUrl.empty() && trackName.empty()) return false;

    if (!albumUrl.empty()) {
        std::string playUri = albumUrl;

        // Strip tracking parameters
        size_t uoPos = playUri.find("&uo=");
        if (uoPos != std::string::npos) {
            size_t nextAmp = playUri.find('&', uoPos + 1);
            if (nextAmp != std::string::npos) {
                playUri.erase(uoPos, nextAmp - uoPos);
            } else {
                playUri.erase(uoPos);
            }
        }

        // Convert https:// or http:// to music://
        if (playUri.rfind("https://", 0) == 0) {
            playUri = "music://" + playUri.substr(8);
        } else if (playUri.rfind("http://", 0) == 0) {
            playUri = "music://" + playUri.substr(7);
        }

        std::wstring wUri = ToWide(playUri);
        ShellExecuteW(nullptr, L"open", wUri.c_str(), nullptr, nullptr, SW_SHOW);
    }

    // Launch UI Automation background worker to activate playback of the track
    std::thread([this, trackName]() {
        // Wait for Apple Music to navigate and begin rendering the target page
        Sleep(1200);

        // Switch worker thread to user's interactive desktop
        HDESK hDesk = OpenDesktopW(L"default", 0, FALSE, GENERIC_ALL);
        if (hDesk) {
            SetThreadDesktop(hDesk);
        }

        CoInitialize(nullptr);

        IUIAutomation* pAutomation = nullptr;
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IUIAutomation, reinterpret_cast<void**>(&pAutomation));

        bool trackActivated = false;

        std::wstring coreTitle = trackName;
        auto paren = coreTitle.find(L" (");
        if (paren != std::wstring::npos) coreTitle = coreTitle.substr(0, paren);
        auto dash = coreTitle.find(L" - ");
        if (dash != std::wstring::npos) coreTitle = coreTitle.substr(0, dash);

        // Polling loop: up to 15 attempts x 350ms (~5.25 seconds)
        for (int attempt = 0; attempt < 15 && !trackActivated; ++attempt) {
            HWND amHwnd = nullptr;
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                if (!IsWindowVisible(hwnd)) return TRUE;
                wchar_t cls[256]{};
                GetClassNameW(hwnd, cls, 256);
                wchar_t title[256]{};
                GetWindowTextW(hwnd, title, 256);
                if (wcscmp(cls, L"WinUIDesktopWin32WindowClass") == 0 && wcsstr(title, L"Apple Music") != nullptr) {
                    *reinterpret_cast<HWND*>(lParam) = hwnd;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&amHwnd));

            if (!amHwnd || !pAutomation) {
                Sleep(350);
                continue;
            }

            if (IsIconic(amHwnd)) ShowWindow(amHwnd, SW_RESTORE);
            SetForegroundWindow(amHwnd);

            IUIAutomationElement* pRoot = nullptr;
            if (FAILED(pAutomation->ElementFromHandle(amHwnd, &pRoot)) || !pRoot) {
                Sleep(350);
                continue;
            }

            IUIAutomationCondition* pTrueCond = nullptr;
            pAutomation->CreateTrueCondition(&pTrueCond);

            // 1. If we have a track name, look for the ListViewItem matching trackName
            if (!trackName.empty() && pTrueCond) {
                IUIAutomationElementArray* pArray = nullptr;
                if (SUCCEEDED(pRoot->FindAll(TreeScope_Descendants, pTrueCond, &pArray)) && pArray) {
                    int len = 0;
                    pArray->get_Length(&len);
                    std::cout << "[CONTROLLER] Attempt " << attempt << ": " << len << " elements in view, looking for: "
                              << std::string(trackName.begin(), trackName.end()) << std::endl;
                    for (int i = 0; i < len; ++i) {
                        IUIAutomationElement* pElem = nullptr;
                        pArray->GetElement(i, &pElem);
                        if (!pElem) continue;

                        BSTR name{};
                        CONTROLTYPEID ctrlType{};
                        pElem->get_CurrentName(&name);
                        pElem->get_CurrentControlType(&ctrlType);

                        if (name && ctrlType == UIA_ListItemControlTypeId) {
                            if (wcsstr(name, trackName.c_str()) != nullptr ||
                                (!coreTitle.empty() && wcsstr(name, coreTitle.c_str()) != nullptr)) {
                                RECT rect{};
                                pElem->get_CurrentBoundingRectangle(&rect);
                                std::cout << "[CONTROLLER] Found matching ListViewItem! Rect: "
                                          << rect.left << "," << rect.top << " to " << rect.right << "," << rect.bottom << std::endl;
                                if (rect.right > rect.left && rect.bottom > rect.top) {
                                    int cx = rect.left + 80;
                                    int cy = (rect.top + rect.bottom) / 2;

                                    POINT origPos{};
                                    GetCursorPos(&origPos);

                                    SetCursorPos(cx, cy);
                                    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                                    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                                    Sleep(50);
                                    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                                    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                                    SetCursorPos(origPos.x, origPos.y);

                                    std::cout << "[CONTROLLER] Double-clicked at (" << cx << ", " << cy << ")!" << std::endl;
                                    trackActivated = true;
                                    SysFreeString(name);
                                    pElem->Release();
                                    break;
                                }
                            }
                        }
                        if (name) SysFreeString(name);
                        pElem->Release();
                    }
                    pArray->Release();
                }
            }

            // 2. If trackName is empty (e.g. play album), invoke PlayButtonElement directly
            if (!trackActivated && trackName.empty()) {
                VARIANT varProp;
                varProp.vt = VT_BSTR;
                varProp.bstrVal = SysAllocString(L"PlayButtonElement");
                IUIAutomationCondition* pCond = nullptr;
                pAutomation->CreatePropertyCondition(UIA_AutomationIdPropertyId, varProp, &pCond);
                SysFreeString(varProp.bstrVal);

                if (pCond) {
                    IUIAutomationElement* pPlayBtn = nullptr;
                    if (SUCCEEDED(pRoot->FindFirst(TreeScope_Descendants, pCond, &pPlayBtn)) && pPlayBtn) {
                        IUIAutomationInvokePattern* pInvoke = nullptr;
                        if (SUCCEEDED(pPlayBtn->GetCurrentPattern(UIA_InvokePatternId, reinterpret_cast<IUnknown**>(&pInvoke))) && pInvoke) {
                            pInvoke->Invoke();
                            pInvoke->Release();
                            trackActivated = true;
                        }
                        pPlayBtn->Release();
                    }
                    pCond->Release();
                }
            }

            if (pTrueCond) pTrueCond->Release();
            pRoot->Release();

            if (!trackActivated) Sleep(350);
        }

        if (pAutomation) pAutomation->Release();
        CoUninitialize();

        // 3. Fallback: if not started after all attempts, attempt GSMTC Play
        if (!trackActivated) {
            this->Play();
        }
    }).detach();

    return true;
}

bool MediaController::PlayUrl(const std::string& url) {
    return PlayTrack(url, L"");
}

bool MediaController::PlayTrackId(const std::string& trackId) {
    if (trackId.empty()) return false;
    std::string playUri = "music://music.apple.com/song/" + trackId;
    return PlayTrack(playUri, L"");
}

bool MediaController::PlayAlbumId(const std::string& albumId) {
    if (albumId.empty()) return false;
    std::string playUri = "music://music.apple.com/album/" + albumId;
    return PlayTrack(playUri, L"");
}

} // namespace ammod::remote
