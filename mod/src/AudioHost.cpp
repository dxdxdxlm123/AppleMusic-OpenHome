// am-exclusive-audiohost.exe — the "HiFi player" engine.
//
// It owns the endpoint (WASAPI exclusive or ASIO) in its own process, drains
// the shared ring written by the hook, and never talks to Apple Music. The
// hook falls back to its in-process sink when this process is absent.

#include "audio_v2/AsioExclusiveSink.h"
#include "audio_v2/DlnaSink.h"
#include "audio_v2/LocalPlaybackTuning.h"
#include "audio_v2/MemoryLock.h"
#include "audio_v2/RaatSink.h"
#include "audio_v2/RemoteSink.h"
#include "audio_v2/RingPcmSource.h"
#include "audio_v2/SharedAudioRing.h"
#include "audio_v2/WasapiExclusiveSink.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

using ammod::audio_v2::AudioSinkOpenParams;
using ammod::audio_v2::HostMessage;
using ammod::audio_v2::HostMessageType;
using ammod::audio_v2::HostObjectName;
using ammod::audio_v2::HostTargetKind;
using ammod::audio_v2::IAudioSink;
using ammod::audio_v2::kHostBuildId;
using ammod::audio_v2::kHostProtocolVersion;
using ammod::audio_v2::RingPcmSource;
using ammod::audio_v2::SharedRingHeader;
using ammod::audio_v2::kHostDataEventPrefix;
using ammod::audio_v2::kHostPipePrefix;
using ammod::audio_v2::kHostRingPrefix;
using ammod::audio_v2::kHostSpaceEventPrefix;
using ammod::audio_v2::kSharedRingBytes;
using ammod::audio_v2::kSharedRingMagic;
using ammod::audio_v2::kSharedRingMaxFrames;
using ammod::audio_v2::kSharedRingVersion;

std::wstring g_logPath;
CRITICAL_SECTION g_logLock;
// v5: ServeClient() blocks in ReadMessage(), so sink completion is observed
// by a dedicated monitor thread. This lock only guards the unique_ptr itself;
// sink teardown is moved out before Stop()/Close() so the lock stays short.
CRITICAL_SECTION g_sinkLock;
std::atomic<bool> g_eosMonitorRunning{false};

void Log(const wchar_t* text) noexcept {
    if (!text || g_logPath.empty()) return;
    EnterCriticalSection(&g_logLock);
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_logPath.c_str(), L"a, ccs=UTF-8") == 0 && file) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        fwprintf(file, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u pid=%u %s\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                 now.wSecond, now.wMilliseconds, GetCurrentProcessId(), text);
        fclose(file);
    }
    LeaveCriticalSection(&g_logLock);
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : value.substr(0, slash);
}

struct HostState final {
    HANDLE pipe{INVALID_HANDLE_VALUE};
    HANDLE ringMapping{};
    void* ringView{};
    std::size_t ringBytes{};
    std::uint32_t ringCapacityFrames{};
    HANDLE dataEvent{};
    HANDLE spaceEvent{};
    SharedRingHeader* header{};
    RingPcmSource source{};
    std::unique_ptr<IAudioSink> sink;
};

#ifndef SEC_LARGE_PAGES
#define SEC_LARGE_PAGES 0x80000000
#endif

// Same right the large-page allocation path needs. Kept local so the host's
// dynamic imports stay independent of the hook module.
bool EnableLockMemoryPrivilege() noexcept {
    HMODULE advapi = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!advapi) advapi = GetModuleHandleW(L"advapi32.dll");
    if (!advapi) return false;
    using OpenTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
    using LookupFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, PLUID);
    using AdjustFn = BOOL(WINAPI*)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD,
                                   PTOKEN_PRIVILEGES, PDWORD);
    auto openToken = reinterpret_cast<OpenTokenFn>(GetProcAddress(advapi, "OpenProcessToken"));
    auto lookup = reinterpret_cast<LookupFn>(GetProcAddress(advapi, "LookupPrivilegeValueW"));
    auto adjust = reinterpret_cast<AdjustFn>(GetProcAddress(advapi, "AdjustTokenPrivileges"));
    if (!openToken || !lookup || !adjust) return false;
    HANDLE token = nullptr;
    if (!openToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    bool ok = lookup(nullptr, L"SeLockMemoryPrivilege", &privileges.Privileges[0].Luid) != FALSE;
    if (ok) {
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        ok = adjust(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) != FALSE &&
             GetLastError() == ERROR_SUCCESS;
    }
    (void)CloseHandle(token);
    return ok;
}

