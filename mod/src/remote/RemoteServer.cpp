#include "RemoteServer.h"
#include "AmPluginClient.h"
#include "WebRemoteHtml.h"
#include "PlayQueueReader.h"
#include "AmExclusiveConfig.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <mutex>

using json = nlohmann::json;

namespace ammod::remote {

namespace {

const char* kAppleDeveloperToken = "eyJ0eXAiOiJKV1QiLCJhbGciOiJFUzI1NiIsImtpZCI6IldlYlBsYXlLaWQifQ.eyJpc3MiOiJBTVBXZWJQbGF5IiwiaWF0IjoxNzkyNzgwNTM0LCJleHAiOjE3OTg4Mjg1MzQsInJvb3RfaHR0cHNfb3JpZ2luIjpbImFwcGxlLmNvbSJdfQ.wcf_7U-zcgUjY2L8d4HWFkdxP1YSJCkkqEpEtVPKpRARrlaag0DHyIpRcoUEWZU8kJlvx5Q7ZNp04OabfuiuAw";

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), chars);
    return result;
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << int(c);
        }
    }
    return escaped.str();
}

struct CachedResourceInfo {
    std::string name;
    std::string url;
};

std::mutex g_trackCacheMutex;
std::unordered_map<std::string, CachedResourceInfo> g_trackCache;
std::unordered_map<std::string, CachedResourceInfo> g_albumCache;
std::unordered_map<std::string, CachedResourceInfo> g_playlistCache;

struct CachedAlbumMeta {
    std::string albumId;
    std::string artworkUrl;
    std::string releaseDate;
};

std::mutex g_albumMetaMutex;
std::unordered_map<std::string, CachedAlbumMeta> g_albumMetaCache;
std::unordered_map<std::string, std::string> g_songArtworkCache;

// Forward declarations
void SaveFavorites();
void SaveLibSongs();
void SaveLibAlbums();

// Favorites persistence
std::mutex g_favoritesMutex;
json g_favorites = json::array();
bool g_favoritesLoaded = false;

std::wstring GetFavoritesFilePath() {
    return L"C:\\Users\\Public\\AMExclusive\\am_favorites.json";
}

void LoadFavoritesIfNeeded() {
    std::lock_guard lock(g_favoritesMutex);
    std::wstring path = GetFavoritesFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(hFile, nullptr);
        if (size > 0 && size < 10 * 1024 * 1024) {
            std::string content(size, '\0');
            DWORD read = 0;
            ReadFile(hFile, content.data(), size, &read, nullptr);
            try {
                g_favorites = json::parse(content);
                g_favoritesLoaded = true;
                CloseHandle(hFile);
                return;
            } catch (...) {}
        }
        CloseHandle(hFile);
    }

    if (g_favoritesLoaded && !g_favorites.empty()) return;
    g_favoritesLoaded = true;

    if (g_favorites.empty()) {
        g_favorites = json::array({
            {{"id", "635770200"}, {"name", "加州旅馆"}, {"artist", "Eagles"}, {"album", "加州旅馆"}, {"artworkUrl", "https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/88/16/2c/88162c3d-46db-8321-61f3-3a47404cfe76/075596050920.jpg/{w}x{h}bb.jpg"}},
            {{"id", "1544083976"}, {"name", "怪物"}, {"artist", "YOASOBI"}, {"album", "怪物 - Single"}, {"artworkUrl", "https://is1-ssl.mzstatic.com/image/thumb/Music221/v4/9e/ab/2c/9eab2c24-8289-1bd7-548d-e6b871fcf169/195497732630.jpg/{w}x{h}bb.jpg"}},
            {{"id", "547158777"}, {"name", "说谎"}, {"artist", "林宥嘉"}, {"album", "感官/世界"}, {"artworkUrl", "https://is1-ssl.mzstatic.com/image/thumb/Music124/v4/57/e5/9b/57e59bd6-3564-e4c4-1add-e3dbbf4d8ab1/asset.jpg/{w}x{h}bb.jpg"}}
        });
        SaveFavorites();
    }
}

void SaveFavorites() {
    std::wstring path = GetFavoritesFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        std::string s = g_favorites.dump(2);
        DWORD written = 0;
        WriteFile(hFile, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
        CloseHandle(hFile);
    }
}

bool ToggleFavorite(const json& item) {
    LoadFavoritesIfNeeded();
    std::lock_guard lock(g_favoritesMutex);
    std::string id = item.value("id", "");
    std::string name = item.value("name", "");
    std::string artist = item.value("artist", "");
    if (id.empty() && name.empty()) return false;

    for (auto it = g_favorites.begin(); it != g_favorites.end(); ++it) {
        bool match = false;
        if (!id.empty() && (*it).value("id", "") == id) match = true;
        else if ((*it).value("name", "") == name && (*it).value("artist", "") == artist) match = true;
        if (match) {
            g_favorites.erase(it);
            SaveFavorites();
            return false; // removed
        }
    }

    g_favorites.push_back(item);
    SaveFavorites();
    return true; // added
}

// Library Songs persistence
std::mutex g_libSongsMutex;
json g_libSongs = json::array();
bool g_libSongsLoaded = false;

std::wstring GetLibSongsFilePath() {
    return L"C:\\Users\\Public\\AMExclusive\\am_library_songs.json";
}

void LoadLibSongsIfNeeded() {
    std::lock_guard lock(g_libSongsMutex);
    std::wstring path = GetLibSongsFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(hFile, nullptr);
        if (size > 0 && size < 10 * 1024 * 1024) {
            std::string content(size, '\0');
            DWORD read = 0;
            ReadFile(hFile, content.data(), size, &read, nullptr);
            try {
                g_libSongs = json::parse(content);
                g_libSongsLoaded = true;
                CloseHandle(hFile);
                return;
            } catch (...) {}
        }
        CloseHandle(hFile);
    }

    if (g_libSongsLoaded && !g_libSongs.empty()) return;
    g_libSongsLoaded = true;

    if (g_libSongs.empty()) {
        auto recents = PlayQueueReader::ReadRecentItems();
        for (const auto& r : recents) {
            g_libSongs.push_back({
                {"id", r.adamId},
                {"name", r.title},
                {"artist", r.artist},
                {"album", r.album},
                {"artworkUrl", r.artworkUrl}
            });
        }
        SaveLibSongs();
    }
}

void SaveLibSongs() {
    std::wstring path = GetLibSongsFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        std::string s = g_libSongs.dump(2);
        DWORD written = 0;
        WriteFile(hFile, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
        CloseHandle(hFile);
    }
}

bool ToggleLibSong(const json& item) {
    LoadLibSongsIfNeeded();
    std::lock_guard lock(g_libSongsMutex);
    std::string id = item.value("id", "");
    std::string name = item.value("name", "");
    std::string artist = item.value("artist", "");
    if (id.empty() && name.empty()) return false;

    for (auto it = g_libSongs.begin(); it != g_libSongs.end(); ++it) {
        bool match = false;
        if (!id.empty() && (*it).value("id", "") == id) match = true;
        else if ((*it).value("name", "") == name && (*it).value("artist", "") == artist) match = true;
        if (match) {
            g_libSongs.erase(it);
            SaveLibSongs();
            return false; // removed
        }
    }

    g_libSongs.push_back(item);
    SaveLibSongs();
    return true; // added
}

// Library Albums persistence
std::mutex g_libAlbumsMutex;
json g_libAlbums = json::array();
bool g_libAlbumsLoaded = false;

std::wstring GetLibAlbumsFilePath() {
    return L"C:\\Users\\Public\\AMExclusive\\am_library_albums.json";
}

