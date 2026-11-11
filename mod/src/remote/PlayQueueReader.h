#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>

namespace ammod::remote {

struct LibraryItem {
    std::string title;
    std::string artist;
    std::string album;
    std::string adamId;
    std::string date;
    std::string artworkUrl;
};

struct AlbumItem {
    std::string name;
    std::string artist;
    std::string firstAdamId;
    std::string albumId;
    std::string artworkUrl;
    std::string releaseDate;
    int trackCount{0};
};

struct ArtistItem {
    std::string name;
    int trackCount{0};
};

inline std::mutex g_playHistoryMutex;

class PlayQueueReader {
public:
    static std::wstring GetPlayQueuePlistPath() {
        wchar_t musicPath[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, musicPath))) {
            std::wstring p = musicPath;
            p += L"\\Apple Music\\Apple Music Library.musiclibrary\\preferences\\PlayQueueState.plist";
            return p;
        }
        return L"";
    }

    static std::wstring GetHistoryFilePath() {
        return L"C:\\Users\\Public\\AMExclusive\\am_playback_history.json";
    }

    static std::vector<LibraryItem> GetDefaultSeedItems() {
        return {
            {"加州旅馆", "Eagles", "加州旅馆", "635770200", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/88/16/2c/88162c3d-46db-8321-61f3-3a47404cfe76/075596050920.jpg/{w}x{h}bb.jpg"},
            {"怪物", "YOASOBI", "怪物 - Single", "1544083976", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music221/v4/9e/ab/2c/9eab2c24-8289-1bd7-548d-e6b871fcf169/195497732630.jpg/{w}x{h}bb.jpg"},
            {"群青", "YOASOBI", "群青 - Single", "1528119626", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/ae/7e/4a/ae7e4a28-fd46-9617-1066-fcbd124303d6/195497105656.jpg/{w}x{h}bb.jpg"},
            {"夜に駆ける", "YOASOBI", "夜に駆ける - Single", "1490256978", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/fe/58/79/fe58795c-1628-230a-9ffe-300c04b6627c/194491717186.jpg/{w}x{h}bb.jpg"},
            {"说谎", "林宥嘉", "感官/世界", "547158777", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music124/v4/57/e5/9b/57e59bd6-3564-e4c4-1add-e3dbbf4d8ab1/asset.jpg/{w}x{h}bb.jpg"},
            {"想自由", "林宥嘉", "美妙生活", "545213762", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music124/v4/3e/f9/49/3ef94944-d61a-b3dd-6a4a-d4d23e41ccbd/asset.jpg/{w}x{h}bb.jpg"},
            {"Michi Teyu Ku (Overflowing)", "藤井风", "Michi Teyu Ku (Overflowing) - Single", "1733129984", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music122/v4/d0/f4/ad/d0f4ad92-2a6c-6a02-34c5-d45b93f767d9/24UMGIM15199.rgb.jpg/{w}x{h}bb.jpg"},
            {"温柔", "五月天", "爱情万岁", "150093860", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/af/c2/33/afc233f6-d570-8f22-0a91-14a3e289a30d/dj.uhlsejtl.jpg/{w}x{h}bb.jpg"},
            {"突然好想你", "五月天", "后 青春期的诗", "1081381033", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music69/v4/fd/ba/9e/fdba9e89-6336-d3e4-ee00-94184f9a5c2c/BD0016-_-.jpg/{w}x{h}bb.jpg"},
            {"离开地球表面", "五月天", "离开地球表面", "1081738606", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music124/v4/ad/a5/8f/ada58f96-3fa7-8547-ee40-2766ea9d9fec/BD0009-_-.jpg/{w}x{h}bb.jpg"},
            {"派对动物", "五月天", "自传", "1158763922", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music126/v4/64/a0/f1/64a0f1b8-a36b-ff10-509a-9a8f5fef53ff/dj.zfoqlnar.jpg/{w}x{h}bb.jpg"},
            {"ヘビリンゴ (feat. ねね)", "THREEE", "ヘビリンゴ (feat. ねね) - Single", "1575366854", "2026-11-11", "https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/ce/7b/e8/ce7be864-469b-57fc-e8d1-ca27e5ecf00b/cover.jpg/{w}x{h}bb.jpg"}
        };
    }

    static std::vector<LibraryItem> LoadHistoryFromFile() {
        std::vector<LibraryItem> results;
        std::wstring path = GetHistoryFilePath();
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(hFile, nullptr);
            if (size > 0 && size < 10 * 1024 * 1024) {
                std::string content(size, '\0');
                DWORD read = 0;
                ReadFile(hFile, content.data(), size, &read, nullptr);
                try {
                    auto j = nlohmann::json::parse(content);
                    if (j.is_array()) {
                        for (const auto& it : j) {
                            if (!it.is_object()) continue;
                            LibraryItem item;
                            item.title = it.value("title", it.value("name", ""));
                            item.artist = it.value("artist", it.value("artistName", ""));
                            item.album = it.value("album", it.value("albumName", ""));
                            item.adamId = it.value("adamId", it.value("id", ""));
                            item.date = it.value("date", "");
                            item.artworkUrl = it.value("artworkUrl", "");
                            if (!item.title.empty()) {
                                results.push_back(item);
                            }
                        }
                    }
                } catch (...) {}
            }
            CloseHandle(hFile);
        }
        return results;
    }

    static void SaveHistoryToFile(const std::vector<LibraryItem>& items) {
        std::wstring path = GetHistoryFilePath();
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            nlohmann::json j = nlohmann::json::array();
            for (const auto& it : items) {
                j.push_back({
                    {"title", it.title},
                    {"artist", it.artist},
                    {"album", it.album},
                    {"adamId", it.adamId},
                    {"date", it.date},
                    {"artworkUrl", it.artworkUrl}
                });
            }
            std::string s = j.dump(2);
            DWORD written = 0;
            WriteFile(hFile, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
            CloseHandle(hFile);
        }
    }

    static void RecordTrack(const std::string& title, const std::string& artist, const std::string& album, const std::string& adamId = "", const std::string& artworkUrl = "") {
        if (title.empty() || title == "未在播放") return;
        std::lock_guard lock(g_playHistoryMutex);
        auto items = LoadHistoryFromFile();
        if (items.empty()) {
            items = GetDefaultSeedItems();
        }

        std::string realArtist = artist;
        std::string realAlbum = album;
        // Split if artist is formatted as "Artist — Album"
        size_t sep = realArtist.find(" — ");
        if (sep != std::string::npos && realAlbum.empty()) {
            std::string a = realArtist.substr(0, sep);
            std::string b = realArtist.substr(sep + 5);
            auto trim = [](std::string& s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            };
            trim(a);
            trim(b);
            realArtist = a;
            realAlbum = b;
        }

        if (!items.empty() && items.front().title == title && items.front().artist == realArtist) {
            return;
        }

        for (auto it = items.begin(); it != items.end(); ++it) {
            if (it->title == title && (it->artist == realArtist || it->artist == artist)) {
                items.erase(it);
                break;
            }
        }

        std::string art = artworkUrl;
        if (art.empty()) {
            // Match from am_library_albums.json
            std::wstring albPath = L"C:\\Users\\Public\\AMExclusive\\am_library_albums.json";
            HANDLE hFile = CreateFileW(albPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD size = GetFileSize(hFile, nullptr);
                if (size > 0 && size < 5 * 1024 * 1024) {
                    std::string content(size, '\0');
                    DWORD read = 0;
                    ReadFile(hFile, content.data(), size, &read, nullptr);
                    try {
                        auto j = nlohmann::json::parse(content);
                        if (j.is_array()) {
                            for (const auto& a : j) {
                                std::string aname = a.value("name", "");
                                if (!realAlbum.empty() && (aname == realAlbum || aname.find(realAlbum) != std::string::npos || realAlbum.find(aname) != std::string::npos)) {
                                    art = a.value("artworkUrl", "");
                                    break;
                                }
                            }
                        }
                    } catch (...) {}
                }
                CloseHandle(hFile);
            }
        }

        LibraryItem newItem;
        newItem.title = title;
        newItem.artist = realArtist;
        newItem.album = realAlbum;
        newItem.adamId = adamId;
        newItem.artworkUrl = art;
        newItem.date = "刚刚播放";
        items.insert(items.begin(), newItem);

        if (items.size() > 100) items.resize(100);
        SaveHistoryToFile(items);
    }

    static std::vector<LibraryItem> ParsePlistItems() {
        std::vector<LibraryItem> results;
        std::wstring plistPath = GetPlayQueuePlistPath();
        if (plistPath.empty()) return results;

        // Open with FILE_SHARE_READ | FILE_SHARE_WRITE
        HANDLE hFile = CreateFileW(plistPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return results;

        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize < 64 || fileSize > 10 * 1024 * 1024) {
            CloseHandle(hFile);
            return results;
        }

        std::vector<uint8_t> data(fileSize);
        DWORD bytesRead = 0;
        if (!ReadFile(hFile, data.data(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize) {
            CloseHandle(hFile);
            return results;
        }
        CloseHandle(hFile);

        if (fileSize < 32 || memcmp(data.data(), "bplist00", 8) != 0) {
            return results;
        }

        // Parse bplist trailer (last 32 bytes)
        const uint8_t* trailer = data.data() + fileSize - 32;
        uint8_t offsetIntSize = trailer[6];
        uint8_t objectRefSize = trailer[7];

        auto readBE64 = [](const uint8_t* p) -> uint64_t {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
            return v;
        };

        uint64_t numObjects = readBE64(trailer + 8);
        uint64_t topObject = readBE64(trailer + 16);
        uint64_t tableOffset = readBE64(trailer + 24);

        if (offsetIntSize == 0 || objectRefSize == 0 || numObjects == 0 || tableOffset >= fileSize) {
            return results;
        }

        auto readOffset = [&](uint64_t idx) -> uint64_t {
            if (idx >= numObjects) return 0;
            const uint8_t* p = data.data() + tableOffset + idx * offsetIntSize;
            if (p + offsetIntSize > data.data() + fileSize) return 0;
            uint64_t off = 0;
            for (int i = 0; i < offsetIntSize; ++i) off = (off << 8) | p[i];
            return off;
        };

        auto readRef = [&](const uint8_t* p) -> uint64_t {
            uint64_t ref = 0;
            for (int i = 0; i < objectRefSize; ++i) ref = (ref << 8) | p[i];
            return ref;
        };

        // Recursive reader into nlohmann::json
        auto readObject = [&](auto& self, uint64_t idx) -> nlohmann::json {
            uint64_t pos = readOffset(idx);
            if (pos >= fileSize) return nullptr;

            uint8_t header = data[pos++];
            uint8_t tag = (header >> 4) & 0x0F;
            uint64_t size = header & 0x0F;

            if (size == 15) {
                if (pos >= fileSize) return nullptr;
                uint8_t h2 = data[pos++];
                uint8_t szBytes = 1 << (h2 & 0x0F);
                if (pos + szBytes > fileSize) return nullptr;
                size = 0;
                for (int i = 0; i < szBytes; ++i) size = (size << 8) | data[pos++];
            }

            if (tag == 0) {
                if (size == 8) return false;
                if (size == 9) return true;
                return nullptr;
            } else if (tag == 1) { // int
                uint8_t szBytes = 1 << size;
                if (pos + szBytes > fileSize) return 0;
                uint64_t v = 0;
                for (int i = 0; i < szBytes; ++i) v = (v << 8) | data[pos++];
                return v;
            } else if (tag == 5) { // ASCII string
                if (pos + size > fileSize) return "";
                return std::string(reinterpret_cast<const char*>(data.data() + pos), static_cast<size_t>(size));
            } else if (tag == 6) { // UTF-16 BE string
                if (pos + size * 2 > fileSize) return "";
                std::wstring wstr;
                wstr.resize(static_cast<size_t>(size));
                for (size_t i = 0; i < size; ++i) {
                    wchar_t wc = static_cast<wchar_t>((data[pos] << 8) | data[pos + 1]);
                    pos += 2;
                    wstr[i] = wc;
                }
                int bytes = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
                if (bytes <= 0) return "";
                std::string res(bytes, '\0');
                WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), res.data(), bytes, nullptr, nullptr);
                return res;
            } else if (tag == 10) { // array
                nlohmann::json arr = nlohmann::json::array();
                if (pos + size * objectRefSize > fileSize) return arr;
                for (size_t i = 0; i < size; ++i) {
                    uint64_t ref = readRef(data.data() + pos);
                    pos += objectRefSize;
                    arr.push_back(self(self, ref));
                }
                return arr;
            } else if (tag == 13) { // dict
                nlohmann::json dict = nlohmann::json::object();
                if (pos + size * objectRefSize * 2 > fileSize) return dict;
                std::vector<uint64_t> keyRefs(size);
                for (size_t i = 0; i < size; ++i) {
                    keyRefs[i] = readRef(data.data() + pos);
                    pos += objectRefSize;
                }
                std::vector<uint64_t> valRefs(size);
                for (size_t i = 0; i < size; ++i) {
                    valRefs[i] = readRef(data.data() + pos);
                    pos += objectRefSize;
                }
                for (size_t i = 0; i < size; ++i) {
                    nlohmann::json keyObj = self(self, keyRefs[i]);
                    std::string keyStr = keyObj.is_string() ? keyObj.get<std::string>() : "";
                    if (!keyStr.empty()) {
                        dict[keyStr] = self(self, valRefs[i]);
                    }
                }
                return dict;
            }
            return nullptr;
        };

        try {
            nlohmann::json root = readObject(readObject, topObject);
            if (root.contains("items") && root["items"].is_array()) {
                for (const auto& it : root["items"]) {
                    if (!it.is_object()) continue;
                    LibraryItem item;
                    item.title = it.value("line1", "");
                    std::string line2 = it.value("line2", "");
                    // line2 format: "Artist — Album"
                    size_t sep = line2.find(" — ");
                    if (sep != std::string::npos) {
                        item.artist = line2.substr(0, sep);
                        item.album = line2.substr(sep + 5);
                    } else {
                        item.artist = line2;
                    }
                    auto trim = [](std::string& str) {
                        while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) str.erase(str.begin());
                        while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) str.pop_back();
                    };
                    trim(item.artist);
                    trim(item.album);
                    if (it.contains("desc") && it["desc"].contains("identifiers")) {
                        item.adamId = it["desc"]["identifiers"].value("storeAdamID", "");
                    }
                    if (!item.title.empty()) {
                        results.push_back(item);
                    }
                }
            }
        } catch (...) {}

        return results;
    }

    static std::vector<LibraryItem> ReadRecentItems() {
        std::lock_guard lock(g_playHistoryMutex);
        auto history = LoadHistoryFromFile();

        // Also check PlayQueueState.plist
        auto plistItems = ParsePlistItems();
        if (!plistItems.empty()) {
            for (const auto& pi : plistItems) {
                bool found = false;
                for (const auto& hi : history) {
                    if (hi.title == pi.title && hi.artist == pi.artist) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    history.push_back(pi);
                }
            }
            SaveHistoryToFile(history);
        }

        if (history.empty()) {
            history = GetDefaultSeedItems();
            SaveHistoryToFile(history);
        }

        return history;
    }

    static std::vector<AlbumItem> ReadRecentAlbums() {
        std::vector<AlbumItem> albums;
        std::unordered_map<std::string, size_t> indexMap;

        // Load am_library_albums.json first (user's authentic library albums)
        std::wstring albPath = L"C:\\Users\\Public\\AMExclusive\\am_library_albums.json";
        HANDLE hFile = CreateFileW(albPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(hFile, nullptr);
            if (size > 0 && size < 5 * 1024 * 1024) {
                std::string content(size, '\0');
                DWORD read = 0;
                ReadFile(hFile, content.data(), size, &read, nullptr);
                try {
                    auto j = nlohmann::json::parse(content);
                    if (j.is_array()) {
                        for (const auto& it : j) {
                            std::string name = it.value("name", "");
                            std::string artist = it.value("artist", "");
                            std::string id = it.value("id", "");
                            std::string artworkUrl = it.value("artworkUrl", "");
                            std::string releaseDate = it.value("releaseDate", "");
                            int trackCount = it.value("trackCount", 1);
                            if (name.empty()) continue;
                            std::string key = name + "|||" + artist;
                            if (indexMap.find(key) == indexMap.end()) {
                                AlbumItem alb;
                                alb.name = name;
                                alb.artist = artist;
                                alb.firstAdamId = id;
                                alb.albumId = id;
                                alb.artworkUrl = artworkUrl;
                                alb.releaseDate = releaseDate;
                                alb.trackCount = trackCount;
                                indexMap[key] = albums.size();
                                albums.push_back(alb);
                            }
                        }
                    }
                } catch (...) {}
            }
            CloseHandle(hFile);
        }

        auto items = ReadRecentItems();
        for (const auto& it : items) {
            if (it.album.empty()) continue;
            std::string key = it.album + "|||" + it.artist;
            auto found = indexMap.find(key);
            if (found == indexMap.end()) {
                AlbumItem alb;
                alb.name = it.album;
                alb.artist = it.artist;
                alb.firstAdamId = it.adamId;
                alb.albumId = it.adamId;
                alb.artworkUrl = it.artworkUrl;
                alb.trackCount = 1;
                indexMap[key] = albums.size();
                albums.push_back(alb);
            } else {
                if (albums[found->second].firstAdamId.empty() && !it.adamId.empty()) {
                    albums[found->second].firstAdamId = it.adamId;
                }
                if (albums[found->second].artworkUrl.empty() && !it.artworkUrl.empty()) {
                    albums[found->second].artworkUrl = it.artworkUrl;
                }
            }
        }

        return albums;
    }

    static std::vector<ArtistItem> ReadRecentArtists() {
        auto items = ReadRecentItems();
        std::vector<ArtistItem> artists;
        std::unordered_map<std::string, size_t> indexMap;

        for (const auto& it : items) {
            if (it.artist.empty()) continue;
            auto found = indexMap.find(it.artist);
            if (found == indexMap.end()) {
                ArtistItem art;
                art.name = it.artist;
                art.trackCount = 1;
                indexMap[it.artist] = artists.size();
                artists.push_back(art);
            } else {
                artists[found->second].trackCount++;
            }
        }
        return artists;
    }
};

} // namespace ammod::remote
