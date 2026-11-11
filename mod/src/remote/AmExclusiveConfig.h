#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace ammod::remote {

using json = nlohmann::json;

inline std::wstring GetAmExclusiveIniPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"PUBLIC", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"C:\\Users\\Public\\AMExclusive\\am-exclusive.ini";
    std::wstring path(buffer, length);
    path += L"\\AMExclusive\\am-exclusive.ini";
    return path;
}

inline std::wstring ReadIniTextW(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    const auto path = GetAmExclusiveIniPath();
    wchar_t value[1024]{};
    GetPrivateProfileStringW(section, key, fallback, value, static_cast<DWORD>(std::size(value)), path.c_str());
    return value;
}

inline int ReadIniIntW(const wchar_t* section, const wchar_t* key, int fallback = 0) {
    const auto path = GetAmExclusiveIniPath();
    return GetPrivateProfileIntW(section, key, fallback, path.c_str());
}

inline void WriteIniTextW(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    const auto path = GetAmExclusiveIniPath();
    WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
}

inline void FlushIniW() {
    const auto path = GetAmExclusiveIniPath();
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

inline std::string Utf8Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

inline std::wstring Utf8Widen(const std::string& text) {
    if (text.empty()) return {};
    int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), chars);
    return result;
}

inline json ReadFullAmExclusiveConfig() {
    json root = json::object();

    // [mod]
    std::string outputTab = Utf8Narrow(ReadIniTextW(L"mod", L"output_tab", L"local"));
    if (outputTab.empty()) outputTab = "local";
    std::string output = Utf8Narrow(ReadIniTextW(L"mod", L"output", L"wasapi"));
    if (output.empty()) output = "wasapi";
    std::string playerMode = Utf8Narrow(ReadIniTextW(L"mod", L"player_mode", L"am"));
    if (playerMode.empty()) playerMode = "am";

    int period100ns = ReadIniIntW(L"mod", L"period_100ns", 200000);
    int bufferMs = period100ns / 10000;
    if (bufferMs <= 0) bufferMs = 20;

    int allowResampling = ReadIniIntW(L"mod", L"allow_resampling", 0);
    int forceHires = ReadIniIntW(L"mod", L"force_hires", 1);
    std::string wasapiDevice = Utf8Narrow(ReadIniTextW(L"mod", L"wasapi_device", L""));
    std::string wasapiName = Utf8Narrow(ReadIniTextW(L"mod", L"wasapi_name", L""));
    std::string asioDriver = Utf8Narrow(ReadIniTextW(L"mod", L"asio_driver", L""));
    std::string asioName = Utf8Narrow(ReadIniTextW(L"mod", L"asio_name", L""));

    root["mod"] = {
        {"output_tab", outputTab},
        {"output", output},
        {"player_mode", playerMode},
        {"period_100ns", period100ns},
        {"buffer_ms", bufferMs},
        {"allow_resampling", allowResampling},
        {"force_hires", forceHires},
        {"wasapi_device", wasapiDevice},
        {"wasapi_name", wasapiName},
        {"asio_driver", asioDriver},
        {"asio_name", asioName}
    };

    // [local]
    int localEnabled = ReadIniIntW(L"local", L"enabled", 1);
    int reservoirMs = ReadIniIntW(L"local", L"reservoir_ms", 2000);
    int timer100ns = ReadIniIntW(L"local", L"timer_100ns", 5000);
    int dedicatedCore = ReadIniIntW(L"local", L"dedicated_core", 1);
    int processPriority = ReadIniIntW(L"local", L"process_priority", 0);
    int disableThreadBoost = ReadIniIntW(L"local", L"disable_thread_boost", 1);
    int largePages = ReadIniIntW(L"local", L"large_pages", 1);
    int maxQueueBlocks = ReadIniIntW(L"local", L"max_queue_blocks", 4096);
    bool systemTiming = (timer100ns != 0 || dedicatedCore != 0 || largePages != 0);

    root["local"] = {
        {"enabled", localEnabled},
        {"reservoir_ms", reservoirMs},
        {"system_timing", systemTiming ? 1 : 0},
        {"timer_100ns", timer100ns},
        {"dedicated_core", dedicatedCore},
        {"process_priority", processPriority},
        {"disable_thread_boost", disableThreadBoost},
        {"large_pages", largePages},
        {"max_queue_blocks", maxQueueBlocks}
    };

    // [dlna]
    std::string dlnaUdn = Utf8Narrow(ReadIniTextW(L"dlna", L"device_udn", L""));
    std::string dlnaName = Utf8Narrow(ReadIniTextW(L"dlna", L"device_name", L""));
    std::string dlnaLocation = Utf8Narrow(ReadIniTextW(L"dlna", L"location", L""));
    std::string streamFormat = Utf8Narrow(ReadIniTextW(L"dlna", L"stream_format", L"wav"));
    int dlnaBufferMs = ReadIniIntW(L"dlna", L"buffer_ms", 3000);
    int followTrackDuration = ReadIniIntW(L"dlna", L"follow_track_duration", 1);
    int streamSeconds = ReadIniIntW(L"dlna", L"stream_seconds", 600);
    std::string formatPref = Utf8Narrow(ReadIniTextW(L"dlna", L"format_pref", L"lpcm"));
    int maxBits = ReadIniIntW(L"dlna", L"max_bits", 24);
    std::string pcmByteOrder = Utf8Narrow(ReadIniTextW(L"dlna", L"pcm_byte_order", L"big"));
    std::string manualUrls = Utf8Narrow(ReadIniTextW(L"dlna", L"manual_urls", L""));

    root["dlna"] = {
        {"device_udn", dlnaUdn},
        {"device_name", dlnaName},
        {"location", dlnaLocation},
        {"stream_format", streamFormat},
        {"buffer_ms", dlnaBufferMs},
        {"follow_track_duration", followTrackDuration},
        {"stream_seconds", streamSeconds},
        {"format_pref", formatPref},
        {"max_bits", maxBits},
        {"pcm_byte_order", pcmByteOrder},
        {"manual_urls", manualUrls}
    };

    // [roon] (check roon first, then r_device fallback)
    std::string roonId = Utf8Narrow(ReadIniTextW(L"roon", L"device_id", L""));
    if (roonId.empty()) roonId = Utf8Narrow(ReadIniTextW(L"r_device", L"device_id", L""));

    std::string roonName = Utf8Narrow(ReadIniTextW(L"roon", L"device_name", L""));
    if (roonName.empty()) roonName = Utf8Narrow(ReadIniTextW(L"r_device", L"device_name", L""));

    std::string roonAddress = Utf8Narrow(ReadIniTextW(L"roon", L"address", L""));
    if (roonAddress.empty()) roonAddress = Utf8Narrow(ReadIniTextW(L"r_device", L"address", L""));

    int roonPort = ReadIniIntW(L"roon", L"port", 0);
    if (roonPort == 0) roonPort = ReadIniIntW(L"r_device", L"port", 0);

    std::string raatVersion = Utf8Narrow(ReadIniTextW(L"roon", L"raat_version", L""));
    if (raatVersion.empty()) raatVersion = Utf8Narrow(ReadIniTextW(L"r_device", L"raat_version", L""));

    int roonBufferMs = ReadIniIntW(L"roon", L"buffer_ms", 1000);
    if (roonBufferMs == 1000) {
        int rBuf = ReadIniIntW(L"r_device", L"buffer_ms", 1000);
        if (rBuf != 1000) roonBufferMs = rBuf;
    }

    int startLeadMs = ReadIniIntW(L"roon", L"start_lead_ms", 1000);
    if (startLeadMs == 1000) {
        int rLead = ReadIniIntW(L"r_device", L"start_lead_ms", 1000);
        if (rLead != 1000) startLeadMs = rLead;
    }

    int wireBits = ReadIniIntW(L"roon", L"wire_bits", 0);
    int soodPort = ReadIniIntW(L"roon", L"sood_port", 0);

    root["roon"] = {
        {"device_id", roonId},
        {"device_name", roonName},
        {"address", roonAddress},
        {"port", roonPort},
        {"raat_version", raatVersion},
        {"buffer_ms", roonBufferMs},
        {"start_lead_ms", startLeadMs},
        {"wire_bits", wireBits},
        {"sood_port", soodPort}
    };

    return root;
}