void LoadLibAlbumsIfNeeded() {
    std::lock_guard lock(g_libAlbumsMutex);
    std::wstring path = GetLibAlbumsFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(hFile, nullptr);
        if (size > 0 && size < 10 * 1024 * 1024) {
            std::string content(size, '\0');
            DWORD read = 0;
            ReadFile(hFile, content.data(), size, &read, nullptr);
            try {
                g_libAlbums = json::parse(content);
                g_libAlbumsLoaded = true;
                CloseHandle(hFile);
                return;
            } catch (...) {}
        }
        CloseHandle(hFile);
    }

    if (g_libAlbumsLoaded && !g_libAlbums.empty()) return;
    g_libAlbumsLoaded = true;

    if (g_libAlbums.empty()) {
        auto recents = PlayQueueReader::ReadRecentAlbums();
        for (const auto& alb : recents) {
            g_libAlbums.push_back({
                {"id", alb.albumId.empty() ? alb.firstAdamId : alb.albumId},
                {"name", alb.name},
                {"artist", alb.artist},
                {"trackCount", alb.trackCount},
                {"artworkUrl", alb.artworkUrl},
                {"releaseDate", alb.releaseDate}
            });
        }
        SaveLibAlbums();
    }
}

void SaveLibAlbums() {
    std::wstring path = GetLibAlbumsFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        std::string s = g_libAlbums.dump(2);
        DWORD written = 0;
        WriteFile(hFile, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
        CloseHandle(hFile);
    }
}

bool ToggleLibAlbum(const json& item) {
    LoadLibAlbumsIfNeeded();
    std::lock_guard lock(g_libAlbumsMutex);
    std::string id = item.value("id", "");
    std::string name = item.value("name", "");
    std::string artist = item.value("artist", "");
    if (id.empty() && name.empty()) return false;

    for (auto it = g_libAlbums.begin(); it != g_libAlbums.end(); ++it) {
        bool match = false;
        if (!id.empty() && (*it).value("id", "") == id) match = true;
        else if ((*it).value("name", "") == name && (*it).value("artist", "") == artist) match = true;
        if (match) {
            g_libAlbums.erase(it);
            SaveLibAlbums();
            return false; // removed
        }
    }

    g_libAlbums.push_back(item);
    SaveLibAlbums();
    return true; // added
}

void LogDebugMsg(const char* msg) {
    FILE* f = fopen("C:\\Users\\Public\\AMExclusive\\remote_debug.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

void RunLibrarySyncScript() {
    try {
        std::wstring scriptPath = L"C:\\Users\\Public\\AMExclusive\\sync_library_from_am.py";
        if (GetFileAttributesW(scriptPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return;
        }

        std::wstring cmd = L"python.exe \"C:\\Users\\Public\\AMExclusive\\sync_library_from_am.py\"";
        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = { 0 };
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    } catch (...) {
        LogDebugMsg("Exception in RunLibrarySyncScript");
    }
}

json FetchAppleMusicApi(const std::string& path, const std::string& query = "") {
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return json::object();

    WinHttpSetTimeouts(hSession, 4000, 4000, 8000, 8000);
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.music.apple.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return json::object();
    }

    std::string fullPath = path;
    if (!query.empty()) {
        fullPath += (fullPath.find('?') == std::string::npos ? "?" : "&") + query;
    }
    std::wstring wPath = Widen(fullPath);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    std::string responseBody;
    if (hRequest) {
        std::wstring headers = L"Authorization: Bearer " + Widen(kAppleDeveloperToken) +
                               L"\r\nOrigin: https://music.apple.com\r\nAccept: application/json\r\n";
        if (WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD bytesAvailable = 0;
            while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                std::vector<char> buffer(bytesAvailable);
                DWORD bytesRead = 0;
                if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
                    responseBody.append(buffer.data(), bytesRead);
                } else {
                    break;
                }
            }
        }
        WinHttpCloseHandle(hRequest);
    }
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (responseBody.empty()) return json::object();
    try {
        return json::parse(responseBody);
    } catch (...) {
        return json::object();
    }
}

CachedResourceInfo ResolveTrackInfo(const std::string& trackId) {
    if (trackId.empty()) return {};
    {
        std::lock_guard lock(g_trackCacheMutex);
        auto it = g_trackCache.find(trackId);
        if (it != g_trackCache.end() && !it->second.url.empty()) {
            return it->second;
        }
    }

    // Lookup via Apple Music Streaming API
    auto resp = FetchAppleMusicApi("/v1/catalog/cn/songs/" + trackId);
    if (resp.contains("data") && resp["data"].is_array() && !resp["data"].empty()) {
        const auto& attrs = resp["data"][0].value("attributes", json::object());
        std::string name = attrs.value("name", "");
        std::string url = attrs.value("url", "");
        if (!name.empty() && !url.empty()) {
            CachedResourceInfo info{ name, url };
            std::lock_guard lock(g_trackCacheMutex);
            g_trackCache[trackId] = info;
            return info;
        }
    }
    return {};
}

CachedResourceInfo ResolveAlbumInfo(const std::string& albumId) {
    if (albumId.empty()) return {};
    {
        std::lock_guard lock(g_trackCacheMutex);
        auto it = g_albumCache.find(albumId);
        if (it != g_albumCache.end()) return it->second;
    }
    auto resp = FetchAppleMusicApi("/v1/catalog/cn/albums/" + albumId);
    if (resp.contains("data") && resp["data"].is_array() && !resp["data"].empty()) {
        const auto& attrs = resp["data"][0].value("attributes", json::object());
        std::string name = attrs.value("name", "");
        std::string url = attrs.value("url", "");
        if (!url.empty()) {
            CachedResourceInfo info{ name, url };
            std::lock_guard lock(g_trackCacheMutex);
            g_albumCache[albumId] = info;
            return info;
        }
    }
    return { "", "music://music.apple.com/album/" + albumId };
}

CachedResourceInfo ResolvePlaylistInfo(const std::string& playlistId) {
    if (playlistId.empty()) return {};
    {
        std::lock_guard lock(g_trackCacheMutex);
        auto it = g_playlistCache.find(playlistId);
        if (it != g_playlistCache.end()) return it->second;
    }
    auto resp = FetchAppleMusicApi("/v1/catalog/cn/playlists/" + playlistId);
    if (resp.contains("data") && resp["data"].is_array() && !resp["data"].empty()) {
        const auto& attrs = resp["data"][0].value("attributes", json::object());
        std::string name = attrs.value("name", "");
        std::string url = attrs.value("url", "");
        if (!url.empty()) {
            CachedResourceInfo info{ name, url };
            std::lock_guard lock(g_trackCacheMutex);
            g_playlistCache[playlistId] = info;
            return info;
        }
    }
    return { "", "music://music.apple.com/playlist/" + playlistId };
}

json FetchAppleSearch(const std::string& term, const std::string& types, int limit) {
    if (term.empty()) {
        return {
            {"data", {
                {"results", json::object()}
            }}
        };
    }

    std::string q = "term=" + UrlEncode(term) + "&types=" + types + "&limit=" + std::to_string(limit);
    auto apiRes = FetchAppleMusicApi("/v1/catalog/cn/search", q);

    if (apiRes.contains("results")) {
        // Pre-cache song names and URLs from results if present
        if (apiRes["results"].contains("songs") && apiRes["results"]["songs"].contains("data")) {
            std::lock_guard lock(g_trackCacheMutex);
            for (const auto& song : apiRes["results"]["songs"]["data"]) {
                std::string sId = song.value("id", "");
                if (song.contains("attributes")) {
                    const auto& attrs = song["attributes"];
                    std::string sName = attrs.value("name", "");
                    std::string sUrl = attrs.value("url", "");
                    if (!sId.empty() && !sName.empty() && !sUrl.empty()) {
                        g_trackCache[sId] = { sName, sUrl };
                    }
                }
            }
        }
        return {
            {"data", {
                {"results", apiRes["results"]}
            }}
        };
    }

    return {
        {"data", {
            {"results", json::object()}
        }}
    };
}

