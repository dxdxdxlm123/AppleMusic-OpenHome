#include "RaatProtocol.h"

#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ammod::raat {
namespace {

std::uint32_t ReadBe32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24u) |
           (static_cast<std::uint32_t>(data[1]) << 16u) |
           (static_cast<std::uint32_t>(data[2]) << 8u) |
           static_cast<std::uint32_t>(data[3]);
}

void WriteBe32(std::uint8_t* data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 24u);
    data[1] = static_cast<std::uint8_t>(value >> 16u);
    data[2] = static_cast<std::uint8_t>(value >> 8u);
    data[3] = static_cast<std::uint8_t>(value);
}

bool WaitWritable(SOCKET socket, std::uint32_t timeoutMs) noexcept {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(socket, &writeSet);
    fd_set errSet;
    FD_ZERO(&errSet);
    FD_SET(socket, &errSet);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000u);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000u) * 1000u);
    const int r = select(0, nullptr, &writeSet, &errSet, &tv);
    if (r <= 0) return false;
    if (FD_ISSET(socket, &errSet)) return false;
    return FD_ISSET(socket, &writeSet) != 0;
}

bool WaitReadable(SOCKET socket, std::uint32_t timeoutMs) noexcept {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    fd_set errSet;
    FD_ZERO(&errSet);
    FD_SET(socket, &errSet);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000u);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000u) * 1000u);
    const int r = select(0, &readSet, nullptr, &errSet, &tv);
    if (r <= 0) return false;
    if (FD_ISSET(socket, &errSet)) return false;
    return FD_ISSET(socket, &readSet) != 0;
}

std::string ValueAfterKey(const std::string& json, const std::string& key,
                          std::size_t& valueStart) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != ':') return {};
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    valueStart = pos;
    return json.substr(pos);
}

std::string RequestNameOf(const std::string& json) noexcept {
    const auto key = json.find("\"request\"");
    if (key == std::string::npos) return {};
    auto colon = json.find(':', key);
    if (colon == std::string::npos) return {};
    auto quote = json.find('"', colon);
    if (quote == std::string::npos) return {};
    const auto end = json.find('"', quote + 1);
    if (end == std::string::npos) return {};
    return json.substr(quote + 1, end - quote - 1);
}

} // namespace

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        out.data(), length);
    return out;
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + value.size() / 8u + 8u);
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (ch < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    return out;
}

bool JsonFindString(const std::string& json, const std::string& key,
                    std::string& out) {
    std::size_t start = 0;
    const auto rest = ValueAfterKey(json, key, start);
    if (rest.empty() || rest[0] != '"') return false;
    out.clear();
    for (std::size_t i = 1; i < rest.size(); ++i) {
        const char ch = rest[i];
        if (ch == '"') return true;
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (i + 1 >= rest.size()) return false;
        const char escape = rest[++i];
        switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            if (i + 4 >= rest.size()) return false;
            unsigned codepoint = 0;
            for (int digit = 0; digit < 4; ++digit) {
                const char hex = rest[++i];
                codepoint <<= 4u;
                if (hex >= '0' && hex <= '9') codepoint += static_cast<unsigned>(hex - '0');
                else if (hex >= 'a' && hex <= 'f') codepoint += static_cast<unsigned>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') codepoint += static_cast<unsigned>(hex - 'A' + 10);
                else return false;
            }
            if (codepoint < 0x80) {
                out.push_back(static_cast<char>(codepoint));
            } else if (codepoint < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (codepoint >> 6u)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3Fu)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (codepoint >> 12u)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6u) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3Fu)));
            }
            break;
        }
        default: out.push_back(escape); break;
        }
    }
    return false;
}

std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string out;
    return JsonFindString(json, key, out) ? out : std::string{};
}