void CloseSink(HostState& state) noexcept {
    // Retire the pointer under the lock, then tear the sink down outside it.
    std::unique_ptr<IAudioSink> doomed;
    EnterCriticalSection(&g_sinkLock);
    doomed = std::move(state.sink);
    LeaveCriticalSection(&g_sinkLock);
    if (doomed) {
        doomed->Stop();
        doomed->Close();
    }
    if (state.header) state.header->hostState = 0;
}

// ServeClient() owns the only pipe loop and blocks on ReadMessage(); there is
// no idle window to poll the sink. A 10 ms monitor is cheap and keeps the
// completion stamp off the audio threads entirely.
DWORD WINAPI EosMonitorThunk(void* context) noexcept {
    auto* state = static_cast<HostState*>(context);
    while (g_eosMonitorRunning.load(std::memory_order_acquire)) {
        bool complete = false;
        EnterCriticalSection(&g_sinkLock);
        if (state->sink) complete = state->sink->EndOfStreamComplete();
        LeaveCriticalSection(&g_sinkLock);
        if (complete && state->header) {
            const auto generation =
                state->header->generation.load(std::memory_order_acquire);
            if (generation != SharedRingHeader::kNoGeneration) {
                state->header->eosCompleteGeneration.store(
                    generation, std::memory_order_release);
            }
        }
        Sleep(10);
    }
    return 0;
}

bool ReadMessage(HANDLE pipe, HostMessage& message) noexcept {
    DWORD read = 0;
    if (!ReadFile(pipe, &message, sizeof(message), &read, nullptr)) return false;
    return read == sizeof(message) && message.size == sizeof(HostMessage);
}

bool WriteMessage(HANDLE pipe, const HostMessage& message) noexcept {
    DWORD written = 0;
    return WriteFile(pipe, &message, sizeof(message), &written, nullptr) &&
           written == sizeof(message);
}

// The engine owns the audio threads, so it also owns the JPLAY-style tuning.
// Reading the same [local] section keeps the panel semantics identical; the
// hook process deliberately does not need to configure anything here.
ammod::audio_v2::LocalPlaybackTuning ReadTuning() noexcept {
    ammod::audio_v2::LocalPlaybackTuning tuning{};
    const std::wstring path = ExeDirectory() + L"\\am-exclusive.ini";
    auto number = [&path](const wchar_t* key, int fallback) -> std::uint32_t {
        return static_cast<std::uint32_t>(
            GetPrivateProfileIntW(L"local", key, fallback, path.c_str()));
    };
    tuning.enabled = number(L"enabled", 0) != 0;
    tuning.reservoirMs = number(L"reservoir_ms", 100);
    tuning.timerResolution100ns = number(L"timer_100ns", 0);
    tuning.dedicatedCore = number(L"dedicated_core", 0) != 0;
    tuning.processPriority = number(L"process_priority", 0) != 0;
    tuning.disableThreadBoost = number(L"disable_thread_boost", 1) != 0;
    tuning.largePages = number(L"large_pages", 0) != 0;
    tuning.maxQueueBlocks = number(L"max_queue_blocks", 4096);
    tuning.Clamp();
    return tuning;
}

// Size of the large-page section that backs the cross-process ring. This is a
// *reservation/pinning* size, not the logical ring depth: OpenTarget() still
// clamps capacityFrames to kSharedRingMaxFrames (~10 s at 192 kHz), so a big
// pool buys TLB reach and guaranteed residency without adding playback delay.
std::uint32_t ReadLargePagePoolMb() noexcept {
    const std::wstring path = ExeDirectory() + L"\\am-exclusive.ini";
    const auto value = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"local", L"large_page_pool_mb", 512, path.c_str()));
    if (value == 0 || value > 4096u) return 512u;
    return value;
}