void PopulateRecentMetadata(const std::vector<AlbumItem>& albums, const std::vector<LibraryItem>& tracks) {
    std::vector<std::string> missingSongIds;
    {
        std::lock_guard lock(g_albumMetaMutex);
        for (const auto& alb : albums) {
            if (!alb.firstAdamId.empty()) {
                if (g_albumMetaCache.find(alb.firstAdamId) == g_albumMetaCache.end()) {
                    missingSongIds.push_back(alb.firstAdamId);
                }
            }
        }
        for (const auto& tr : tracks) {
            if (!tr.adamId.empty() && g_songArtworkCache.find(tr.adamId) == g_songArtworkCache.end()) {
                if (std::find(missingSongIds.begin(), missingSongIds.end(), tr.adamId) == missingSongIds.end()) {
                    missingSongIds.push_back(tr.adamId);
                }
            }
            if (missingSongIds.size() >= 30) break;
        }
    }

    if (!missingSongIds.empty()) {
        std::string idsParam;
        for (size_t i = 0; i < missingSongIds.size(); ++i) {
            if (i > 0) idsParam += ",";
            idsParam += missingSongIds[i];
        }

        auto songBatch = FetchAppleMusicApi("/v1/catalog/cn/songs", "ids=" + idsParam + "&include=albums");
        if (songBatch.contains("data") && songBatch["data"].is_array()) {
            std::lock_guard lock(g_albumMetaMutex);
            for (const auto& item : songBatch["data"]) {
                std::string sId = item.value("id", "");
                const auto& attrs = item.value("attributes", json::object());
                std::string artUrl = "";
                if (attrs.contains("artwork") && attrs["artwork"].is_object()) {
                    artUrl = attrs["artwork"].value("url", "");
                }
                std::string relDate = attrs.value("releaseDate", "");
                std::string albId = "";
                if (item.contains("relationships") && item["relationships"].contains("albums") &&
                    item["relationships"]["albums"].contains("data") && !item["relationships"]["albums"]["data"].empty()) {
                    albId = item["relationships"]["albums"]["data"][0].value("id", "");
                }
                if (albId.empty() && attrs.contains("url")) {
                    std::string u = attrs.value("url", "");
                    auto pos1 = u.find("/album/");
                    if (pos1 != std::string::npos) {
                        auto pos2 = u.find('/', pos1 + 7);
                        if (pos2 != std::string::npos) {
                            auto pos3 = u.find('?', pos2 + 1);
                            albId = (pos3 != std::string::npos) ? u.substr(pos2 + 1, pos3 - (pos2 + 1)) : u.substr(pos2 + 1);
                        }
                    }
                }
                if (!sId.empty()) {
                    g_albumMetaCache[sId] = { albId, artUrl, relDate };
                    if (!artUrl.empty()) g_songArtworkCache[sId] = artUrl;
                }
                std::string albName = attrs.value("albumName", "");
                std::string artName = attrs.value("artistName", "");
                if (!albName.empty() && !artName.empty()) {
                    g_albumMetaCache[albName + "|||" + artName] = { albId, artUrl, relDate };
                }
            }
        }
    }

    // Secondary fallback for any album without resolved albumId
    for (const auto& alb : albums) {
        bool resolved = false;
        {
            std::lock_guard lock(g_albumMetaMutex);
            if (!alb.firstAdamId.empty()) {
                auto it = g_albumMetaCache.find(alb.firstAdamId);
                if (it != g_albumMetaCache.end() && !it->second.albumId.empty()) {
                    resolved = true;
                }
            }
            if (!resolved) {
                auto it2 = g_albumMetaCache.find(alb.name + "|||" + alb.artist);
                if (it2 != g_albumMetaCache.end() && !it2->second.albumId.empty()) {
                    resolved = true;
                }
            }
        }
        if (!resolved && !alb.name.empty()) {
            auto srch = FetchAppleSearch(alb.name + " " + alb.artist, "albums", 1);
            if (srch.contains("data") && srch["data"].contains("results") &&
                srch["data"]["results"].contains("albums") && srch["data"]["results"]["albums"].contains("data") &&
                !srch["data"]["results"]["albums"]["data"].empty()) {
                const auto& firstAlb = srch["data"]["results"]["albums"]["data"][0];
                std::string foundAlbId = firstAlb.value("id", "");
                const auto& aAttrs = firstAlb.value("attributes", json::object());
                std::string foundArtUrl = "";
                if (aAttrs.contains("artwork") && aAttrs["artwork"].is_object()) {
                    foundArtUrl = aAttrs["artwork"].value("url", "");
                }
                std::string foundRelDate = aAttrs.value("releaseDate", "");
                std::lock_guard lock(g_albumMetaMutex);
                CachedAlbumMeta meta{ foundAlbId, foundArtUrl, foundRelDate };
                if (!alb.firstAdamId.empty()) g_albumMetaCache[alb.firstAdamId] = meta;
                g_albumMetaCache[alb.name + "|||" + alb.artist] = meta;
            }
        }
    }
}

