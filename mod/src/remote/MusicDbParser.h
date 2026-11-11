#pragma once

#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <cstdint>

extern "C" {
#include "tinf.h"
}

namespace ammod::remote {

struct MusicDbAlbum {
    std::string title;
    std::string artist;
    std::string artworkUrl;
    std::string catalogId;
    std::string firstSongId;
    std::string releaseDate;
    int trackCount{0};
};

struct MusicDbTrack {
    int64_t itemId{0};
    std::string title;
    std::string artist;
    std::string album;
    std::string artworkUrl;
    std::string songId;
};

class MusicDbParser {
public:
    static bool GetLibraryDir(std::wstring& outDir) {
        PWSTR pPath = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &pPath))) {
            return false;
        }
        outDir = std::wstring(pPath) + L"\\Apple Music\\Apple Music Library.musiclibrary";
        CoTaskMemFree(pPath);
        return true;
    }

    static bool Parse(std::vector<MusicDbAlbum>& outAlbums, std::vector<MusicDbTrack>& outTracks) {
        std::wstring libDir;
        if (!GetLibraryDir(libDir)) return false;

        std::wstring dbPath = libDir + L"\\Library.musicdb";
        std::wstring artDbPath = libDir + L"\\artwork.sqlite";

        if (!ParseMusicDbFile(dbPath, outAlbums, outTracks)) {
            return false;
        }

        // Offline artwork enrichment from artwork.sqlite via winsqlite3.dll
        std::unordered_map<int64_t, std::string> trackArts;
        QueryArtworkSqlite(artDbPath, trackArts);

        // Map track artwork and aggregate tracks to albums
        std::unordered_map<std::string, std::string> albumArtMap;
        std::unordered_map<std::string, int> albumTrackCount;
        std::unordered_map<std::string, std::string> albumFirstSong;

        for (auto& tr : outTracks) {
            auto it = trackArts.find(tr.itemId);
            if (it != trackArts.end()) {
                tr.artworkUrl = it->second;
                std::string key = tr.album + "|||" + tr.artist;
                if (albumArtMap.find(key) == albumArtMap.end()) albumArtMap[key] = it->second;
                if (albumArtMap.find(tr.album) == albumArtMap.end()) albumArtMap[tr.album] = it->second;
            }
            std::string key = tr.album + "|||" + tr.artist;
            albumTrackCount[key]++;
            albumTrackCount[tr.album]++;
        }

        for (auto& alb : outAlbums) {
            std::string key = alb.title + "|||" + alb.artist;
            auto itArt = albumArtMap.find(key);
            if (itArt == albumArtMap.end()) itArt = albumArtMap.find(alb.title);
            if (itArt != albumArtMap.end()) {
                alb.artworkUrl = itArt->second;
            }
            auto itCnt = albumTrackCount.find(key);
            if (itCnt == albumTrackCount.end()) itCnt = albumTrackCount.find(alb.title);
            if (itCnt != albumTrackCount.end()) {
                alb.trackCount = itCnt->second;
            }
        }

        return !outAlbums.empty();
    }