HRESULT OpenTarget(HostState& state, const HostMessage& request) noexcept {
    CloseSink(state);
    ammod::audio_v2::LocalPlaybackTuningRuntime::Instance().Configure(ReadTuning());
    AudioSinkOpenParams params{};
    params.format = request.format;
    params.periodFrames = request.periodFrames;
    params.alignmentRetries = request.alignmentRetries ? request.alignmentRetries : 2;
    params.streamBufferMs = request.streamBufferMs;
    params.streamSeconds = request.streamSeconds;
    params.sourceReadyEvent = state.dataEvent;
    params.endpointId = request.endpointId;
    params.asioDriverClsid = request.asioClsid;
    params.asioDriverName = request.asioName;
    if (request.targetKind == static_cast<std::uint32_t>(
            ammod::audio_v2::HostTargetKind::Raat) ||
        request.raatAddress[0] != L'\0') {
        params.raat.deviceId = request.endpointId;
        params.raat.friendlyName = request.asioName;
        params.raat.address = request.raatAddress;
        params.raat.raatVersion = request.raatVersion;
        params.raat.controlPort = static_cast<std::uint16_t>(request.raatControlPort);
        params.endpointId.clear();
        params.asioDriverClsid.clear();
        params.dlna.location.clear();
    } else if (request.targetKind == static_cast<std::uint32_t>(
            ammod::audio_v2::HostTargetKind::Dlna) ||
        request.dlnaLocation[0] != L'\0') {
        params.dlna.udn = request.endpointId;
        params.dlna.friendlyName = request.asioName;
        params.dlna.location = request.dlnaLocation;
        params.endpointId.clear();
        params.asioDriverClsid.clear();
    }

    if (state.header) {
        std::uint32_t capacity = request.ringFrames;
        if (capacity < 4096u) capacity = 4096u;
        if (capacity > kSharedRingMaxFrames) capacity = kSharedRingMaxFrames;
        if (state.ringCapacityFrames != 0 && capacity > state.ringCapacityFrames) {
            capacity = state.ringCapacityFrames;
        }
        ammod::audio_v2::SharedRingReset(
            state.header, params.format.sampleRate,
            params.format.validBits ? params.format.validBits : params.format.containerBits,
            params.format.containerBits,
            static_cast<std::uint32_t>(params.format.encoding),
            request.generation, capacity);
    }

    std::unique_ptr<IAudioSink> created;
    if (!params.raat.address.empty() || !params.raat.deviceId.empty()) {
        created = std::make_unique<ammod::audio_v2::RaatSink>();
    } else if (!params.dlna.location.empty()) {
        // Network renderer: the engine owns the SOAP control client, the
        // HTTP stream server and the encoder, none of which belong in the
        // Apple Music process.
        created = std::make_unique<ammod::audio_v2::DlnaSink>();
    } else if (!params.asioDriverClsid.empty()) {
        created = std::make_unique<ammod::audio_v2::AsioExclusiveSink>();
    } else {
        created = std::make_unique<ammod::audio_v2::WasapiExclusiveSink>();
    }
    if (!created) return E_OUTOFMEMORY;
    const HRESULT hr = created->Open(params);
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&g_sinkLock);
    state.sink = std::move(created);
    LeaveCriticalSection(&g_sinkLock);
    return S_OK;
}