json HandleLibraryApi(const std::string& path, const std::string& hostAddress, int port) {
    LoadFavoritesIfNeeded();
    auto recentItems = PlayQueueReader::ReadRecentItems();
    auto recentAlbums = PlayQueueReader::ReadRecentAlbums();
    auto recentArtists = PlayQueueReader::ReadRecentArtists();

    PopulateRecentMetadata(recentAlbums, recentItems);

    std::string baseArtwork = "http://" + hostAddress + ":" + std::to_string(port) + "/api/v2/playback/artwork";

    if (path.find("recently-added") != std::string::npos || path.find("albums") != std::string::npos) {
        json dataArr = json::array();
        for (size_t i = 0; i < recentAlbums.size() && i < 25; ++i) {
            const auto& alb = recentAlbums[i];
            std::string albId = "";
            std::string artUrl = "";
            {
                std::lock_guard lock(g_albumMetaMutex);
                auto it = g_albumMetaCache.find(alb.firstAdamId);
                if (it != g_albumMetaCache.end() && !it->second.albumId.empty()) {
                    albId = it->second.albumId;
                    artUrl = it->second.artworkUrl;
                } else {
                    auto it2 = g_albumMetaCache.find(alb.name + "|||" + alb.artist);
                    if (it2 != g_albumMetaCache.end() && !it2->second.albumId.empty()) {
                        albId = it2->second.albumId;
                        artUrl = it2->second.artworkUrl;
                    }
                }
            }
            if (albId.empty()) {
                albId = alb.firstAdamId.empty() ? ("alb_" + std::to_string(i)) : alb.firstAdamId;
            }
            if (artUrl.empty()) artUrl = baseArtwork;

            dataArr.push_back({
                {"id", albId},
                {"type", "library-albums"},
                {"href", "/v1/me/library/albums/" + albId},
                {"attributes", {
                    {"name", alb.name},
                    {"artistName", alb.artist},
                    {"trackCount", alb.trackCount},
                    {"playParams", {
                        {"id", albId},
                        {"kind", "album"},
                        {"isLibrary", true}
                    }},
                    {"artwork", {
                        {"url", artUrl}
                    }}
                }},
                {"relationships", {
                    {"catalog", {
                        {"data", json::array({{{"id", albId}, {"type", "albums"}}})}
                    }}
                }}
            });
        }
        return {{"data", dataArr}};
    } else if (path.find("songs") != std::string::npos) {
        json dataArr = json::array();
        for (size_t i = 0; i < recentItems.size() && i < 50; ++i) {
            const auto& it = recentItems[i];
            std::string sId = it.adamId.empty() ? ("song_" + std::to_string(i)) : it.adamId;
            dataArr.push_back({
                {"id", sId},
                {"type", "library-songs"},
                {"href", "/v1/me/library/songs/" + sId},
                {"attributes", {
                    {"name", it.title},
                    {"artistName", it.artist},
                    {"albumName", it.album},
                    {"playParams", {
                        {"id", sId},
                        {"kind", "song"},
                        {"isLibrary", true}
                    }},
                    {"artwork", {
                        {"url", baseArtwork}
                    }}
                }}
            });
        }
        return {{"data", dataArr}};
    } else if (path.find("playlists") != std::string::npos || path.find("pins") != std::string::npos) {
        json dataArr = json::array();
        dataArr.push_back({
            {"id", "pl.recent"},
            {"type", "library-playlists"},
            {"href", "/v1/me/library/playlists/pl.recent"},
            {"attributes", {
                {"name", "最近播放 (Recently Played)"},
                {"curatorName", "AMExclusive"},
                {"trackCount", recentItems.size()},
                {"playParams", {
                    {"id", "recent"},
                    {"kind", "playlist"},
                    {"isLibrary", true}
                }},
                {"artwork", {{"url", baseArtwork}}}
            }}
        });
        {
            std::lock_guard lock(g_favoritesMutex);
            if (!g_favorites.empty()) {
                dataArr.push_back({
                    {"id", "pl.favorites"},
                    {"type", "library-playlists"},
                    {"href", "/v1/me/library/playlists/pl.favorites"},
                    {"attributes", {
                        {"name", "我喜爱的音乐 (Favorites)"},
                        {"curatorName", "AMExclusive"},
                        {"trackCount", g_favorites.size()},
                        {"playParams", {
                            {"id", "favorites"},
                            {"kind", "playlist"},
                            {"isLibrary", true}
                        }},
                        {"artwork", {{"url", baseArtwork}}}
                    }}
                });
            }
        }
        return {{"data", dataArr}};
    } else if (path.find("artists") != std::string::npos) {
        json dataArr = json::array();
        for (size_t i = 0; i < recentArtists.size() && i < 25; ++i) {
            const auto& art = recentArtists[i];
            dataArr.push_back({
                {"id", "art_" + std::to_string(i)},
                {"type", "library-artists"},
                {"attributes", {
                    {"name", art.name},
                    {"trackCount", art.trackCount}
                }}
            });
        }
        return {{"data", dataArr}};
    }

    return {{"data", json::array()}};
}

} // namespace

RemoteServer::RemoteServer(int port) : port_(port) {
    server_ = std::make_unique<httplib::Server>();
    SetupRoutes();

    controller_.SetOnTrackChanged([this](const TrackSnapshot& snap) {
        OnTrackChanged(snap);
    });

    controller_.SetOnPlaybackStateChanged([this](const TrackSnapshot& snap) {
        OnStateChanged(snap);
    });

    controller_.SetOnTimeChanged([this](const TrackSnapshot& snap) {
        OnTimeChanged(snap);
    });
}

RemoteServer::~RemoteServer() {
    Stop();
}

bool RemoteServer::Start() {
    if (running_) return true;
    running_ = true;

    controller_.Start();

    // Run server in background thread
    std::thread([this]() {
        LogDebugMsg("Server listen thread starting");
        try {
            if (!server_->listen("0.0.0.0", port_)) {
                LogDebugMsg("server_->listen returned false");
            }
        } catch (const std::exception& e) {
            std::string err = std::string("Exception in server_->listen: ") + e.what();
            LogDebugMsg(err.c_str());
        } catch (...) {
            LogDebugMsg("Unknown exception in server_->listen");
        }
        LogDebugMsg("Server listen thread exiting");
    }).detach();

    // Background watcher for Apple Music library database changes
    std::thread([this]() {
        LogDebugMsg("Watcher thread starting");
        try {
            wchar_t userProfile[MAX_PATH] = { 0 };
            DWORD len = GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH);
            if (len == 0) return;
            std::wstring dbPath = std::wstring(userProfile) + L"\\Music\\Apple Music\\Apple Music Library.musiclibrary\\artwork.sqlite";
            std::wstring walPath = dbPath + L"-wal";

            FILETIME lastDbTime = { 0, 0 };
            FILETIME lastWalTime = { 0, 0 };

            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!running_) break;
                bool changed = false;

                WIN32_FILE_ATTRIBUTE_DATA fad;
                if (GetFileAttributesExW(dbPath.c_str(), GetFileExInfoStandard, &fad)) {
                    if (lastDbTime.dwLowDateTime != 0 &&
                        (fad.ftLastWriteTime.dwLowDateTime != lastDbTime.dwLowDateTime ||
                         fad.ftLastWriteTime.dwHighDateTime != lastDbTime.dwHighDateTime)) {
                        changed = true;
                    }
                    lastDbTime = fad.ftLastWriteTime;
                }

                if (GetFileAttributesExW(walPath.c_str(), GetFileExInfoStandard, &fad)) {
                    if (lastWalTime.dwLowDateTime != 0 &&
                        (fad.ftLastWriteTime.dwLowDateTime != lastWalTime.dwLowDateTime ||
                         fad.ftLastWriteTime.dwHighDateTime != lastWalTime.dwHighDateTime)) {
                        changed = true;
                    }
                    lastWalTime = fad.ftLastWriteTime;
                }

                if (changed) {
                    LogDebugMsg("Watcher detected artwork database change, running sync");
                    RunLibrarySyncScript();
                    LoadLibAlbumsIfNeeded();
                    LoadLibSongsIfNeeded();
                }
            }
        } catch (const std::exception& e) {
            std::string err = std::string("Exception in watcher thread: ") + e.what();
            LogDebugMsg(err.c_str());
        } catch (...) {
            LogDebugMsg("Unknown exception in watcher thread");
        }
        LogDebugMsg("Watcher thread exiting");
    }).detach();

    return true;
}

void RemoteServer::Stop() {
    if (!running_) return;
    running_ = false;

    if (server_) {
        server_->stop();
    }
    controller_.Stop();
}

std::string RemoteServer::BuildNowPlayingJson(const TrackSnapshot& snapshot) {
    std::string title = Narrow(snapshot.title);
    std::string artist = Narrow(snapshot.artist);
    std::string album = Narrow(snapshot.album);

    if (!title.empty() && !artist.empty() && title != "未在播放") {
        PlayQueueReader::RecordTrack(title, artist, album, "");
    }

    json track = {
        {"name", title},
        {"artistName", artist},
        {"albumName", album},
        {"durationInMillis", snapshot.durationMs},
        {"hasLyrics", false},
        {"hasTimeSyncedLyrics", false},
        {"inLibrary", true},
        {"audioTraits", json::array({"lossless", "hi-res"})},
        {"artwork", {
            {"url", "http://" + hostAddress_ + ":" + std::to_string(port_) + "/api/v2/playback/artwork"}
        }},
        {"playParams", {
            {"id", "local_apple_music"},
            {"kind", "song"}
        }}
    };

    return track.dump();
}

