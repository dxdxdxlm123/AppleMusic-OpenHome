#pragma once

#include "MediaController.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace httplib {
class Server;
namespace ws {
class WebSocket;
}
}

namespace ammod::remote {

class RemoteServer {
public:
    explicit RemoteServer(int port = 10767);
    ~RemoteServer();

    bool Start();
    void Stop();

    int GetPort() const { return port_; }

private:
    void SetupRoutes();
    void OnTrackChanged(const TrackSnapshot& snapshot);
    void OnStateChanged(const TrackSnapshot& snapshot);
    void OnTimeChanged(const TrackSnapshot& snapshot);

    void BroadcastWebSocket(const std::string& message);
    std::string BuildNowPlayingJson(const TrackSnapshot& snapshot);
    std::string BuildPlaybackSnapshotJson(const TrackSnapshot& snapshot);

    int port_ = 10767;
    std::unique_ptr<httplib::Server> server_;
    MediaController controller_;
    std::string hostAddress_ = "127.0.0.1";

    std::mutex wsMutex_;
    std::unordered_set<httplib::ws::WebSocket*> activeSockets_;
    bool running_ = false;
};

} // namespace ammod::remote
