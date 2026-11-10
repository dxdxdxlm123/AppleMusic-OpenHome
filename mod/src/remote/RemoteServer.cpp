#include "RemoteServer.h"
#include "AmPluginClient.h"
#include "WebRemoteHtml.h"

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

struct CachedTrackInfo {
    std::string name;
    std::string url;
};

std::mutex g_trackCacheMutex;
std::unordered_map<std::string, CachedTrackInfo> g_trackCache;

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

CachedTrackInfo ResolveTrackInfo(const std::string& trackId) {
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
            CachedTrackInfo info{ name, url };
            std::lock_guard lock(g_trackCacheMutex);
            g_trackCache[trackId] = info;
            return info;
        }
    }
    return {};
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
        server_->listen("0.0.0.0", port_);
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
            if (!id.empty()) {
                std::cout << "[REMOTE] Play Item ID: " << id << std::endl;
                auto info = ResolveTrackInfo(id);
                std::cout << "[REMOTE] Resolved Track: " << info.name << ", URL: " << info.url << std::endl;
                if (!info.url.empty()) {
                    controller_.PlayTrack(info.url, Widen(info.name));
                } else {
                    controller_.PlayTrackId(id);
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

    // Apple Music API raw proxy
    server_->Get(R"(/api/v2/amapi/raw/(.*))", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string path = req.matches[1];
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

    // Audio Output Devices (WASAPI DACs, ASIO drivers, DLNA renderers)
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
                {"location", d.location}
            });
        }
        json j = {{"devices", arr}};
        res.set_content(j.dump(), "application/json");
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

    // Select Active Audio Output Target (DAC / ASIO / DLNA)
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