inline bool SaveFullAmExclusiveConfig(const json& update) {
    try {
        // [mod]
        if (update.contains("mod") && update["mod"].is_object()) {
            const auto& mod = update["mod"];
            if (mod.contains("output_tab")) {
                WriteIniTextW(L"mod", L"output_tab", Utf8Widen(mod["output_tab"].get<std::string>()));
            }
            if (mod.contains("output")) {
                WriteIniTextW(L"mod", L"output", Utf8Widen(mod["output"].get<std::string>()));
            }
            if (mod.contains("player_mode")) {
                WriteIniTextW(L"mod", L"player_mode", Utf8Widen(mod["player_mode"].get<std::string>()));
            }
            if (mod.contains("period_100ns")) {
                WriteIniTextW(L"mod", L"period_100ns", std::to_wstring(mod["period_100ns"].get<int>()));
            } else if (mod.contains("buffer_ms")) {
                int ms = mod["buffer_ms"].get<int>();
                WriteIniTextW(L"mod", L"period_100ns", std::to_wstring(ms * 10000));
            }
            if (mod.contains("allow_resampling")) {
                int v = mod["allow_resampling"].is_boolean() ? (mod["allow_resampling"].get<bool>() ? 1 : 0) : mod["allow_resampling"].get<int>();
                WriteIniTextW(L"mod", L"allow_resampling", std::to_wstring(v));
            }
            if (mod.contains("force_hires")) {
                int v = mod["force_hires"].is_boolean() ? (mod["force_hires"].get<bool>() ? 1 : 0) : mod["force_hires"].get<int>();
                WriteIniTextW(L"mod", L"force_hires", std::to_wstring(v));
            }
            if (mod.contains("wasapi_device")) {
                WriteIniTextW(L"mod", L"wasapi_device", Utf8Widen(mod["wasapi_device"].get<std::string>()));
            }
            if (mod.contains("wasapi_name")) {
                WriteIniTextW(L"mod", L"wasapi_name", Utf8Widen(mod["wasapi_name"].get<std::string>()));
            }
            if (mod.contains("asio_driver")) {
                WriteIniTextW(L"mod", L"asio_driver", Utf8Widen(mod["asio_driver"].get<std::string>()));
            }
            if (mod.contains("asio_name")) {
                WriteIniTextW(L"mod", L"asio_name", Utf8Widen(mod["asio_name"].get<std::string>()));
            }
        }

        // [local]
        if (update.contains("local") && update["local"].is_object()) {
            const auto& loc = update["local"];
            if (loc.contains("enabled")) {
                int v = loc["enabled"].is_boolean() ? (loc["enabled"].get<bool>() ? 1 : 0) : loc["enabled"].get<int>();
                WriteIniTextW(L"local", L"enabled", std::to_wstring(v));
            }
            if (loc.contains("reservoir_ms")) {
                WriteIniTextW(L"local", L"reservoir_ms", std::to_wstring(loc["reservoir_ms"].get<int>()));
            }
            if (loc.contains("system_timing")) {
                bool sys = loc["system_timing"].is_boolean() ? loc["system_timing"].get<bool>() : (loc["system_timing"].get<int>() != 0);
                WriteIniTextW(L"local", L"timer_100ns", sys ? L"5000" : L"0");
                WriteIniTextW(L"local", L"dedicated_core", sys ? L"1" : L"0");
                WriteIniTextW(L"local", L"large_pages", sys ? L"1" : L"0");
                WriteIniTextW(L"local", L"disable_thread_boost", L"1");
            }
            if (loc.contains("process_priority")) {
                WriteIniTextW(L"local", L"process_priority", std::to_wstring(loc["process_priority"].get<int>()));
            }
        }

        // [dlna]
        if (update.contains("dlna") && update["dlna"].is_object()) {
            const auto& dlna = update["dlna"];
            if (dlna.contains("device_udn")) {
                WriteIniTextW(L"dlna", L"device_udn", Utf8Widen(dlna["device_udn"].get<std::string>()));
            }
            if (dlna.contains("device_name")) {
                WriteIniTextW(L"dlna", L"device_name", Utf8Widen(dlna["device_name"].get<std::string>()));
            }
            if (dlna.contains("location")) {
                WriteIniTextW(L"dlna", L"location", Utf8Widen(dlna["location"].get<std::string>()));
            }
            if (dlna.contains("stream_format")) {
                WriteIniTextW(L"dlna", L"stream_format", Utf8Widen(dlna["stream_format"].get<std::string>()));
            }
            if (dlna.contains("buffer_ms")) {
                WriteIniTextW(L"dlna", L"buffer_ms", std::to_wstring(dlna["buffer_ms"].get<int>()));
            }
            if (dlna.contains("follow_track_duration")) {
                int v = dlna["follow_track_duration"].is_boolean() ? (dlna["follow_track_duration"].get<bool>() ? 1 : 0) : dlna["follow_track_duration"].get<int>();
                WriteIniTextW(L"dlna", L"follow_track_duration", std::to_wstring(v));
            }
            if (dlna.contains("manual_urls")) {
                WriteIniTextW(L"dlna", L"manual_urls", Utf8Widen(dlna["manual_urls"].get<std::string>()));
            }
        }

        // [roon] and [r_device]
        if (update.contains("roon") && update["roon"].is_object()) {
            const auto& roon = update["roon"];
            for (const auto* sec : {L"roon", L"r_device"}) {
                if (roon.contains("device_id")) {
                    WriteIniTextW(sec, L"device_id", Utf8Widen(roon["device_id"].get<std::string>()));
                }
                if (roon.contains("device_name")) {
                    WriteIniTextW(sec, L"device_name", Utf8Widen(roon["device_name"].get<std::string>()));
                }
                if (roon.contains("address")) {
                    WriteIniTextW(sec, L"address", Utf8Widen(roon["address"].get<std::string>()));
                }
                if (roon.contains("port")) {
                    WriteIniTextW(sec, L"port", std::to_wstring(roon["port"].get<int>()));
                }
                if (roon.contains("raat_version")) {
                    WriteIniTextW(sec, L"raat_version", Utf8Widen(roon["raat_version"].get<std::string>()));
                }
                if (roon.contains("buffer_ms")) {
                    WriteIniTextW(sec, L"buffer_ms", std::to_wstring(roon["buffer_ms"].get<int>()));
                }
                if (roon.contains("start_lead_ms")) {
                    WriteIniTextW(sec, L"start_lead_ms", std::to_wstring(roon["start_lead_ms"].get<int>()));
                }
                if (roon.contains("wire_bits")) {
                    WriteIniTextW(sec, L"wire_bits", std::to_wstring(roon["wire_bits"].get<int>()));
                }
                if (roon.contains("sood_port")) {
                    WriteIniTextW(sec, L"sood_port", std::to_wstring(roon["sood_port"].get<int>()));
                }
            }
        }

        FlushIniW();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace ammod::remote
