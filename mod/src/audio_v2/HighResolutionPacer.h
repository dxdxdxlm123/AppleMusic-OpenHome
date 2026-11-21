#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <thread>

namespace ammod::audio_v2 {

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// High-resolution pacing helper for RAAT and DLNA streaming.
// Supports 3 pacing modes (high_precision_pacing = 0 / 1 / 2):
//   0 = Coarse sleep (std::this_thread::sleep_for / Sleep, softest)
//   1 = Pure high-resolution waitable timer, zero-spin (high clarity, zero CPU spin / hash, default recommended)
//   2 = High-resolution waitable timer + QPC sub-millisecond tail spin alignment (sharpest)
class HighResolutionPacer final {
public:
    HighResolutionPacer() noexcept {
        timer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) {
            timer_ = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
    }

    ~HighResolutionPacer() noexcept {
        if (timer_) {
            ::CloseHandle(timer_);
            timer_ = nullptr;
        }
    }

    HighResolutionPacer(const HighResolutionPacer&) = delete;
    HighResolutionPacer& operator=(const HighResolutionPacer&) = delete;

    static std::uint64_t NowUs() noexcept {
        using Clock = std::chrono::steady_clock;
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch()).count());
    }

    // Mode 1: Pure waitable timer sleep without any QPC tail spin.
    // Directly sleeps waitUs using CREATE_WAITABLE_TIMER_HIGH_RESOLUTION.
    void SleepUsPure(std::uint64_t waitUs, const std::atomic<bool>* stopSignal = nullptr) noexcept {
        if (waitUs == 0) return;
        if (stopSignal && stopSignal->load(std::memory_order_acquire)) return;

        if (timer_) {
            LARGE_INTEGER due{};
            // Negative value represents relative time in 100ns units (1 us = 10 * 100ns)
            due.QuadPart = -static_cast<LONGLONG>(waitUs * 10ull);
            if (::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                const DWORD timeoutMs = static_cast<DWORD>((waitUs + 999ull) / 1000ull + 50u);
                ::WaitForSingleObject(timer_, timeoutMs);
                return;
            }
        }

        // Fallback to coarse sleep
        const auto ms = std::max<std::uint64_t>(1u, waitUs / 1000u);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    void SleepUsPure(std::uint64_t waitUs, const std::atomic<bool>& stopSignal) noexcept {
        SleepUsPure(waitUs, &stopSignal);
    }

    void SleepUsPure(std::uint64_t waitUs) noexcept {
        SleepUsPure(waitUs, nullptr);
    }

    // Mode 0: Coarse sleep fallback.
    void SleepUsCoarse(std::uint64_t waitUs, const std::atomic<bool>* stopSignal = nullptr) noexcept {
        if (waitUs == 0) return;
        if (stopSignal && stopSignal->load(std::memory_order_acquire)) return;
        const auto ms = std::max<std::uint64_t>(1u, waitUs / 1000u);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    void SleepUsCoarse(std::uint64_t waitUs, const std::atomic<bool>& stopSignal) noexcept {
        SleepUsCoarse(waitUs, &stopSignal);
    }

    void SleepUsCoarse(std::uint64_t waitUs) noexcept {
        SleepUsCoarse(waitUs, nullptr);
    }

    // Mode 2: Hybrid waitable timer + tail spin alignment.
    void SleepUsSpin(std::uint64_t waitUs, const std::atomic<bool>* stopSignal = nullptr) noexcept {
        if (waitUs == 0) return;
        if (stopSignal && stopSignal->load(std::memory_order_acquire)) return;
        const auto start = NowUs();

        if (timer_ && waitUs >= 1000u) {
            const std::uint64_t timerUs = waitUs - 250u;
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(timerUs * 10ull);
            if (::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                ::WaitForSingleObject(timer_, static_cast<DWORD>((timerUs / 1000u) + 5u));
            }
            if (stopSignal && stopSignal->load(std::memory_order_acquire)) return;
        }

        while (true) {
            if (stopSignal && stopSignal->load(std::memory_order_acquire)) break;
            const auto now = NowUs();
            if (now >= start + waitUs) break;
            const auto rem = (start + waitUs) - now;
            if (rem > 50u) {
                ::SwitchToThread();
            } else {
                YieldProcessor();
            }
        }
    }

    void SleepUsSpin(std::uint64_t waitUs, const std::atomic<bool>& stopSignal) noexcept {
        SleepUsSpin(waitUs, &stopSignal);
    }

    void SleepUsSpin(std::uint64_t waitUs) noexcept {
        SleepUsSpin(waitUs, nullptr);
    }

    // Universal dispatcher according to mode (0 = coarse, 1 = pure waitable zero-spin, 2 = spin-aligned)
    void SleepUs(std::uint64_t waitUs, int mode = 1, const std::atomic<bool>* stopSignal = nullptr) noexcept {
        if (mode == 0) {
            SleepUsCoarse(waitUs, stopSignal);
        } else if (mode == 2) {
            SleepUsSpin(waitUs, stopSignal);
        } else {
            SleepUsPure(waitUs, stopSignal);
        }
    }

    void SleepUs(std::uint64_t waitUs, const std::atomic<bool>& stopSignal, int mode = 1) noexcept {
        SleepUs(waitUs, mode, &stopSignal);
    }

private:
    HANDLE timer_{nullptr};
};

} // namespace ammod::audio_v2
