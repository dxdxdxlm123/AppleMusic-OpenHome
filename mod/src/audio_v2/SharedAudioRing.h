#pragma once

// Cross-process single-producer / single-consumer PCM ring.
//
// The hook process (AMPLibraryAgent) writes canonical Q31 interleaved int32
// frames; am-exclusive-audiohost.exe reads them and feeds the real endpoint.
// Both sides map the same named section, so the layout below must stay POD and
// identical in both translation units. Atomics are lock-free on x64 and live in
// the shared pages, which is what makes the indices safe across processes.

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ammod::audio_v2 {

inline constexpr std::uint32_t kSharedRingMagic = 0x31454D41; // 'AME1'
inline constexpr std::uint32_t kSharedRingVersion = 2;
inline constexpr std::uint32_t kSharedRingChannels = 2;
// v0.3.5: the ring is no longer capped at ~10 s. A 512 MiB section holds
// 67,108,800 frames = ~349 s (5.8 min) at 192 kHz / 2 ch / 32-bit Q31.
// The logical depth is chosen per format by the host (default 120 s), and is
// always clamped to both this ceiling and the mapped section's real capacity.
inline constexpr std::uint32_t kSharedRingMaxFrames = 67108800u;
// Ordinary-page fallback still needs to clear one minute at 192 kHz:
// 128 MiB = ~87 s. Keeping it smaller than the large-page pool avoids a
// 512 MiB working-set floor / VirtualLock request on the fallback path.
inline constexpr std::size_t kSharedRingFallbackBytes = 128ull << 20;

struct SharedRingHeader final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t capacityFrames{};
    std::uint32_t channels{};
    std::uint32_t sampleRate{};
    std::uint32_t validBits{};
    std::uint32_t containerBits{};
    std::uint32_t encoding{};
    std::uint32_t endOfStream{};
    std::uint32_t hostState{};   // 0 closed, 1 open, 2 running
    alignas(8) std::atomic<std::uint64_t> writeFrame{};
    alignas(8) std::atomic<std::uint64_t> readFrame{};
    alignas(8) std::atomic<std::uint64_t> generation{};
    alignas(8) std::atomic<std::uint64_t> producedFrames{};
    alignas(8) std::atomic<std::uint64_t> droppedFrames{};
    alignas(8) std::atomic<std::uint64_t> underrunFrames{};
    // v2: host -> hook completion signal. The host stamps the generation of
    // the sink that actually finished its device-clock drain; the hook only
    // accepts a stamp matching the generation it opened. Without this the
    // Remote sink's EndOfStreamComplete() was a dead flag and every track had
    // to be retired by the runtime's timeout instead of real drain evidence.
    alignas(8) std::atomic<std::uint64_t> eosCompleteGeneration{};
    // A never-used generation is 0, so a freshly reset ring cannot look like
    // a completed one. Generations handed out by the hook start at 1.
    inline static constexpr std::uint64_t kNoGeneration = 0;
};

inline constexpr std::size_t kSharedRingHeaderBytes = sizeof(SharedRingHeader);
inline constexpr std::size_t kSharedRingBytes =
    sizeof(SharedRingHeader) +
    static_cast<std::size_t>(kSharedRingMaxFrames) * kSharedRingChannels * sizeof(std::int32_t);

inline std::int32_t* SharedRingData(void* base) noexcept {
    return reinterpret_cast<std::int32_t*>(
        static_cast<std::uint8_t*>(base) + sizeof(SharedRingHeader));
}

inline void SharedRingReset(SharedRingHeader* header, std::uint32_t sampleRate,
                            std::uint32_t validBits, std::uint32_t containerBits,
                            std::uint32_t encoding, std::uint64_t generation,
                            std::uint32_t capacityFrames) noexcept {
    header->capacityFrames = capacityFrames;
    header->channels = kSharedRingChannels;
    header->sampleRate = sampleRate;
    header->validBits = validBits;
    header->containerBits = containerBits;
    header->encoding = encoding;
    header->endOfStream = 0;
    header->hostState = 1;
    header->writeFrame.store(0, std::memory_order_relaxed);
    header->readFrame.store(0, std::memory_order_relaxed);
    header->generation.store(generation, std::memory_order_release);
    header->producedFrames.store(0, std::memory_order_relaxed);
    header->droppedFrames.store(0, std::memory_order_relaxed);
    header->underrunFrames.store(0, std::memory_order_relaxed);
    // Retire any completion stamp left by the previous track before the new
    // generation becomes visible to the hook.
    header->eosCompleteGeneration.store(SharedRingHeader::kNoGeneration,
                                        std::memory_order_release);
}

// Producer side (hook). Never blocks the caller for long: the pump thread may
// wait on the "space available" event, but the AM decoder callback never does.
class SharedRingProducer final {
public:
    bool Attach(void* base, HANDLE spaceEvent) noexcept {
        if (!base) return false;
        header_ = static_cast<SharedRingHeader*>(base);
        if (header_->magic != kSharedRingMagic || header_->version != kSharedRingVersion) {
            header_ = nullptr;
            return false;
        }
        data_ = SharedRingData(base);
        spaceEvent_ = spaceEvent;
        return true;
    }

    bool Attached() const noexcept { return header_ != nullptr; }

    std::uint64_t BufferedFrames() const noexcept {
        if (!header_) return 0;
        const auto write = header_->writeFrame.load(std::memory_order_acquire);
        const auto read = header_->readFrame.load(std::memory_order_acquire);
        return write >= read ? write - read : 0;
    }