private:
    static bool ParseMusicDbFile(const std::wstring& dbPath, std::vector<MusicDbAlbum>& outAlbums, std::vector<MusicDbTrack>& outTracks) {
        HANDLE hFile = CreateFileW(dbPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize < 128) {
            CloseHandle(hFile);
            return false;
        }

        std::vector<uint8_t> data(fileSize);
        DWORD bytesRead = 0;
        ReadFile(hFile, data.data(), fileSize, &bytesRead, nullptr);
        CloseHandle(hFile);

        if (bytesRead != fileSize) return false;

        uint32_t envelopeLength = *reinterpret_cast<uint32_t*>(&data[4]);
        uint32_t maxCryptSize = *reinterpret_cast<uint32_t*>(&data[84]);
        if (envelopeLength >= fileSize) return false;

        uint32_t payloadLength = fileSize - envelopeLength;
        uint32_t cryptSize = (maxCryptSize > 0 && maxCryptSize < payloadLength) ? maxCryptSize : (payloadLength / 16) * 16;
        if (envelopeLength + cryptSize > fileSize) return false;

        // BCrypt AES-128 ECB decryption
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
            return false;
        }

        if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        DWORD keyObjSize = 0, cbResult = 0;
        BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&keyObjSize, sizeof(DWORD), &cbResult, 0);
        std::vector<uint8_t> keyObj(keyObjSize);

        const uint8_t aesKey[16] = { 'B','H','U','I','L','u','i','l','f','g','h','u','i','l','a','3' };
        BCRYPT_KEY_HANDLE hKey = nullptr;
        if (BCryptGenerateSymmetricKey(hAlg, &hKey, keyObj.data(), keyObjSize, (PUCHAR)aesKey, 16, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        std::vector<uint8_t> decrypted(cryptSize);
        ULONG decryptedSize = 0;
        NTSTATUS status = BCryptDecrypt(hKey, &data[envelopeLength], cryptSize, nullptr, nullptr, 0, decrypted.data(), cryptSize, &decryptedSize, 0);
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);

        if (status != 0) {
            return false;
        }

        std::vector<uint8_t> fullCompressed;
        fullCompressed.insert(fullCompressed.end(), decrypted.begin(), decrypted.end());
        if (envelopeLength + cryptSize < fileSize) {
            fullCompressed.insert(fullCompressed.end(), data.begin() + envelopeLength + cryptSize, data.end());
        }

        tinf_init();
        unsigned int destLen = 10 * 1024 * 1024; // 10MB decompression buffer
        std::vector<uint8_t> decomp(destLen);
        int res = tinf_zlib_uncompress(decomp.data(), &destLen, fullCompressed.data(), static_cast<unsigned int>(fullCompressed.size()));
        if (res != TINF_OK) {
            return false;
        }
        decomp.resize(destLen);

        // Parse decompressed buffer
        size_t pos = 0;
        std::vector<MusicDbAlbum> rawAlbums;
        MusicDbAlbum* curAlbum = nullptr;
        MusicDbTrack* curTrack = nullptr;

        while (pos + 16 < decomp.size()) {
            const char* sig = reinterpret_cast<const char*>(&decomp[pos]);
            if (memcmp(sig, "iama", 4) == 0) {
                rawAlbums.push_back({});
                curAlbum = &rawAlbums.back();
                curTrack = nullptr;
                pos += 24;
                continue;
            } else if (memcmp(sig, "itma", 4) == 0) {
                outTracks.push_back({});
                curTrack = &outTracks.back();
                if (pos + 24 <= decomp.size()) {
                    curTrack->itemId = *reinterpret_cast<const int64_t*>(&decomp[pos + 16]);
                }
                curAlbum = nullptr;
                pos += 24;
                continue;
            } else if (memcmp(sig, "boma", 4) == 0) {
                uint32_t secSize = *reinterpret_cast<const uint32_t*>(&decomp[pos + 8]);
                uint32_t subType = *reinterpret_cast<const uint32_t*>(&decomp[pos + 12]);
                if (pos + secSize <= decomp.size() && secSize >= 36) {
                    uint32_t strLen = *reinterpret_cast<const uint32_t*>(&decomp[pos + 24]);
                    if (strLen > 0 && pos + 36 + strLen <= decomp.size()) {
                        const wchar_t* wstr = reinterpret_cast<const wchar_t*>(&decomp[pos + 36]);
                        int chars = strLen / 2;
                        while (chars > 0 && wstr[chars - 1] == L'\0') --chars;
                        int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr, chars, nullptr, 0, nullptr, nullptr);
                        if (u8len > 0) {
                            std::string val(u8len, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, wstr, chars, val.data(), u8len, nullptr, nullptr);
                            if (curAlbum) {
                                if (subType == 0x012C || (subType == 0x0003 && curAlbum->title.empty())) curAlbum->title = val;
                                else if (subType == 0x012D || subType == 0x012E || ((subType == 0x0004 || subType == 0x001B) && curAlbum->artist.empty())) curAlbum->artist = val;
                            } else if (curTrack) {
                                if (subType == 0x0002) curTrack->title = val;
                                else if (subType == 0x0003) curTrack->album = val;
                                else if (subType == 0x0004) curTrack->artist = val;
                            }
                        }
                    }
                }
                pos += (secSize > 16) ? secSize : 16;
                continue;
            } else if (memcmp(sig, "lama", 4) == 0 || memcmp(sig, "lAma", 4) == 0 ||
                       memcmp(sig, "ltma", 4) == 0 || memcmp(sig, "lPma", 4) == 0 ||
                       memcmp(sig, "lpma", 4) == 0) {
                curAlbum = nullptr;
                curTrack = nullptr;
                pos += 4;
                continue;
            }
            pos += 1;
        }

        // Deduplicate albums
        std::unordered_set<std::string> seen;
        int albIdx = 1;
        for (auto& a : rawAlbums) {
            if (a.title.empty()) continue;
            std::string key = a.title + "|||" + a.artist;
            if (seen.insert(key).second) {
                a.catalogId = "lib.alb." + std::to_string(albIdx++);
                outAlbums.push_back(a);
            }
        }

        return !outAlbums.empty();
    }

    typedef struct sqlite3 sqlite3;
    typedef struct sqlite3_stmt sqlite3_stmt;
    typedef int (*pfn_sqlite3_open_v2)(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
    typedef int (*pfn_sqlite3_prepare_v2)(sqlite3 *db, const char *zSql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail);
    typedef int (*pfn_sqlite3_step)(sqlite3_stmt*);
    typedef const unsigned char *(*pfn_sqlite3_column_text)(sqlite3_stmt*, int iCol);
    typedef int64_t (*pfn_sqlite3_column_int64)(sqlite3_stmt*, int iCol);
    typedef int (*pfn_sqlite3_column_int)(sqlite3_stmt*, int iCol);
    typedef int (*pfn_sqlite3_finalize)(sqlite3_stmt *pStmt);
    typedef int (*pfn_sqlite3_close)(sqlite3*);

    static bool QueryArtworkSqlite(const std::wstring& dbPath, std::unordered_map<int64_t, std::string>& outArtworks) {
        HMODULE hSql = LoadLibraryW(L"winsqlite3.dll");
        if (!hSql) return false;

        auto fn_open = (pfn_sqlite3_open_v2)GetProcAddress(hSql, "sqlite3_open_v2");
        auto fn_prep = (pfn_sqlite3_prepare_v2)GetProcAddress(hSql, "sqlite3_prepare_v2");
        auto fn_step = (pfn_sqlite3_step)GetProcAddress(hSql, "sqlite3_step");
        auto fn_col_text = (pfn_sqlite3_column_text)GetProcAddress(hSql, "sqlite3_column_text");
        auto fn_col_i64 = (pfn_sqlite3_column_int64)GetProcAddress(hSql, "sqlite3_column_int64");
        auto fn_col_int = (pfn_sqlite3_column_int)GetProcAddress(hSql, "sqlite3_column_int");
        auto fn_finalize = (pfn_sqlite3_finalize)GetProcAddress(hSql, "sqlite3_finalize");
        auto fn_close = (pfn_sqlite3_close)GetProcAddress(hSql, "sqlite3_close");

        if (!fn_open || !fn_prep || !fn_step || !fn_col_text || !fn_col_i64 || !fn_finalize || !fn_close) {
            FreeLibrary(hSql);
            return false;
        }

        int u8len = WideCharToMultiByte(CP_UTF8, 0, dbPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string u8path(u8len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, dbPath.c_str(), -1, u8path.data(), u8len, nullptr, nullptr);

        sqlite3* db = nullptr;
        // SQLITE_OPEN_READONLY = 0x00000001
        if (fn_open(u8path.c_str(), &db, 0x00000001, nullptr) != 0 || !db) {
            FreeLibrary(hSql);
            return false;
        }

        const char* sql = "SELECT item_to_artwork.item_id, artwork_source.location, artwork_source.location_type "
                          "FROM item_to_artwork JOIN artwork_source "
                          "ON item_to_artwork.artwork_id = artwork_source.artwork_id "
                          "WHERE artwork_source.location_type IN (2, 4);";

        sqlite3_stmt* stmt = nullptr;
        if (fn_prep(db, sql, -1, &stmt, nullptr) == 0) {
            // SQLITE_ROW = 100
            while (fn_step(stmt) == 100) {
                int64_t itemId = fn_col_i64(stmt, 0);
                const char* locText = (const char*)fn_col_text(stmt, 1);
                int locType = fn_col_int(stmt, 2);
                if (locText) {
                    std::string loc(locText);
                    std::string artUrl;
                    if (locType == 4) {
                        artUrl = "https://is1-ssl.mzstatic.com/image/thumb/" + loc + "/{w}x{h}bb.jpg";
                    } else if (locType == 2) {
                        artUrl = loc;
                    }
                    if (!artUrl.empty()) {
                        outArtworks[itemId] = artUrl;
                    }
                }
            }
            fn_finalize(stmt);
        }

        fn_close(db);
        FreeLibrary(hSql);
        return true;
    }
};

} // namespace ammod::remote
