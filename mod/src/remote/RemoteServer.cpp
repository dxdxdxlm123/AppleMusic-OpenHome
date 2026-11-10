#include "RemoteServer.h"

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

json FetchAppleSearch(const std::string& term, int limit) {
    json emptyResults = {
        {"data", {
            {"results", {
                {"songs", {{"data", json::array()}}}
            }}
        }}
    };

    if (term.empty()) return emptyResults;

    HINTERNET hSession = WinHttpOpen(L"AMExclusiveRemote/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return emptyResults;

    WinHttpSetTimeouts(hSession, 3000, 3000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"itunes.apple.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return emptyResults;
    }

    std::string path = "/search?term=" + UrlEncode(term) + "&entity=song&limit=" + std::to_string(limit);
    std::wstring wPath = Widen(path);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return emptyResults;
    }

    bool sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr);

    std::string responseBody;
    if (sent) {
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
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (responseBody.empty()) return emptyResults;

    try {
        auto itunesJson = json::parse(responseBody);
        json songsData = json::array();

        if (itunesJson.contains("results") && itunesJson["results"].is_array()) {
            for (const auto& item : itunesJson["results"]) {
                std::string trackId = std::to_string(item.value("trackId", 0ll));
                std::string trackName = item.value("trackName", "");
                std::string artistName = item.value("artistName", "");
                std::string albumName = item.value("collectionName", "");
                std::string artworkUrl = item.value("artworkUrl100", "");
                int64_t durationMs = item.value("trackTimeMillis", 0ll);
                std::string trackViewUrl = item.value("trackViewUrl", "");

                // Convert 100x100 to template {w}x{h}bb.{f}
                size_t pos = artworkUrl.find("100x100bb.");
                if (pos != std::string::npos) {
                    artworkUrl.replace(pos, 10, "{w}x{h}bb.");
                }

                json songItem = {
                    {"id", trackId},
                    {"type", "songs"},
                    {"attributes", {
                        {"name", trackName},
                        {"artistName", artistName},
                        {"albumName", albumName},
                        {"durationInMillis", durationMs},
                        {"url", trackViewUrl},
                        {"artwork", {
                            {"url", artworkUrl}
                        }},
                        {"playParams", {
                            {"id", trackId},
                            {"kind", "song"}
                        }}
                    }}
                };
                songsData.push_back(songItem);
            }
        }

        return {
            {"data", {
                {"results", {
                    {"songs", {
                        {"data", songsData}
                    }}
                }}
            }}
        };
    } catch (...) {
        return emptyResults;
    }
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
    // CORS & Options handler
    server_->Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "*");
        res.status = 204;
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

    // Audio Quality (Hi-Res badge)
    server_->Get("/api/v2/playback/audio-quality", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        json j = {
            {"data", {
                {"flavor", "alac"},
                {"flavorLabel", "Hi-Res Lossless · WASAPI Exclusive Bit-Perfect"}
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
            if (j.contains("id") && j["id"].is_string()) {
                std::string id = j["id"].get<std::string>();
                controller_.PlayTrackId(id);
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
                controller_.PlayUrl(url);
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
                controller_.PlayAlbumId(id);
            }
        } catch (...) {}
        res.set_content("{\"data\":{}}", "application/json");
    });

    // Queue & Volume placeholders
    server_->Get("/api/v2/queue", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"data\":{\"items\":[],\"position\":0},\"meta\":{\"total\":0}}", "application/json");
    });

    server_->Get("/api/v2/audio/volume", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"data\":{\"volume\":1.0}}", "application/json");
    });

    // Search catalog
    server_->Get("/api/v2/amapi/search", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string term = req.get_param_value("term");
        std::string limitStr = req.get_param_value("limit");
        int limit = 15;
        if (!limitStr.empty()) {
            try { limit = std::stoi(limitStr); } catch (...) {}
        }
        json searchResult = FetchAppleSearch(term, limit);
        res.set_content(searchResult.dump(), "application/json");
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