    std::uint64_t FreeFrames() const noexcept {
        if (!header_) return 0;
        const auto buffered = BufferedFrames();
        return header_->capacityFrames > buffered ? header_->capacityFrames - buffered : 0;
    }

    // Frames the engine has already drained into the endpoint. In remote mode
    // this is the closest thing the hook has to "submitted" telemetry.
    std::uint64_t ConsumedFrames() const noexcept {
        return header_ ? header_->readFrame.load(std::memory_order_acquire) : 0;
    }

    // Writes up to frames; returns how many were copied. When the ring is
    // full and waitMs is non-zero, waits for the consumer's space event.
    std::uint32_t Write(const std::int32_t* source, std::uint32_t frames,
                        DWORD waitMs) noexcept {
        if (!header_ || !source || frames == 0 || !data_) return 0;
        const std::uint32_t capacity = header_->capacityFrames;
        if (capacity == 0) return 0;
        if (FreeFrames() < frames) {
            if (waitMs != 0 && spaceEvent_) {
                (void)WaitForSingleObject(spaceEvent_, waitMs);
            }
            if (FreeFrames() < frames) return 0;
        }
        const std::uint64_t write = header_->writeFrame.load(std::memory_order_relaxed);
        std::uint64_t offset = write % capacity;
        const std::uint32_t first = static_cast<std::uint32_t>(
            offset + frames <= capacity ? frames : capacity - offset);
        const std::size_t stride = kSharedRingChannels;
        std::memcpy(data_ + offset * stride, source,
                    static_cast<std::size_t>(first) * stride * sizeof(std::int32_t));
        if (first < frames) {
            std::memcpy(data_, source + static_cast<std::size_t>(first) * stride,
                        static_cast<std::size_t>(frames - first) * stride * sizeof(std::int32_t));
        }
        header_->writeFrame.store(write + frames, std::memory_order_release);
        header_->producedFrames.fetch_add(frames, std::memory_order_relaxed);
        return frames;
    }

    void MarkEndOfStream() noexcept {
        if (header_) header_->endOfStream = 1;
    }

    // True only when the host has confirmed drain of this exact generation.
    bool HostEosComplete(std::uint64_t generation) const noexcept {
        if (!header_ || generation == SharedRingHeader::kNoGeneration) return false;
        return header_->eosCompleteGeneration.load(std::memory_order_acquire) == generation;
    }

    void MarkHostState(std::uint32_t value) noexcept {
        if (header_) header_->hostState = value;
    }

private:
    SharedRingHeader* header_{};
    std::int32_t* data_{};
    HANDLE spaceEvent_{};
};

// Consumer side (audiohost). Called from the WASAPI render thread or the ASIO
// bufferSwitch callback, so it never blocks and never allocates.
class SharedRingConsumer final {
public:
    bool Attach(void* base, HANDLE spaceEvent) noexcept {
        if (!base) return false;
        header_ = static_cast<SharedRingHeader*>(base);
        if (header_->magic != kSharedRingMagic || header_->version != kSharedRingVersion) {
            header_ = nullptr;
            return false;
        }
        data_ = SharedRingData(base);
        spaceEvent_ = spaceEvent;
        return true;
    }

    bool Attached() const noexcept { return header_ != nullptr; }
    const SharedRingHeader* Header() const noexcept { return header_; }

    std::uint32_t Available() const noexcept {
        if (!header_) return 0;
        const auto write = header_->writeFrame.load(std::memory_order_acquire);
        const auto read = header_->readFrame.load(std::memory_order_relaxed);
        return write >= read ? static_cast<std::uint32_t>(write - read) : 0;
    }

    bool EndOfStream() const noexcept { return header_ && header_->endOfStream != 0; }

    // Called by the audiohost's EOS monitor thread once the real sink reports
    // completion. Stamping generation (rather than a bool) prevents a late
    // completion from the previous track from retiring the next one.
    void MarkEosComplete() noexcept {
        if (!header_) return;
        const auto generation = header_->generation.load(std::memory_order_acquire);
        if (generation == SharedRingHeader::kNoGeneration) return;
        header_->eosCompleteGeneration.store(generation, std::memory_order_release);
    }

    std::uint32_t Read(std::int32_t* destination, std::uint32_t frames) noexcept {
        if (!header_ || !destination || frames == 0 || !data_) return 0;
        const std::uint32_t capacity = header_->capacityFrames;
        if (capacity == 0) return 0;
        const std::uint32_t available = Available();
        const std::uint32_t count = available < frames ? available : frames;
        if (count == 0) return 0;
        const std::uint64_t read = header_->readFrame.load(std::memory_order_relaxed);
        std::uint64_t offset = read % capacity;
        const std::uint32_t first = static_cast<std::uint32_t>(
            offset + count <= capacity ? count : capacity - offset);
        const std::size_t stride = kSharedRingChannels;
        std::memcpy(destination, data_ + offset * stride,
                    static_cast<std::size_t>(first) * stride * sizeof(std::int32_t));
        if (first < count) {
            std::memcpy(destination + static_cast<std::size_t>(first) * stride, data_,
                        static_cast<std::size_t>(count - first) * stride * sizeof(std::int32_t));
        }
        header_->readFrame.store(read + count, std::memory_order_release);
        if (spaceEvent_) SetEvent(spaceEvent_);
        return count;
    }

    void MarkHostState(std::uint32_t value) noexcept {
        if (header_) header_->hostState = value;
    }

private:
    SharedRingHeader* header_{};
    std::int32_t* data_{};
    HANDLE spaceEvent_{};
};

} // namespace ammod::audio_v2