std::string RemoteServer::BuildPlaybackSnapshotJson(const TrackSnapshot& snapshot) {
    std::string stateStr = snapshot.isPlaying ? "playing" : "paused";
    double curSec = static_cast<double>(snapshot.positionMs) / 1000.0;
    double durSec = static_cast<double>(snapshot.durationMs) / 1000.0;
    double remSec = durSec > curSec ? (durSec - curSec) : 0.0;

    json trackJson = json::parse(BuildNowPlayingJson(snapshot));

    json snapshotJson = {
        {"data", {
            {"state", stateStr},
            {"volume", 1.0},
            {"rate", 1.0},
            {"repeatMode", "none"},
            {"shuffleMode", false},
            {"autoplay", false},
            {"nowPlaying", trackJson},
            {"time", {
                {"currentTime", curSec},
                {"duration", durSec},
                {"remaining", remSec}
            }}
        }}
    };

    return snapshotJson.dump();
}

void RemoteServer::OnTrackChanged(const TrackSnapshot& snapshot) {
    json trackJson = json::parse(BuildNowPlayingJson(snapshot));
    json eventJson = json::array({"API:Playback", {
        {"type", "playbackStatus.nowPlayingItemDidChange"},
        {"data", trackJson}
    }});
    BroadcastWebSocket("42" + eventJson.dump());
}

void RemoteServer::OnStateChanged(const TrackSnapshot& snapshot) {
    std::string stateStr = snapshot.isPlaying ? "playing" : "paused";
    double curSec = static_cast<double>(snapshot.positionMs) / 1000.0;
    json eventJson = json::array({"API:Playback", {
        {"type", "playbackStatus.playbackStateDidChange"},
        {"data", {
            {"state", stateStr},
            {"anchor", {
                {"currentTime", curSec},
                {"rate", 1.0}
            }}
        }}
    }});
    BroadcastWebSocket("42" + eventJson.dump());
}

void RemoteServer::OnTimeChanged(const TrackSnapshot& snapshot) {
    double curSec = static_cast<double>(snapshot.positionMs) / 1000.0;
    double durSec = static_cast<double>(snapshot.durationMs) / 1000.0;
    json eventJson = json::array({"API:Playback", {
        {"type", "playbackStatus.playbackTimeDidChange"},
        {"data", {
            {"currentPlaybackTime", curSec},
            {"currentPlaybackDuration", durSec},
            {"isPlaying", snapshot.isPlaying},
            {"anchor", {
                {"currentTime", curSec},
                {"duration", durSec},
                {"rate", 1.0}
            }}
        }}
    }});
    BroadcastWebSocket("42" + eventJson.dump());
}

void RemoteServer::BroadcastWebSocket(const std::string& message) {
    std::lock_guard lock(wsMutex_);
    for (auto it = activeSockets_.begin(); it != activeSockets_.end(); ) {
        auto* ws = *it;
        if (ws && ws->is_open()) {
            ws->send(message);
            ++it;
        } else {
            it = activeSockets_.erase(it);
        }
    }
}

