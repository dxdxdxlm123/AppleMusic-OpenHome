#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ammod::metadata {

// A single bounded RIFF "id3 " chunk, not a complete WAV. No file/network I/O.
inline constexpr std::size_t kMaxWavId3ChunkBytes = 64u * 1024u;
inline constexpr std::size_t kMaxWavId3TextBytes = 16u * 1024u;

namespace wav_id3_detail {
inline void Le32(std::vector<BYTE>& out, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) out.push_back(static_cast<BYTE>(value >> (i * 8)));
}
inline std::uint32_t ReadLe32(const BYTE* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
inline std::uint32_t ReadBe32(const BYTE* p) noexcept {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
        (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
inline bool ValidUtf16(const BYTE* data, std::size_t size) noexcept {
    if ((size & 1u) != 0 || size > kMaxWavId3TextBytes) return false;
    for (std::size_t i = 0; i < size; i += 2) {
        const auto unit = std::uint16_t(data[i]) | (std::uint16_t(data[i + 1]) << 8);
        if (unit == 0) return false; // no embedded text-list separator
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (i + 3 >= size) return false;
            const auto low = std::uint16_t(data[i + 2]) | (std::uint16_t(data[i + 3]) << 8);
            if (low < 0xdc00 || low > 0xdfff) return false;
            i += 2;
        } else if (unit >= 0xdc00 && unit <= 0xdfff) {
            return false;
        }
    }
    return true;
}
inline bool EncodeText(const std::wstring& text, std::vector<BYTE>& bytes) {
    // This precheck bounds allocations even when wchar_t is wider than UTF-16.
    if (text.size() > kMaxWavId3TextBytes / 2u) return false;
    for (wchar_t ch : text) {
        const auto cp = static_cast<std::uint32_t>(ch);
        if (cp == 0 || cp > 0x10ffffu) return false;
        if (cp > 0xffffu) {
            const auto value = cp - 0x10000u;
            const auto high = 0xd800u + (value >> 10);
            const auto low = 0xdc00u + (value & 0x3ffu);
            bytes.push_back(static_cast<BYTE>(high)); bytes.push_back(static_cast<BYTE>(high >> 8));
            bytes.push_back(static_cast<BYTE>(low)); bytes.push_back(static_cast<BYTE>(low >> 8));
        } else {
            bytes.push_back(static_cast<BYTE>(cp)); bytes.push_back(static_cast<BYTE>(cp >> 8));
        }
        if (bytes.size() > kMaxWavId3TextBytes) return false;
    }
    return ValidUtf16(bytes.data(), bytes.size());
}
} // namespace wav_id3_detail

// Strictly accept only the builder's small ID3v2.3 text vocabulary. A caller
// cannot inject another RIFF/data chunk, inconsistent sizes, or arbitrary tags.
inline bool IsWavId3Chunk(const std::vector<BYTE>& chunk) noexcept {
    using namespace wav_id3_detail;
    if (chunk.size() < 18 || chunk.size() > kMaxWavId3ChunkBytes ||
        chunk[0] != 'i' || chunk[1] != 'd' || chunk[2] != '3' || chunk[3] != ' ') return false;
    const std::size_t size = ReadLe32(chunk.data() + 4);
    if (size < 10 || size > chunk.size() - 8 ||
        size + 8 + (size & 1u) != chunk.size() || ((size & 1u) && chunk.back() != 0)) return false;
    const BYTE* tag = chunk.data() + 8;
    if (tag[0] != 'I' || tag[1] != 'D' || tag[2] != '3' || tag[3] != 3 ||
        tag[4] != 0 || tag[5] != 0) return false;
    std::uint32_t framesSize = 0;
    for (unsigned i = 6; i != 10; ++i) {
        if (tag[i] >= 128) return false;
        framesSize = (framesSize << 7) | tag[i];
    }
    if (std::size_t(framesSize) + 10 != size) return false;
    unsigned seen = 0;
    std::size_t position = 10;
    while (position < size) {
        if (size - position < 10) return false;
        const BYTE* frame = tag + position;
        unsigned bit = 0;
        if (frame[0] == 'T' && frame[1] == 'I' && frame[2] == 'T' && frame[3] == '2') bit = 1;
        if (frame[0] == 'T' && frame[1] == 'P' && frame[2] == 'E' && frame[3] == '1') bit = 2;
        if (frame[0] == 'T' && frame[1] == 'A' && frame[2] == 'L' && frame[3] == 'B') bit = 4;
        if (bit == 0 || (seen & bit) || frame[8] != 0 || frame[9] != 0) return false;
        seen |= bit;
        const std::size_t payload = ReadBe32(frame + 4);
        if (payload < 5 || payload > size - position - 10 || frame[10] != 1 ||
            frame[11] != 0xff || frame[12] != 0xfe ||
            !ValidUtf16(frame + 13, payload - 3)) return false;
        position += 10 + payload;
    }
    return seen != 0 && position == size;
}

// Empty input yields no chunk. Invalid Unicode, embedded NUL, or any text over
// 16 KiB UTF-16 bytes rejects the whole snapshot, never a partial/truncated tag.
// Allocation failures propagate to the caller like other vector/string APIs.
inline std::vector<BYTE> BuildWavId3Chunk(const std::wstring& title,
                                         const std::wstring& artist,
                                         const std::wstring& album) {
    using namespace wav_id3_detail;
    const std::array<const std::wstring*, 3> values = {&title, &artist, &album};
    const std::array<const char*, 3> ids = {"TIT2", "TPE1", "TALB"};
    std::vector<BYTE> frames;
    for (std::size_t i = 0; i != values.size(); ++i) {
        if (values[i]->empty()) continue;
        std::vector<BYTE> text;
        if (!EncodeText(*values[i], text)) return {};
        const auto size = static_cast<std::uint32_t>(3 + text.size());
        frames.insert(frames.end(), ids[i], ids[i] + 4);
        for (int shift = 24; shift >= 0; shift -= 8) frames.push_back(static_cast<BYTE>(size >> shift));
        frames.insert(frames.end(), {0, 0, 1, 0xff, 0xfe}); // frame flags, encoding, BOM
        frames.insert(frames.end(), text.begin(), text.end());
    }
    if (frames.empty()) return {};
    const auto size = static_cast<std::uint32_t>(frames.size());
    std::vector<BYTE> chunk = {'i', 'd', '3', ' '};
    Le32(chunk, size + 10);
    chunk.insert(chunk.end(), {'I', 'D', '3', 3, 0, 0});
    for (int shift = 21; shift >= 0; shift -= 7) chunk.push_back(static_cast<BYTE>((size >> shift) & 0x7f));
    chunk.insert(chunk.end(), frames.begin(), frames.end());
    if ((chunk.size() & 1u) != 0) chunk.push_back(0);
    return IsWavId3Chunk(chunk) ? chunk : std::vector<BYTE>{};
}

} // namespace ammod::metadata
