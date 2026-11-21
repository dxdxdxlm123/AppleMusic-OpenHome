#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <algorithm>

namespace ammod::audio_v2 {

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// High-resolution pacing helper for RAAT and DLNA streaming.
// Combines Windows 10/11 high-resolution waitable timer (for CPU-friendly bulk waits)
// with sub-millisecond QPC/steady_clock microsecond alignment to reduce pacing jitter
// from ~1.5ms down to ~20-50us without spinning CPU during long pauses.
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

    // Sleep for waitUs microseconds.
    void SleepUs(std::uint64_t waitUs) noexcept {
        if (waitUs == 0) return;
        const auto start = NowUs();

        // For sleeps >= 1000us (~1ms), surrender CPU via kernel waitable timer,
        // waking up ~250us early for fine-grained sub-millisecond alignment.
        if (timer_ && waitUs >= 1000u) {
            const std::uint64_t timerUs = waitUs - 250u;
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(timerUs * 10ull); // 100ns units, negative = relative
            if (::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                ::WaitForSingleObject(timer_, static_cast<DWORD>((timerUs / 1000u) + 5u));
            }
        }

        // Sub-millisecond fine precision alignment
        while (true) {
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

    // Sleep for waitUs microseconds, checking stopSignal every iteration.
    void SleepUs(std::uint64_t waitUs, const std::atomic<bool>& stopSignal) noexcept {
        if (waitUs == 0 || stopSignal.load(std::memory_order_acquire)) return;
        const auto start = NowUs();

        if (timer_ && waitUs >= 1000u) {
            const std::uint64_t timerUs = waitUs - 250u;
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(timerUs * 10ull);
            if (::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                ::WaitForSingleObject(timer_, static_cast<DWORD>((timerUs / 1000u) + 5u));
            }
            if (stopSignal.load(std::memory_order_acquire)) return;
        }

        while (!stopSignal.load(std::memory_order_acquire)) {
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

private:
    HANDLE timer_{nullptr};
};

} // namespace ammod::audio_v2