void ServeClient(HostState& state) noexcept {
    for (;;) {
        HostMessage request{};
        if (!ReadMessage(state.pipe, request)) break;
        HostMessage reply{};
        reply.type = request.type;
        reply.size = sizeof(HostMessage);
        reply.result = S_OK;

        switch (static_cast<HostMessageType>(request.type)) {
        case HostMessageType::Hello:
            reply.type = static_cast<std::uint32_t>(HostMessageType::HelloAck);
            // A mismatched layout can only be refused; the hook falls back to
            // its in-process sinks instead of corrupting the ring.
            reply.protocol = kHostProtocolVersion;
            reply.buildId = kHostBuildId;
            reply.result = (request.protocol == kHostProtocolVersion &&
                            request.buildId == kHostBuildId)
                ? S_OK : E_NOT_VALID_STATE;
            break;
        case HostMessageType::Open:
            reply.result = OpenTarget(state, request);
            reply.type = static_cast<std::uint32_t>(HostMessageType::OpenResult);
            Log(reply.result == S_OK ? L"open ok" : L"open failed");
            break;
        case HostMessageType::Start:
            reply.result = state.sink ? state.sink->Start(state.source) : E_UNEXPECTED;
            reply.type = static_cast<std::uint32_t>(HostMessageType::StartResult);
            Log(reply.result == S_OK ? L"start ok" : L"start failed");
            break;
        case HostMessageType::Pause:
            if (state.sink) state.sink->Pause();
            break;
        case HostMessageType::Stop:
            if (state.sink) state.sink->Stop();
            break;
        case HostMessageType::Close:
            CloseSink(state);
            break;
        case HostMessageType::Shutdown:
            CloseSink(state);
            (void)WriteMessage(state.pipe, reply);
            return;
        default:
            reply.result = E_INVALIDARG;
            break;
        }
        if (!WriteMessage(state.pipe, reply)) break;
    }
    CloseSink(state);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_sinkLock);
    g_logPath = ExeDirectory() + L"\\am-exclusive-audiohost.log";

    std::uint32_t session = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--session") == 0) {
            session = static_cast<std::uint32_t>(_wtoi(argv[i + 1]));
        }
    }
    if (session == 0) {
        DWORD current = 0;
        (void)ProcessIdToSessionId(GetCurrentProcessId(), &current);
        session = current;
    }

    const std::wstring mutexName = HostObjectName(L"Local\\AMEAudioHostMutex_", session);
    HANDLE instance = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instance) CloseHandle(instance);
        return 0;
    }

    const std::wstring pipeName = HostObjectName(kHostPipePrefix, session);
    const std::wstring ringName = HostObjectName(kHostRingPrefix, session);
    const std::wstring dataName = HostObjectName(kHostDataEventPrefix, session);
    const std::wstring spaceName = HostObjectName(kHostSpaceEventPrefix, session);

    HostState state{};
    // This process is the playback engine, so process-wide tuning belongs here
    // and only here: raise the class at startup so the pipe/ring threads are
    // covered even before the first track opens.
    ammod::audio_v2::LocalPlaybackTuningRuntime::SetProcessPriorityAllowed(true);
    ammod::audio_v2::LocalPlaybackTuningRuntime::Instance().Configure(ReadTuning());
    ammod::audio_v2::SetHardWorkingSetFloorEnabled(true);
    const bool largePagePrivilege = EnableLockMemoryPrivilege();
    Log(largePagePrivilege ? L"lock-memory privilege enabled"
                           : L"lock-memory privilege unavailable");
    if (!largePagePrivilege) {
        Log(L"large pages need SeLockMemoryPrivilege granted to the host account "
            L"(Local Security Policy > Lock pages in memory), not just enabled in the token");
    }
    // Query the platform large-page unit dynamically (usually 2 MB) so the
    // requested pool can be rounded to a legal section size without hardcoding
    // an SDK level.
    using GetLargePageMinimumFn = SIZE_T(WINAPI*)();
    auto getLargePageMinimum = reinterpret_cast<GetLargePageMinimumFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLargePageMinimum"));
    const std::size_t largePageUnit =
        (getLargePageMinimum && getLargePageMinimum() != 0)
            ? static_cast<std::size_t>(getLargePageMinimum()) : (2ull << 20);

    // The section is a pinned large-page *pool*, not the logical ring depth:
    // OpenTarget() still clamps capacityFrames to kSharedRingMaxFrames (~10 s
    // at 192 kHz). The extra space buys TLB reach and guaranteed residency,
    // and never turns into extra playback latency.
    const std::uint32_t poolMb = ReadLargePagePoolMb();
    bool usingLargePages = false;
    if (largePagePrivilege) {
        for (std::size_t megabytes = poolMb; megabytes >= 1u; megabytes /= 2u) {
            std::size_t bytes = static_cast<std::size_t>(megabytes) << 20;
            bytes = ((bytes + largePageUnit - 1) / largePageUnit) * largePageUnit;
            if (bytes == 0 || bytes > (4ull << 30)) continue;
            state.ringMapping = CreateFileMappingW(
                INVALID_HANDLE_VALUE, nullptr,
                PAGE_READWRITE | SEC_COMMIT | SEC_LARGE_PAGES, 0,
                static_cast<DWORD>(bytes), ringName.c_str());
            if (state.ringMapping) {
                state.ringBytes = bytes;
                usingLargePages = true;
                wchar_t detail[128]{};
                _snwprintf_s(detail, sizeof(detail) / sizeof(detail[0]), _TRUNCATE,
                             L"ring uses large pages: %u MB pool, %llu MB section",
                             poolMb, static_cast<unsigned long long>(bytes >> 20));
                Log(detail);
                break;
            }
        }
    }
    if (!state.ringMapping) {
        state.ringBytes = kSharedRingBytes;
        state.ringMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(state.ringBytes), ringName.c_str());
        Log(L"ring falls back to ordinary pages");
    }
    state.ringView = state.ringMapping
        ? MapViewOfFile(state.ringMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0) : nullptr;
    if (state.ringView && state.ringBytes != 0) {
        // Commit and touch every page once so the render thread never takes a
        // demand-zero fault on the audio path. For a 512 MB pool this is a
        // one-off startup cost, paid before the first track opens.
        std::memset(state.ringView, 0, state.ringBytes);
        const std::size_t computedFrames =
            (state.ringBytes - sizeof(SharedRingHeader)) /
            (ammod::audio_v2::kSharedRingChannels * sizeof(std::int32_t));
        // The pool is deliberately larger than the logical ring. Clamp here
        // so no later code can mistake the section size for buffer depth.
        state.ringCapacityFrames = static_cast<std::uint32_t>(
            computedFrames > kSharedRingMaxFrames ? kSharedRingMaxFrames : computedFrames);
        if (usingLargePages) {
            // SEC_LARGE_PAGES sections are already non-pageable and resident;
            // raising a 512 MB working-set floor would be both unnecessary and
            // likely refused by the quota. Try VirtualLock as a best effort
            // and report it, but never treat its refusal as a degradation.
            const bool locked = VirtualLock(state.ringView, state.ringBytes) != FALSE;
            Log(locked ? L"ring pinned: large pages resident + VirtualLock"
                       : L"ring pinned: large pages resident (VirtualLock quota-limited, "
                         L"large pages cannot be paged out)");
        } else {
            // Ordinary pages still need to be pinned. The hard working-set
            // floor is what makes VirtualLock cover the whole ring; if either
            // step is refused the ring keeps working, just less protected.
            const bool floor = ammod::audio_v2::EnsureLockableWorkingSet(state.ringBytes);
            const bool locked = VirtualLock(state.ringView, state.ringBytes) != FALSE;
            Log(locked ? L"ring locked in memory"
                       : (floor ? L"ring not locked; working-set floor only"
                                : L"ring not locked; no working-set floor"));
        }
    }
    state.dataEvent = CreateEventW(nullptr, FALSE, FALSE, dataName.c_str());
    state.spaceEvent = CreateEventW(nullptr, FALSE, FALSE, spaceName.c_str());
    if (!state.ringView || !state.dataEvent || !state.spaceEvent ||
        state.ringCapacityFrames == 0) {
        Log(L"init failed: ring or events unavailable");
        return 1;
    }
    state.header = static_cast<SharedRingHeader*>(state.ringView);
    state.header->magic = kSharedRingMagic;
    state.header->version = kSharedRingVersion;
    state.header->hostState = 0;
    if (!state.source.Attach(state.ringView, state.spaceEvent)) {
        Log(L"init failed: ring attach refused");
        return 1;
    }

    state.pipe = CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, sizeof(HostMessage) * 8, sizeof(HostMessage) * 8, 0, nullptr);
    if (state.pipe == INVALID_HANDLE_VALUE) {
        Log(L"init failed: named pipe unavailable");
        return 1;
    }
    // v5: the pipe loop blocks, so sink completion is published to the ring
    // from a separate monitor. Started once; the process exits with it running.
    g_eosMonitorRunning.store(true, std::memory_order_release);
    HANDLE monitorThread = CreateThread(nullptr, 0, &EosMonitorThunk, &state, 0, nullptr);
    if (monitorThread) {
        CloseHandle(monitorThread);
    } else {
        Log(L"warning: eos monitor thread unavailable; host completion will not be signalled");
    }
    Log(L"audiohost ready");

    for (;;) {
        const BOOL connected = ConnectNamedPipe(state.pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            Sleep(50);
            continue;
        }
        Log(L"client connected");
        ServeClient(state);
        DisconnectNamedPipe(state.pipe);
        Log(L"client disconnected");
    }
}