bool JsonFindInt(const std::string& json, const std::string& key,
                 std::int64_t& out) {
    std::size_t start = 0;
    const auto rest = ValueAfterKey(json, key, start);
    if (rest.empty()) return false;
    std::size_t pos = 0;
    while (pos < rest.size() && (rest[pos] == ' ' || rest[pos] == '\t')) ++pos;
    bool negative = false;
    if (pos < rest.size() && rest[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= rest.size() || rest[pos] < '0' || rest[pos] > '9') return false;
    std::uint64_t value = 0;
    constexpr std::uint64_t kMaxAbs = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    while (pos < rest.size() && rest[pos] >= '0' && rest[pos] <= '9') {
        const std::uint64_t digit = static_cast<std::uint64_t>(rest[pos] - '0');
        if (value > (kMaxAbs - digit) / 10ull) {
            value = kMaxAbs;
            break;
        }
        value = value * 10ull + digit;
        ++pos;
    }
    out = negative ? -static_cast<std::int64_t>(value)
                   : static_cast<std::int64_t>(value);
    return true;
}

std::int64_t JsonGetInt(const std::string& json, const std::string& key,
                        std::int64_t fallback) {
    std::int64_t out = fallback;
    return JsonFindInt(json, key, out) ? out : fallback;
}

bool JsonFindDouble(const std::string& json, const std::string& key,
                    double& out) {
    std::size_t start = 0;
    const auto rest = ValueAfterKey(json, key, start);
    if (rest.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(rest.c_str(), &end);
    if (end == rest.c_str()) return false;
    out = value;
    return true;
}

double JsonGetDouble(const std::string& json, const std::string& key,
                     double fallback) {
    double out = fallback;
    return JsonFindDouble(json, key, out) ? out : fallback;
}

bool JsonFindBool(const std::string& json, const std::string& key,
                  bool& out) {
    std::size_t start = 0;
    const auto rest = ValueAfterKey(json, key, start);
    if (rest.empty()) return false;
    if (rest.compare(0, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (rest.compare(0, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool JsonGetBool(const std::string& json, const std::string& key,
                 bool fallback) {
    bool out = fallback;
    return JsonFindBool(json, key, out) ? out : fallback;
}

std::uint64_t NowMicroseconds() noexcept {
    using Clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
}

std::uint64_t NowNanoseconds() noexcept {
    using Clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

bool EnsureWinsock() noexcept {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static bool initialized = false;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID parameter, PVOID*) -> BOOL {
        WSADATA data{};
        *static_cast<bool*>(parameter) = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        return TRUE;
    }, &initialized, nullptr);
    return initialized;
}

std::wstring NewUuidString() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        wchar_t fallback[64]{};
        std::swprintf(fallback, std::size(fallback), L"%08lx-%08lx",
                      static_cast<unsigned long>(GetTickCount64()),
                      static_cast<unsigned long>(GetCurrentProcessId()));
        return fallback;
    }
    const unsigned long long node =
        (static_cast<unsigned long long>(guid.Data4[2]) << 40u) |
        (static_cast<unsigned long long>(guid.Data4[3]) << 32u) |
        (static_cast<unsigned long long>(guid.Data4[4]) << 24u) |
        (static_cast<unsigned long long>(guid.Data4[5]) << 16u) |
        (static_cast<unsigned long long>(guid.Data4[6]) << 8u) |
        static_cast<unsigned long long>(guid.Data4[7]);
    wchar_t text[40]{};
    std::swprintf(text, std::size(text),
                  L"%08lX-%04X-%04X-%04X-%012llX",
                  static_cast<unsigned long>(guid.Data1),
                  static_cast<unsigned>(guid.Data2),
                  static_cast<unsigned>(guid.Data3),
                  static_cast<unsigned>(guid.Data4[0]) * 256u +
                      static_cast<unsigned>(guid.Data4[1]),
                  node);
    return text;
}

TcpConnection::~TcpConnection() { Close(); }

std::wstring TcpConnection::LocalAddress() const noexcept {
    if (socket_ == INVALID_SOCKET) return {};
    sockaddr_in local{};
    int length = sizeof(local);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
        return {};
    }
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) == nullptr) return {};
    return ToWide(text);
}

void TcpConnection::Close() noexcept {
    if (socket_ != INVALID_SOCKET) {
        (void)::shutdown(socket_, SD_BOTH);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

bool TcpConnection::Connect(const std::wstring& address, std::uint16_t port,
                            std::uint32_t timeoutMs, std::wstring& error,
                            const std::wstring& bindAddress) noexcept {
    Close();
    lastWsaError_ = 0;
    if (!EnsureWinsock()) {
        error = L"WSAStartup failed";
        return false;
    }
    const std::string text = ToUtf8(address);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (inet_pton(AF_INET, text.c_str(), &endpoint.sin_addr) != 1) {
        error = L"invalid IPv4 address " + address;
        return false;
    }
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        lastWsaError_ = WSAGetLastError();
        error = L"socket failed " + std::to_wstring(lastWsaError_);
        return false;
    }

    if (!bindAddress.empty()) {
        const std::string bindUtf8 = ToUtf8(bindAddress);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        if (inet_pton(AF_INET, bindUtf8.c_str(), &local.sin_addr) == 1) {
            if (::bind(socket_, reinterpret_cast<const sockaddr*>(&local),
                       sizeof(local)) == SOCKET_ERROR) {
                lastWsaError_ = WSAGetLastError();
                error = L"bind " + bindAddress + L" failed " +
                        std::to_wstring(lastWsaError_);
                Close();
                return false;
            }
        }
    }

    u_long nonBlocking = 1;
    (void)ioctlsocket(socket_, FIONBIO, &nonBlocking);
    const int connected = ::connect(socket_,
                                    reinterpret_cast<const sockaddr*>(&endpoint),
                                    sizeof(endpoint));
    if (connected == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        if (code != WSAEWOULDBLOCK) {
            lastWsaError_ = code;
            error = L"connect failed " + std::to_wstring(code);
            Close();
            return false;
        }
        if (!WaitWritable(socket_, timeoutMs)) {
            lastWsaError_ = WSAETIMEDOUT;
            error = L"connect timed out";
            Close();
            return false;
        }
        int socketError = 0;
        int length = sizeof(socketError);
        if (getsockopt(socket_, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socketError), &length) != 0 ||
            socketError != 0) {
            lastWsaError_ = socketError;
            error = L"connect failed " + std::to_wstring(socketError);
            Close();
            return false;
        }
    }
    nonBlocking = 0;
    (void)ioctlsocket(socket_, FIONBIO, &nonBlocking);
    BOOL noDelay = TRUE;
    (void)::setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    int sndbuf = 262144; // 256 KB send buffer
    (void)::setsockopt(socket_, SOL_SOCKET, SO_SNDBUF,
                       reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
    int dscp = 0xB8; // DSCP EF (46) << 2
    (void)::setsockopt(socket_, IPPROTO_IP, IP_TOS,
                       reinterpret_cast<const char*>(&dscp), sizeof(dscp));
    return true;
}

bool TcpConnection::SendAll(const void* data, std::size_t size,
                            std::uint32_t timeoutMs) noexcept {
    if (socket_ == INVALID_SOCKET) {
        lastWsaError_ = WSAENOTCONN;
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (sent < size) {
        if (!WaitWritable(socket_, 50)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                lastWsaError_ = WSAETIMEDOUT;
                return false;
            }
            continue;
        }
        const int chunk = send(socket_, reinterpret_cast<const char*>(bytes + sent),
                               static_cast<int>(std::min<std::size_t>(
                                   size - sent, 65536)), 0);
        if (chunk <= 0) {
            const int code = WSAGetLastError();
            if (code == WSAEWOULDBLOCK) continue;
            lastWsaError_ = code;
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    lastWsaError_ = 0;
    return true;
}

bool TcpConnection::ReadSome(void* data, std::size_t capacity,
                             std::size_t& received, std::uint32_t timeoutMs) noexcept {
    received = 0;
    if (socket_ == INVALID_SOCKET) {
        lastWsaError_ = WSAENOTCONN;
        return false;
    }
    if (!WaitReadable(socket_, timeoutMs)) return false;
    const int chunk = recv(socket_, static_cast<char*>(data),
                           static_cast<int>(capacity), 0);
    if (chunk <= 0) {
        lastWsaError_ = chunk == 0 ? 0 : WSAGetLastError();
        Close();
        return false;
    }
    received = static_cast<std::size_t>(chunk);
    return true;
}

bool IsInternalRequest(const std::string& json) noexcept {
    static const char* const kInternal[] = {
        "load_script", "get_log", "set_log_level", "hi", "force_exit",
    };
    const std::string name = RequestNameOf(json);
    if (name.empty()) return false;
    for (const char* candidate : kInternal) {
        if (name == candidate) return true;
    }
    return false;
}

ControlChannel::~ControlChannel() { Close(); }

void ControlChannel::SetEventCallback(EventCallback callback) noexcept {
    std::lock_guard lock(stateMutex_);
    eventCallback_ = std::move(callback);
}

bool ControlChannel::Connect(const std::wstring& address, std::uint16_t port,
                             std::uint32_t timeoutMs, std::wstring& error,
                             const std::wstring& bindAddress) noexcept {
    Close();
    nextRid_.store(1, std::memory_order_release);
    lastRid_.store(0, std::memory_order_release);

    if (!connection_.Connect(address, port, timeoutMs, error, bindAddress)) {
        return false;
    }

    {
        std::lock_guard lock(stateMutex_);
        queues_.clear();
        disconnectReason_.clear();
    }

    lastInboundMs_.store(GetTickCount64(), std::memory_order_release);
    stopThreads_.store(false, std::memory_order_release);
    open_.store(true, std::memory_order_release);

    // Official Roon LL_BEGIN_KEEPALIVE (0x80000004) negotiating 2000ms interval
    // and 10000ms inactivity timeout.
    std::wstring helloError;
    if (!SendHello(helloError)) {
        Close();
        error = helloError;
        return false;
    }

    try {
        readerThread_ = std::thread(&ControlChannel::ReaderMain, this);
        keepaliveThread_ = std::thread(&ControlChannel::KeepaliveMain, this);
    } catch (...) {
        Close();
        error = L"control worker thread start failed";
        return false;
    }
    return true;
}

bool ControlChannel::SendHello(std::wstring& error) noexcept {
    std::uint8_t frame[16]{};
    WriteBe32(frame, 16u);
    WriteBe32(frame + 4, kLlTypeBeginKeepalive);
    WriteBe32(frame + 8, kLlDefaultKeepaliveMs);
    WriteBe32(frame + 12, kLlDefaultKeepaliveTimeoutMs);
    std::lock_guard lock(sendMutex_);
    if (!connection_.SendAll(frame, sizeof(frame), 1500u)) {
        error = L"hello send failed";
        return false;
    }
    return true;
}

bool ControlChannel::SendKeepaliveFrame() noexcept {
    // Official Roon LL_KEEPALIVE (0x80000003): 8-byte header, 0-byte payload,
    // handled directly by the endpoint's native C layer without touching Lua.
    std::uint8_t frame[8]{};
    WriteBe32(frame, 8u);
    WriteBe32(frame + 4, kLlTypeKeepalive);
    std::lock_guard lock(sendMutex_);
    return connection_.SendAll(frame, sizeof(frame), 1500u);
}

void ControlChannel::MarkDisconnectedLocked(const std::wstring& reason) noexcept {
    if (disconnectReason_.empty()) disconnectReason_ = reason;
    open_.store(false, std::memory_order_release);
    frameCv_.notify_all();
}

void ControlChannel::ReaderMain() noexcept {
    std::vector<std::uint8_t> pending;
    pending.reserve(32768);
    std::uint8_t buffer[8192]{};

    while (!stopThreads_.load(std::memory_order_acquire)) {
        std::size_t received = 0;
        if (!connection_.ReadSome(buffer, sizeof(buffer), received, 100u)) {
            if (!connection_.IsOpen()) {
                std::lock_guard lock(stateMutex_);
                MarkDisconnectedLocked(L"control connection closed");
                break;
            }
            continue;
        }
        lastInboundMs_.store(GetTickCount64(), std::memory_order_release);
        pending.insert(pending.end(), buffer, buffer + received);

        while (pending.size() >= 8) {
            const std::uint32_t total = ReadBe32(pending.data());
            // Gracefully slide past any short non-LL hello ack bytes (such as
            // the 6 zero bytes emitted by VEGA G2.2 right after 0x80000004)
            // instead of aborting the connection with "bad control frame length 0".
            if (total < 8u || total > kLlMaxMessageLen) {
                pending.erase(pending.begin());
                continue;
            }
            if (pending.size() < total) break;

            const std::uint32_t type = ReadBe32(pending.data() + 4);
            const std::uint32_t plainType = type & ~kLlTypeFlag;

            if (type == kLlTypeKeepalive || plainType == 3u ||
                type == kLlTypeBeginKeepalive || type == kLlTypeEndKeepalive) {
                pending.erase(pending.begin(), pending.begin() + total);
                continue;
            }

            if (plainType != 2u || total < 13u) {
                pending.erase(pending.begin(), pending.begin() + total);
                continue;
            }

            const std::uint32_t rid = ReadBe32(pending.data() + 8);
            const bool isFinal = pending[12] != 0;
            std::string json(reinterpret_cast<const char*>(pending.data()) + 13,
                             total - 13u);
            pending.erase(pending.begin(), pending.begin() + total);

            EventCallback cb;
            {
                std::lock_guard lock(stateMutex_);
                auto& q = queues_[rid];
                if (q.size() >= 64u) q.pop_front();
                q.push_back(QueuedFrame{json, isFinal});
                cb = eventCallback_;
            }
            frameCv_.notify_all();
            if (cb) {
                cb(rid, json, isFinal);
            }
        }
    }
}

void ControlChannel::KeepaliveMain() noexcept {
    while (!stopThreads_.load(std::memory_order_acquire) &&
           open_.load(std::memory_order_acquire)) {
        for (int i = 0; i < 20 && !stopThreads_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stopThreads_.load(std::memory_order_acquire) ||
            !open_.load(std::memory_order_acquire)) {
            break;
        }

        // Official Roon watchdog: if > 10000ms with no inbound bytes, kill connection.
        const std::uint64_t nowMs = GetTickCount64();
        const std::uint64_t lastMs = lastInboundMs_.load(std::memory_order_acquire);
        if (nowMs > lastMs && (nowMs - lastMs) > kLlDefaultKeepaliveTimeoutMs) {
            {
                std::lock_guard lock(stateMutex_);
                MarkDisconnectedLocked(L"no data received for >10000ms");
            }
            connection_.Close();
            break;
        }

        if (!SendKeepaliveFrame()) {
            {
                std::lock_guard lock(stateMutex_);
                MarkDisconnectedLocked(L"keepalive send failed");
            }
            connection_.Close();
            break;
        }
    }
}

void ControlChannel::Close() noexcept {
    stopThreads_.store(true, std::memory_order_release);
    open_.store(false, std::memory_order_release);
    connection_.Close();
    frameCv_.notify_all();
    if (keepaliveThread_.joinable()) keepaliveThread_.join();
    if (readerThread_.joinable()) readerThread_.join();
    std::lock_guard lock(stateMutex_);
    queues_.clear();
}

bool ControlChannel::SendFrame(std::uint32_t rid, const std::string& json,
                               std::uint32_t timeoutMs) noexcept {
    const std::uint32_t total = static_cast<std::uint32_t>(8u + 4u + json.size());
    std::vector<std::uint8_t> frame(total);
    WriteBe32(frame.data(), total);
    WriteBe32(frame.data() + 4, IsInternalRequest(json) ? kLlTypeRequestInternal
                                                        : kLlTypeRequest);
    WriteBe32(frame.data() + 8, rid);
    if (!json.empty()) {
        std::memcpy(frame.data() + 12, json.data(), json.size());
    }
    std::lock_guard lock(sendMutex_);
    return connection_.SendAll(frame.data(), frame.size(), timeoutMs);
}

bool ControlChannel::SendArtwork(const void* data, std::size_t size,
                                 std::uint32_t timeoutMs) noexcept {
    if (!IsOpen() || !data || size == 0 || size > kLlMaxMessageLen - 8u) {
        return false;
    }
    const std::uint32_t total = static_cast<std::uint32_t>(8u + size);
    std::vector<std::uint8_t> frame(total);
    WriteBe32(frame.data(), total);
    WriteBe32(frame.data() + 4, kLlTypeArtwork);
    std::memcpy(frame.data() + 8, data, size);
    std::lock_guard lock(sendMutex_);
    return connection_.SendAll(frame.data(), frame.size(), timeoutMs);
}

bool ControlChannel::SendAsync(const std::string& json, std::uint32_t timeoutMs,
                               std::uint32_t& outRid) noexcept {
    outRid = 0;
    if (!IsOpen()) return false;
    const std::uint32_t rid = nextRid_.fetch_add(1, std::memory_order_acq_rel);
    lastRid_.store(rid, std::memory_order_release);
    outRid = rid;
    return SendFrame(rid, json, timeoutMs);
}

bool ControlChannel::SendProbe(const std::string& json,
                               std::uint32_t timeoutMs) noexcept {
    std::uint32_t rid = 0;
    return SendAsync(json, timeoutMs, rid);
}

bool ControlChannel::WaitForReady(std::uint32_t timeoutMs,
                                  std::wstring& error) noexcept {
    (void)timeoutMs;
    error.clear();
    return IsOpen();
}

bool ControlChannel::ReadFrame(std::uint32_t expectedRid, std::uint32_t timeoutMs,
                               std::string& response, bool& final,
                               std::wstring& error) noexcept {
    response.clear();
    final = false;
    error.clear();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    std::unique_lock lock(stateMutex_);
    for (;;) {
        const auto it = queues_.find(expectedRid);
        if (it != queues_.end() && !it->second.empty()) {
            QueuedFrame frame = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty() && frame.isFinal) {
                queues_.erase(it);
            }
            response = std::move(frame.json);
            final = frame.isFinal;
            return true;
        }
        if (!open_.load(std::memory_order_acquire)) {
            error = disconnectReason_.empty() ? L"control connection closed"
                                              : disconnectReason_;
            return false;
        }
        if (frameCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            const auto retryIt = queues_.find(expectedRid);
            if (retryIt != queues_.end() && !retryIt->second.empty()) {
                continue;
            }
            error = L"control response timed out";
            return false;
        }
    }
}

bool ControlChannel::Request(const std::string& json, std::uint32_t timeoutMs,
                             std::string& response, bool& final,
                             std::wstring& error) noexcept {
    response.clear();
    final = false;
    error.clear();
    if (!IsOpen()) {
        error = L"control connection not open";
        return false;
    }
    const std::uint32_t rid = nextRid_.fetch_add(1, std::memory_order_acq_rel);
    lastRid_.store(rid, std::memory_order_release);
    if (!SendFrame(rid, json, timeoutMs)) {
        error = L"control request send failed";
        return false;
    }
    // Never silently reconnect TCP inside Request(): a fresh TCP socket has no
    // injected Lua dispatcher, so resending a business RPC would only return
    // NotImplemented. Propagate the failure so RaatSink rebuilds cleanly.
    return ReadFrame(rid, timeoutMs, response, final, error);
}

namespace {

struct WakeDeviceEntry final {
    std::string deviceId;
    std::string uniqueId;
    std::uint16_t tcpPort{};
};

std::vector<WakeDeviceEntry> ParseWakeInventory(const std::string& text) {
    std::size_t region = 0;
    std::size_t regionEnd = text.size();
    const std::size_t key = text.find("\"devices\"");
    if (key != std::string::npos) {
        const std::size_t open = text.find('[', key);
        if (open != std::string::npos) {
            region = open + 1;
            int depth = 1;
            bool inString = false;
            std::size_t scan = region;
            for (; scan < text.size(); ++scan) {
                const char ch = text[scan];
                if (inString) {
                    if (ch == '\\') {
                        ++scan;
                    } else if (ch == '"') {
                        inString = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    inString = true;
                } else if (ch == '[') {
                    ++depth;
                } else if (ch == ']') {
                    if (--depth == 0) break;
                }
            }
            if (scan < text.size()) regionEnd = scan;
        }
    }

    std::vector<WakeDeviceEntry> entries;
    std::size_t pos = region;
    while (pos < regionEnd) {
        const std::size_t begin = text.find('{', pos);
        if (begin == std::string::npos || begin >= regionEnd) break;
        int depth = 0;
        std::size_t end = begin;
        bool inString = false;
        for (; end < regionEnd; ++end) {
            const char ch = text[end];
            if (inString) {
                if (ch == '\\') {
                    ++end;
                } else if (ch == '"') {
                    inString = false;
                }
                continue;
            }
            if (ch == '"') {
                inString = true;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                if (--depth == 0) {
                    ++end;
                    break;
                }
            }
        }
        if (depth != 0) break;
        const std::string object = text.substr(begin, end - begin);
        pos = end;

        const std::int64_t port = JsonGetInt(object, "tcp_port", 0);
        if (port <= 0 || port > 65535) continue;
        WakeDeviceEntry entry;
        entry.tcpPort = static_cast<std::uint16_t>(port);
        entry.deviceId = JsonGetString(object, "device_id");
        entry.uniqueId = JsonGetString(object, "unique_id");
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool SelectWakeEntry(const std::vector<WakeDeviceEntry>& entries,
                     const std::string& deviceId,
                     WakeDeviceEntry& selected) {
    if (entries.empty()) return false;
    if (deviceId.empty()) {
        if (entries.size() != 1) return false;
        selected = entries.front();
        return true;
    }
    for (const auto& entry : entries) {
        if (entry.deviceId == deviceId || entry.uniqueId == deviceId) {
            selected = entry;
            return true;
        }
    }
    return false;
}

} // namespace

bool WakeDevice(const std::wstring& address, const std::wstring& deviceId,
                std::uint16_t& controlPort, std::wstring& error,
                const std::wstring& bindAddress) noexcept {
    error.clear();
    TcpConnection connection;
    if (!connection.Connect(address, 9200, 2000, error, bindAddress)) return false;

    const std::string enumerate = "{\"request\":\"enumerate_devices\",\"subscription_id\":\"0\"}\n";
    if (!connection.SendAll(enumerate.data(), enumerate.size(), 2000)) {
        error = L"wake enumerate send failed";
        return false;
    }

    const std::string wanted = ToUtf8(deviceId);
    const auto readInventory = [&](std::string& text) -> bool {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        std::size_t received = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            char buffer[1024]{};
            if (!connection.ReadSome(buffer, sizeof(buffer) - 1, received, 200)) {
                continue;
            }
            text.append(buffer, received);
            if (text.find("\"devices\"") != std::string::npos &&
                text.find(']') != std::string::npos) {
                return true;
            }
            if (text.size() > 65536) return true;
        }
        return !text.empty();
    };

    std::string firstResponse;
    (void)readInventory(firstResponse);
    WakeDeviceEntry selected;
    if (SelectWakeEntry(ParseWakeInventory(firstResponse), wanted, selected)) {
        controlPort = selected.tcpPort;
        connection.Close();
        return true;
    }

    if (!wanted.empty()) {
        const std::string enable = "{\"request\":\"enable_device\",\"device_id\":\"" +
            JsonEscape(wanted) + "\",\"subscription_id\":\"0\"}\n";
        (void)connection.SendAll(enable.data(), enable.size(), 2000);
    }
    (void)connection.SendAll("BEGIN_KEEPALIVE\n", 16, 2000);

    std::string secondResponse;
    (void)readInventory(secondResponse);
    WakeDeviceEntry afterEnable;
    if (SelectWakeEntry(ParseWakeInventory(secondResponse), wanted, afterEnable)) {
        controlPort = afterEnable.tcpPort;
        connection.Close();
        return true;
    }
    error = L"wake did not yield a tcp_port";
    return false;
}

} // namespace ammod::raat