void RemoteServer::SetupRoutes() {
    server_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response&) {
        std::cout << "[REMOTE] " << req.method << " " << req.path << std::endl;
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // CORS & Options handler
    server_->Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "*");
        res.status = 204;
    });

    // Web Remote UI
    server_->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto html = GetWebRemoteHtml();
        res.set_content(html.data(), html.size(), "text/html; charset=utf-8");
    });

    // PWA Manifest
    server_->Get("/manifest.json", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        json j = {
            {"name", "AMExclusive 遥控器"},
            {"short_name", "AMExclusive"},
            {"start_url", "/"},
            {"display", "standalone"},
            {"background_color", "#000000"},
            {"theme_color", "#121214"},
            {"orientation", "portrait"},
            {"icons", json::array({
                {{"src", "/icon-192.png"}, {"sizes", "192x192"}, {"type", "image/png"}},
                {{"src", "/icon-512.png"}, {"sizes", "512x512"}, {"type", "image/png"}}
            })}
        };
        res.set_content(j.dump(2), "application/manifest+json");
    });

    // PWA Icon
    server_->Get("/icon-192.png", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto icon = GetAppIconPng();
        res.set_content(icon.data(), icon.size(), "image/png");
    });

    server_->Get("/icon-512.png", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto icon = GetAppIconPng();
        res.set_content(icon.data(), icon.size(), "image/png");
    });

    // Health check
    server_->Get("/api/v1/playback/active", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // Client Info / Discovery
    server_->Get("/api/v2/client/info", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto token = req.get_header_value("apptoken");
        if (token.empty()) {
            res.status = 403;
            res.set_content("{\"error\":\"UNAUTHORIZED_APP_TOKEN\"}", "application/json");
            return;
        }
        json j = {
            {"framework", "AMExclusive"},
            {"platform", "windows"},
            {"version", "0.1.0"},
            {"port", 10767}
        };
        res.set_content(j.dump(), "application/json");
    });

    // Pairing auth request (Auto-approve)
    server_->Post("/api/v2/auth/request", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        json j = {
            {"data", {
                {"token", "ame_token_authorized"},
                {"scopes", {"playback", "queue", "library", "audio", "account"}}
            }}
        };
        res.set_content(j.dump(), "application/json");
    });

    // Snapshot
    server_->Get("/api/v2/playback", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (req.has_header("Host")) {
            std::string host = req.get_header_value("Host");
            auto colon = host.find(':');
            if (colon != std::string::npos) host = host.substr(0, colon);
            hostAddress_ = host;
        }
        res.set_content(BuildPlaybackSnapshotJson(controller_.GetSnapshot()), "application/json");
    });

    // Now Playing
    server_->Get("/api/v2/playback/now-playing", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        json j = {{"data", json::parse(BuildNowPlayingJson(controller_.GetSnapshot()))}};
        res.set_content(j.dump(), "application/json");
    });

    // State
    server_->Get("/api/v2/playback/state", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto snap = controller_.GetSnapshot();
        json j = {{"data", {{"state", snap.isPlaying ? "playing" : "paused"}}}};
        res.set_content(j.dump(), "application/json");
    });

    // Time
    server_->Get("/api/v2/playback/time", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto snap = controller_.GetSnapshot();
        double curSec = static_cast<double>(snap.positionMs) / 1000.0;
        double durSec = static_cast<double>(snap.durationMs) / 1000.0;
        json j = {{"data", {{"currentTime", curSec}, {"duration", durSec}, {"remaining", durSec - curSec}}}};
        res.set_content(j.dump(), "application/json");
    });

    // Audio Quality (AMExclusive format monitor)
    server_->Get("/api/v2/playback/audio-quality", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto status = AmPluginClient::Instance().GetStatus();
        std::string flavor = "alac";
        std::string label = "Lossless";
        if (status.state == 6) { // Active
            if (status.format.find("192000") != std::string::npos ||
                status.format.find("96000") != std::string::npos ||
                status.format.find("88200") != std::string::npos ||
                status.format.find("176400") != std::string::npos) {
                label = "Hi-Res Lossless · " + status.format;
            } else if (!status.format.empty()) {
                label = "Lossless · " + status.format;
            } else {
                label = "Lossless · Bit-Perfect Exclusive";
            }
        } else if (status.connected && status.enabled) {
            label = "Lossless (Exclusive Armed)";
        } else {
            label = "High Quality";
        }
        json j = {
            {"data", {
                {"flavor", flavor},
                {"flavorLabel", label}
            }}
        };
        res.set_content(j.dump(), "application/json");
    });

    // Artwork
    server_->Get("/api/v2/playback/artwork", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string mime;
        auto bytes = controller_.GetArtwork(mime);
        if (!bytes.empty()) {
            res.set_content(reinterpret_cast<const char*>(bytes.data()), bytes.size(), mime);
        } else {
            res.status = 404;
        }
    });

    // Controls
    server_->Post("/api/v2/playback/play", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        controller_.Play();
        res.set_content("{\"data\":{}}", "application/json");
    });

    server_->Post("/api/v2/playback/pause", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        controller_.Pause();
        res.set_content("{\"data\":{}}", "application/json");
    });

    server_->Post("/api/v2/playback/toggle", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        controller_.TogglePlayPause();
        res.set_content("{\"data\":{}}", "application/json");
    });

    server_->Post("/api/v2/playback/next", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        controller_.SkipNext();
        res.set_content("{\"data\":{}}", "application/json");
    });

    server_->Post("/api/v2/playback/previous", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        controller_.SkipPrevious();
        res.set_content("{\"data\":{}}", "application/json");
    });

    server_->Post("/api/v2/playback/seek", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto j = json::parse(req.body);
            if (j.contains("position") && j["position"].is_number()) {
                controller_.Seek(j["position"].get<double>());
            }
        } catch (...) {}
        res.set_content("{\"data\":{}}", "application/json");
    });

    // Play Item / Play URL
    server_->Post("/api/v2/playback/play-item", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto j = json::parse(req.body);
            std::string id;
            if (j.contains("id")) {
                if (j["id"].is_string()) {
                    id = j["id"].get<std::string>();
                } else if (j["id"].is_number()) {
                    id = std::to_string(j["id"].get<int64_t>());
                }
            }
            std::string type = j.value("type", "songs");
            std::string name = j.value("name", "");

            if (!id.empty()) {
                std::cout << "[REMOTE] Play Item Type: " << type << ", ID: " << id << ", Name: " << name << std::endl;
                if (type == "albums" || type == "library-albums") {
                    auto albInfo = ResolveAlbumInfo(id);
                    std::cout << "[REMOTE] Resolved Album: " << albInfo.name << ", URL: " << albInfo.url << std::endl;
                    controller_.PlayTrack(albInfo.url, L"");
                } else if (type == "playlists" || type == "library-playlists") {
                    auto plInfo = ResolvePlaylistInfo(id);
                    std::cout << "[REMOTE] Resolved Playlist: " << plInfo.name << ", URL: " << plInfo.url << std::endl;
                    controller_.PlayTrack(plInfo.url, L"");
                } else {
                    auto info = ResolveTrackInfo(id);
                    std::string trackName = !name.empty() ? name : info.name;
                    std::cout << "[REMOTE] Resolved Track: " << trackName << ", URL: " << info.url << std::endl;
                    if (!info.url.empty()) {
                        controller_.PlayTrack(info.url, Widen(trackName));
                    } else {
                        controller_.PlayTrackId(id);
                    }
                }
            }
        } catch (...) {}
        res.set_content("{\"data\":{}}", "application/json");
    });

    server_->Post("/api/v2/playback/play-url", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto j = json::parse(req.body);
            if (j.contains("url") && j["url"].is_string()) {
                std::string url = j["url"].get<std::string>();
                controller_.PlayTrack(url, L"");
            }
        } catch (...) {}
        res.set_content("{\"data\":{}}", "application/json");
    });

    server_->Post("/api/v2/playback/play-collection", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto j = json::parse(req.body);
            if (j.contains("id") && j["id"].is_string()) {
                std::string id = j["id"].get<std::string>();
                std::string url = "music://music.apple.com/album/" + id;
                controller_.PlayTrack(url, L"");
            }
        } catch (...) {}
        res.set_content("{\"data\":{}}", "application/json");
    });

    // Queue & Volume placeholders
    server_->Get("/api/v2/queue", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"data\":{\"items\":[],\"position\":0},\"meta\":{\"total\":0}}", "application/json");
    });

    server_->Get("/api/v2/queue/position", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"data\":{\"position\":0,\"total\":1}}", "application/json");
    });

    server_->Get("/api/v2/library/now-playing/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"data\":{\"inLibrary\":true,\"rating\":0}}", "application/json");
    });

    // Dedicated Library Endpoints for Web Remote
    server_->Get("/api/v2/library/recent", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto recentItems = PlayQueueReader::ReadRecentItems();
        auto recentAlbums = PlayQueueReader::ReadRecentAlbums();
        auto recentArtists = PlayQueueReader::ReadRecentArtists();

        PopulateRecentMetadata(recentAlbums, recentItems);

        json tracksArr = json::array();
        for (const auto& it : recentItems) {
            std::string artUrl = it.artworkUrl;
            if (artUrl.empty()) {
                std::lock_guard lock(g_albumMetaMutex);
                auto f = g_songArtworkCache.find(it.adamId);
                if (f != g_songArtworkCache.end()) {
                    artUrl = f->second;
                } else {
                    auto f2 = g_albumMetaCache.find(it.album + "|||" + it.artist);
                    if (f2 != g_albumMetaCache.end()) {
                        artUrl = f2->second.artworkUrl;
                    }
                }
            }
            if (artUrl.empty() && !it.album.empty()) {
                LoadLibAlbumsIfNeeded();
                std::lock_guard lock(g_libAlbumsMutex);
                for (const auto& a : g_libAlbums) {
                    std::string aname = a.value("name", "");
                    if (aname == it.album || aname.find(it.album) != std::string::npos || it.album.find(aname) != std::string::npos) {
                        artUrl = a.value("artworkUrl", "");
                        break;
                    }
                }
            }
            tracksArr.push_back({
                {"title", it.title},
                {"artist", it.artist},
                {"album", it.album},
                {"adamId", it.adamId},
                {"date", it.date},
                {"artworkUrl", artUrl}
            });
        }

        json albumsArr = json::array();
        for (const auto& alb : recentAlbums) {
            std::string albId = alb.albumId.empty() ? alb.firstAdamId : alb.albumId;
            std::string artUrl = alb.artworkUrl;
            std::string relDate = alb.releaseDate;
            if (artUrl.empty() || albId.empty()) {
                std::lock_guard lock(g_albumMetaMutex);
                auto it = g_albumMetaCache.find(alb.firstAdamId);
                if (it != g_albumMetaCache.end()) {
                    if (albId.empty()) albId = it->second.albumId;
                    if (artUrl.empty()) artUrl = it->second.artworkUrl;
                    if (relDate.empty()) relDate = it->second.releaseDate;
                } else {
                    auto it2 = g_albumMetaCache.find(alb.name + "|||" + alb.artist);
                    if (it2 != g_albumMetaCache.end()) {
                        if (albId.empty()) albId = it2->second.albumId;
                        if (artUrl.empty()) artUrl = it2->second.artworkUrl;
                        if (relDate.empty()) relDate = it2->second.releaseDate;
                    }
                }
            }
            albumsArr.push_back({
                {"name", alb.name},
                {"artist", alb.artist},
                {"trackCount", alb.trackCount},
                {"firstAdamId", alb.firstAdamId},
                {"albumId", albId},
                {"artworkUrl", artUrl},
                {"releaseDate", relDate}
            });
        }

        json artistsArr = json::array();
        for (const auto& art : recentArtists) {
            artistsArr.push_back({
                {"name", art.name},
                {"trackCount", art.trackCount}
            });
        }

        json j = {
            {"data", {
                {"tracks", tracksArr},
                {"albums", albumsArr},
                {"artists", artistsArr}
            }}
        };
        res.set_content(j.dump(), "application/json");
    });

    server_->Get("/api/v2/library/favorites", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        LoadFavoritesIfNeeded();
        std::lock_guard lock(g_favoritesMutex);
        json j = {{"data", g_favorites}};
        res.set_content(j.dump(), "application/json");
    });

    server_->Post("/api/v2/library/favorites/toggle", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool added = false;
        try {
            auto j = json::parse(req.body);
            added = ToggleFavorite(j);
        } catch (...) {}
        res.set_content(json({{"success", true}, {"favorited", added}}).dump(), "application/json");
    });

    server_->Get("/api/v2/library/user-state", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        LoadFavoritesIfNeeded();
        LoadLibSongsIfNeeded();
        LoadLibAlbumsIfNeeded();
        json j = json::object();
        {
            std::lock_guard lock(g_favoritesMutex);
            j["favorites"] = g_favorites;
        }
        {
            std::lock_guard lock(g_libSongsMutex);
            j["songs"] = g_libSongs;
        }
        {
            std::lock_guard lock(g_libAlbumsMutex);
            j["albums"] = g_libAlbums;
        }
        res.set_content(json({{"status", "ok"}, {"data", j}}).dump(), "application/json");
    });

    server_->Get("/api/v2/library/songs", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        LoadLibSongsIfNeeded();
        std::lock_guard lock(g_libSongsMutex);
        json j = {{"data", g_libSongs}};
        res.set_content(j.dump(), "application/json");
    });

    server_->Post("/api/v2/library/songs/toggle", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool added = false;
        try {
            auto j = json::parse(req.body);
            added = ToggleLibSong(j);
        } catch (...) {}
        res.set_content(json({{"success", true}, {"inLibrary", added}}).dump(), "application/json");
    });

    server_->Post("/api/v2/library/sync", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        RunLibrarySyncScript();
        LoadLibAlbumsIfNeeded();
        LoadLibSongsIfNeeded();
        std::lock_guard lock(g_libAlbumsMutex);
        std::lock_guard lock2(g_libSongsMutex);
        json j = {
            {"status", "ok"},
            {"albumsCount", g_libAlbums.size()},
            {"songsCount", g_libSongs.size()}
        };
        res.set_content(j.dump(), "application/json");
    });

    server_->Get("/api/v2/library/sync", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        RunLibrarySyncScript();
        LoadLibAlbumsIfNeeded();
        LoadLibSongsIfNeeded();
        std::lock_guard lock(g_libAlbumsMutex);
        std::lock_guard lock2(g_libSongsMutex);
        json j = {
            {"status", "ok"},
            {"albumsCount", g_libAlbums.size()},
            {"songsCount", g_libSongs.size()}
        };
        res.set_content(j.dump(), "application/json");
    });

    server_->Get("/api/v2/library/albums", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        LoadLibAlbumsIfNeeded();
        std::lock_guard lock(g_libAlbumsMutex);
        json j = {{"data", g_libAlbums}};
        res.set_content(j.dump(), "application/json");
    });

    server_->Post("/api/v2/library/albums/toggle", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool added = false;
        try {
            auto j = json::parse(req.body);
            std::string id = j.value("id", "");
            if (!id.empty()) {
                if (id.rfind("catalog.", 0) == 0) id = id.substr(8);
                controller_.AddAlbumToLibrary(id);
            }
            added = ToggleLibAlbum(j);
        } catch (...) {}
        res.set_content(json({{"success", true}, {"inLibrary", added}}).dump(), "application/json");
    });

    // Recommendations & Charts endpoint
    server_->Get("/api/v2/amapi/charts", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string limitStr = req.get_param_value("limit");
        std::string types = req.get_param_value("types");
        if (types.empty()) types = "songs,albums,playlists";
        if (limitStr.empty()) limitStr = "20";
        auto apiRes = FetchAppleMusicApi("/v1/catalog/cn/charts", "types=" + types + "&limit=" + limitStr);
        json wrapped = {{"data", apiRes}};
        res.set_content(wrapped.dump(), "application/json");
    });

    // Apple Music API raw proxy with user library interceptor
    server_->Get(R"(/api/v2/amapi/raw/(.*))", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string path = req.matches[1];

        // Intercept Cider App's personal library calls
        if (path.rfind("v1/me/library", 0) == 0) {
            std::cout << "[REMOTE] Handling library request: " << path << std::endl;
            json libRes = HandleLibraryApi(path, hostAddress_, port_);
            res.set_content(libRes.dump(), "application/json");
            return;
        }

        size_t sf = path.find("$STOREFRONT");
        while (sf != std::string::npos) {
            path.replace(sf, 11, "cn");
            sf = path.find("$STOREFRONT");
        }
        std::string query;
        for (const auto& [k, v] : req.params) {
            if (!query.empty()) query += "&";
            query += UrlEncode(k) + "=" + UrlEncode(v);
        }
        auto apiRes = FetchAppleMusicApi("/" + path, query);
        json wrapped = {{"data", apiRes}};
        res.set_content(wrapped.dump(), "application/json");
    });

    server_->Get("/api/v2/audio/volume", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"data\":{\"volume\":1.0}}", "application/json");
    });

    // Search catalog (Apple Music Streaming Mainland China Catalog)
    server_->Get("/api/v2/amapi/search", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string term = req.get_param_value("term");
        std::string types = req.get_param_value("types");
        if (types.empty()) types = "songs,albums,artists,playlists";
        std::string limitStr = req.get_param_value("limit");
        int limit = 15;
        if (!limitStr.empty()) {
            try { limit = std::stoi(limitStr); } catch (...) {}
        }
        json searchResult = FetchAppleSearch(term, types, limit);
        res.set_content(searchResult.dump(), "application/json");
    });

    // =========================================================================
    // AMExclusive Plugin Control Endpoints
    // =========================================================================

    // Plugin Status & DAC format monitor
    server_->Get("/api/v2/amexclusive/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto status = AmPluginClient::Instance().GetStatus();
        json j = {
            {"connected", status.connected},
            {"state", status.state},
            {"stateName", status.stateName},
            {"enabled", status.enabled},
            {"hardwareBufferMs", status.hardwareBufferMs},
            {"allowResampling", status.allowResampling},
            {"endpointId", status.endpointId},
            {"endpointName", status.endpointName},
            {"format", status.format},
            {"detail", status.detail},
            {"errorCategory", status.errorCategory},
            {"errorString", status.errorString}
        };
        res.set_content(j.dump(), "application/json");
    });

    // Audio Output Devices (WASAPI DACs, ASIO drivers, DLNA renderers, Roon endpoints)
    server_->Get("/api/v2/amexclusive/devices", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto devices = AmPluginClient::Instance().GetDevices();
        json arr = json::array();
        for (const auto& d : devices) {
            arr.push_back({
                {"id", d.id},
                {"name", d.name},
                {"type", d.type},
                {"online", d.online},
                {"location", d.location},
                {"address", d.address},
                {"port", d.port},
                {"raatVersion", d.raatVersion},
                {"note", d.note}
            });
        }
        json j = {{"devices", arr}};
        res.set_content(j.dump(), "application/json");
    });

    // Get full AMExclusive configuration from am-exclusive.ini
    server_->Get("/api/v2/amexclusive/config", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto config = ReadFullAmExclusiveConfig();
        res.set_content(config.dump(), "application/json");
    });

    // Save AMExclusive configuration to am-exclusive.ini and optionally apply
    server_->Post("/api/v2/amexclusive/config", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool ok = false;
        try {
            auto j = json::parse(req.body);
            ok = SaveFullAmExclusiveConfig(j);
            bool apply = j.value("apply", false);
            if (apply) {
                std::string activeTab = j.value("active_tab", "");
                if (activeTab.empty() && j.contains("mod") && j["mod"].contains("output_tab")) {
                    activeTab = j["mod"]["output_tab"].get<std::string>();
                }
                if (activeTab == "local" || activeTab == "0") {
                    std::string output = "wasapi";
                    if (j.contains("mod") && j["mod"].contains("output")) {
                        output = j["mod"]["output"].get<std::string>();
                    }
                    if (output == "asio") {
                        std::string clsid = j.contains("mod") ? j["mod"].value("asio_driver", "") : "";
                        std::string name = j.contains("mod") ? j["mod"].value("asio_name", "") : "";
                        AmPluginClient::Instance().SetOutputTarget(2, Widen(clsid), Widen(name), L"");
                    } else {
                        std::string id = j.contains("mod") ? j["mod"].value("wasapi_device", "") : "";
                        std::string name = j.contains("mod") ? j["mod"].value("wasapi_name", "") : "";
                        AmPluginClient::Instance().SetOutputTarget(0, Widen(id), Widen(name), L"");
                    }
                } else if (activeTab == "network" || activeTab == "1") {
                    std::string udn = j.contains("dlna") ? j["dlna"].value("device_udn", "") : "";
                    std::string name = j.contains("dlna") ? j["dlna"].value("device_name", "") : "";
                    std::string loc = j.contains("dlna") ? j["dlna"].value("location", "") : "";
                    AmPluginClient::Instance().SetOutputTarget(1, Widen(udn), Widen(name), Widen(loc));
                } else if (activeTab == "r_device" || activeTab == "roon" || activeTab == "2") {
                    std::string id = j.contains("roon") ? j["roon"].value("device_id", "") : "";
                    std::string name = j.contains("roon") ? j["roon"].value("device_name", "") : "";
                    std::string addr = j.contains("roon") ? j["roon"].value("address", "") : "";
                    AmPluginClient::Instance().SetOutputTarget(3, Widen(id), Widen(name), Widen(addr));
                }
                if (j.contains("mod")) {
                    if (j["mod"].contains("allow_resampling")) {
                        int v = j["mod"]["allow_resampling"].is_boolean() ? (j["mod"]["allow_resampling"].get<bool>() ? 1 : 0) : j["mod"]["allow_resampling"].get<int>();
                        AmPluginClient::Instance().SetAllowResampling(v == 1);
                    }
                    if (j["mod"].contains("buffer_ms")) {
                        int ms = j["mod"]["buffer_ms"].get<int>();
                        AmPluginClient::Instance().SetHardwareBuffer(static_cast<uint32_t>(ms));
                    }
                }
                // Restart current track to apply output changes immediately
                AmPluginClient::Instance().RequestRestartCurrent();
            }
        } catch (...) {
            ok = false;
        }
        res.set_content(json({{"success", ok}}).dump(), "application/json");
    });

    // Restart current playing track
    server_->Post("/api/v2/amexclusive/restart-current", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool ok = AmPluginClient::Instance().RequestRestartCurrent();
        res.set_content(json({{"success", ok}}).dump(), "application/json");
    });

    // Manually add network device URL
    server_->Post("/api/v2/amexclusive/add-device", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool ok = false;
        try {
            auto j = json::parse(req.body);
            std::string url = j.value("url", "");
            if (!url.empty()) {
                ok = AmPluginClient::Instance().AddManualDevice(Widen(url));
            }
        } catch (...) {}
        res.set_content(json({{"success", ok}}).dump(), "application/json");
    });

    // Toggle Exclusive Mode
    server_->Post("/api/v2/amexclusive/set-enabled", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool enabled = false;
        try {
            auto j = json::parse(req.body);
            enabled = j.value("enabled", false);
        } catch (...) {}
        bool ok = AmPluginClient::Instance().SetEnabled(enabled);
        res.set_content(json({{"success", ok}}).dump(), "application/json");
    });

    // Set Hardware Buffer Size (e.g. 5ms, 10ms, 20ms, 50ms)
    server_->Post("/api/v2/amexclusive/set-buffer", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        uint32_t ms = 20;
        try {
            auto j = json::parse(req.body);
            ms = j.value("bufferMs", 20u);
        } catch (...) {}
        bool ok = AmPluginClient::Instance().SetHardwareBuffer(ms);
        res.set_content(json({{"success", ok}}).dump(), "application/json");
    });

    // Set Allow Resampling
    server_->Post("/api/v2/amexclusive/set-resampling", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool allow = false;
        try {
            auto j = json::parse(req.body);
            allow = j.value("allow", false);
        } catch (...) {}
        bool ok = AmPluginClient::Instance().SetAllowResampling(allow);
        res.set_content(json({{"success", ok}}).dump(), "application/json");
    });

    // Select Active Audio Output Target (DAC / ASIO / DLNA / Roon)
    server_->Post("/api/v2/amexclusive/set-device", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool ok = false;
        try {
            auto j = json::parse(req.body);
            std::string type = j.value("type", "wasapi");
            std::string id = j.value("id", "");
            std::string name = j.value("name", "");
            std::string loc = j.value("location", "");
            uint8_t kind = 0;
            if (type == "dlna") kind = 1;
            else if (type == "asio") kind = 2;
            else if (type == "raat" || type == "roon") kind = 3;
            ok = AmPluginClient::Instance().SetOutputTarget(kind, Widen(id), Widen(name), Widen(loc));
        } catch (...) {}
        res.set_content(json({{"success", ok}}).dump(), "application/json");
    });

    // Trigger Device Discovery Refresh
    server_->Post("/api/v2/amexclusive/refresh-devices", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        AmPluginClient::Instance().RefreshDevices();
        res.set_content("{\"success\":true}", "application/json");
    });

    // WebSocket /socket.io/ (Engine.IO v4 handler)
    server_->WebSocket("/socket.io/", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
        {
            std::lock_guard lock(wsMutex_);
            activeSockets_.insert(&ws);
        }

        // Send Engine.IO Open packet
        ws.send("0{\"sid\":\"ame_session_1\",\"upgrades\":[],\"pingInterval\":25000,\"pingTimeout\":20000,\"maxPayload\":1000000}");

        std::string msg;
        while (ws.is_open()) {
            auto res = ws.read(msg);
            if (!res) break;

            if (msg.rfind("40", 0) == 0) {
                // Client Socket.IO connect -> reply connected
                ws.send("40{\"sid\":\"ame_session_1\"}");

                // Send current now-playing item and state immediately
                auto snap = controller_.GetSnapshot();
                json trackJson = json::parse(BuildNowPlayingJson(snap));
                json event1 = json::array({"API:Playback", {
                    {"type", "playbackStatus.nowPlayingItemDidChange"},
                    {"data", trackJson}
                }});
                ws.send("42" + event1.dump());

                json event2 = json::array({"API:Playback", {
                    {"type", "playbackStatus.playbackStateDidChange"},
                    {"data", {
                        {"state", snap.isPlaying ? "playing" : "paused"},
                        {"anchor", {
                            {"currentTime", static_cast<double>(snap.positionMs) / 1000.0},
                            {"rate", 1.0}
                        }}
                    }}
                }});
                ws.send("42" + event2.dump());
            } else if (msg == "2") {
                // Ping -> reply Pong
                ws.send("3");
            }
        }

        {
            std::lock_guard lock(wsMutex_);
            activeSockets_.erase(&ws);
        }
    });
}

} // namespace ammod::remote
