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
#include <nlohmann/json.hpp>

namespace ammod::remote {

struct LibraryItem {
    std::string title;
    std::string artist;
    std::string album;
    std::string adamId;
    std::string date;
};

struct AlbumItem {
    std::string name;
    std::string artist;
    std::string firstAdamId;
    int trackCount{0};
};

struct ArtistItem {
    std::string name;
    int trackCount{0};
};

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

    static std::vector<LibraryItem> ReadRecentItems() {
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

    static std::vector<AlbumItem> ReadRecentAlbums() {
        auto items = ReadRecentItems();
        std::vector<AlbumItem> albums;
        std::unordered_map<std::string, size_t> indexMap;

        for (const auto& it : items) {
            if (it.album.empty()) continue;
            std::string key = it.album + "|||" + it.artist;
            auto found = indexMap.find(key);
            if (found == indexMap.end()) {
                AlbumItem alb;
                alb.name = it.album;
                alb.artist = it.artist;
                alb.firstAdamId = it.adamId;
                alb.trackCount = 1;
                indexMap[key] = albums.size();
                albums.push_back(alb);
            } else {
                albums[found->second].trackCount++;
                if (albums[found->second].firstAdamId.empty() && !it.adamId.empty()) {
                    albums[found->second].firstAdamId = it.adamId;
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
