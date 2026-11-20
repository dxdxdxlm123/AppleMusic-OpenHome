#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ammod::metadata {

struct TrackSnapshot {
    std::uint32_t durationMs{};
    std::wstring title, artist, album;
    std::uint32_t trackNumber{};
    std::wstring artworkMime;
    std::vector<std::uint8_t> artwork;
};

inline constexpr std::size_t kMaxTrackTextBytes = 16 * 1024;
inline constexpr std::size_t kMaxArtworkBytes = 2 * 1024 * 1024;
// Excludes the transport newline. Four UTF-8 strings become hex, as does art.
inline constexpr std::size_t kMaxTrackLineBytes =
    6 + 7 + 20 + 8 * kMaxTrackTextBytes + 2 * kMaxArtworkBytes;

namespace detail {
inline bool AllowedCodepoint(std::uint32_t cp) {
    // XML 1.0 whitespace is safe after hex encoding; other controls are not.
    return cp == 9 || cp == 10 || cp == 13 ||
        (cp >= 0x20 && cp <= 0x10ffff && !(cp >= 0x7f && cp <= 0x9f) &&
         !(cp >= 0xd800 && cp <= 0xdfff) && (cp & 0xffff) != 0xfffe &&
         (cp & 0xffff) != 0xffff);
}

inline bool ToUtf8(const std::wstring& wide, std::string& out) {
    if (wide.size() > kMaxTrackTextBytes) return false;
    out.clear();
    for (std::size_t i = 0; i < wide.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint32_t>(wide[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (++i == wide.size()) return false;
                const auto low = static_cast<std::uint32_t>(wide[i]);
                if (low < 0xdc00 || low > 0xdfff) return false;
                cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            }
        }
        if (!AllowedCodepoint(cp)) return false;
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        if (out.size() > kMaxTrackTextBytes) return false;
    }
    return true;
}

inline bool FromUtf8(const std::string& text, std::wstring& wide) {
    wide.clear();
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<std::uint8_t>(text[i++]);
        std::uint32_t cp = first;
        unsigned count = 0;
        std::uint32_t minimum = 0;
        if (first < 0x80) {}
        else if (first >= 0xc2 && first <= 0xdf) { cp &= 0x1f; count = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { cp &= 0x0f; count = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { cp &= 7; count = 3; minimum = 0x10000; }
        else return false;
        if (text.size() - i < count) return false;
        for (unsigned j = 0; j < count; ++j) {
            const auto next = static_cast<std::uint8_t>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || !AllowedCodepoint(cp)) return false;
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp > 0xffff) {
                cp -= 0x10000;
                wide.push_back(static_cast<wchar_t>(0xd800 + (cp >> 10)));
                wide.push_back(static_cast<wchar_t>(0xdc00 + (cp & 0x3ff)));
                continue;
            }
        }
        wide.push_back(static_cast<wchar_t>(cp));
    }
    return true;
}

inline void AppendHex(std::string& out, const std::uint8_t* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
}
inline int HexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}
inline bool DecodeHex(std::string_view hex, std::size_t maximum, std::vector<std::uint8_t>& out) {
    if (hex.size() % 2 || hex.size() / 2 > maximum) return false;
    out.resize(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = HexDigit(hex[2 * i]), low = HexDigit(hex[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}
inline bool ParseUnsigned(std::string_view text, std::uint32_t& value) {
    if (text.empty() || text.size() > 10) return false;
    value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint32_t>(ch - '0');
        if (value > ((std::numeric_limits<std::uint32_t>::max)() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}
} // namespace detail

// Sniff bytes, never a filename, URL, or provider-supplied MIME type.
inline std::wstring ArtworkMime(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > kMaxArtworkBytes) return {};
    constexpr std::uint8_t png[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() >= sizeof(png)) {
        bool matches = true;
        for (std::size_t i = 0; i < sizeof(png); ++i) matches = matches && bytes[i] == png[i];
        if (matches) return L"image/png";
    }
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
        return L"image/jpeg";
    return {};
}

// Narrow compatibility heuristic for the observed Apple Music 1.7 SMTC format:
// Artist = "artist names \u2014 album title", with AlbumTitle empty. Call only for
// AppleMusic sessions, never as a generic provider/parser normalization rule.
// Explicit albums, other dash characters, and ambiguous multiple separators win.
inline void NormalizeAppleMusicMetadata(TrackSnapshot& track) {
    if (!track.album.empty()) return;
    constexpr std::wstring_view separator = L" \u2014 ";
    const auto split = track.artist.find(separator);
    if (split == std::wstring::npos || split == 0 || split + separator.size() == track.artist.size() ||
        track.artist.find(separator, split + 1) != std::wstring::npos) return;
    const auto artist = track.artist.substr(0, split);
    const auto album = track.artist.substr(split + separator.size());
    // Whitespace-only halves do not constitute artist/album evidence.
    if (artist.find_first_not_of(L" \t\r\n") == std::wstring::npos ||
        album.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
    track.artist = artist;
    track.album = album;
}

// Artwork is an independently refreshed attribute, not track identity.
inline bool SameTrack(const TrackSnapshot& a, const TrackSnapshot& b) {
    return a.durationMs == b.durationMs && a.title == b.title && a.artist == b.artist &&
        a.album == b.album && a.trackNumber == b.trackNumber;
}

// Invalid snapshots serialize to an empty string (not a valid empty snapshot).
// The caller owns framing: append '\n'; ParseTrackLine expects no CR/LF framing.
inline std::string SerializeTrackLine(const TrackSnapshot& track) {
    if (track.artwork.size() > kMaxArtworkBytes ||
        (track.artwork.empty() ? !track.artworkMime.empty() :
         (ArtworkMime(track.artwork).empty() || ArtworkMime(track.artwork) != track.artworkMime))) return {};
    std::array<std::string, 4> text;
    if (!detail::ToUtf8(track.title, text[0]) || !detail::ToUtf8(track.artist, text[1]) ||
        !detail::ToUtf8(track.album, text[2]) || !detail::ToUtf8(track.artworkMime, text[3])) return {};
    std::string line = "TRACK2\t" + std::to_string(track.durationMs) + "\t" + std::to_string(track.trackNumber);
    for (const auto& field : text) {
        line.push_back('\t');
        detail::AppendHex(line, reinterpret_cast<const std::uint8_t*>(field.data()), field.size());
    }
    line.push_back('\t');
    detail::AppendHex(line, track.artwork.data(), track.artwork.size());
    return line;
}

// Transactional: an invalid line leaves the caller's previous value untouched.
inline bool ParseTrackLine(const std::string& line, TrackSnapshot& track) {
    if (line.size() > kMaxTrackLineBytes) return false;
    std::array<std::string_view, 8> fields;
    std::size_t start = 0;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto end = line.find('\t', start);
        if ((i + 1 == fields.size()) != (end == std::string::npos)) return false;
        fields[i] = std::string_view(line).substr(start, end == std::string::npos ? end : end - start);
        start = end == std::string::npos ? line.size() : end + 1;
    }
    if (fields[0] != "TRACK2") return false;
    TrackSnapshot parsed;
    if (!detail::ParseUnsigned(fields[1], parsed.durationMs) ||
        !detail::ParseUnsigned(fields[2], parsed.trackNumber)) return false;
    std::wstring* wide[] = {&parsed.title, &parsed.artist, &parsed.album, &parsed.artworkMime};
    std::vector<std::uint8_t> decoded;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!detail::DecodeHex(fields[3 + i], kMaxTrackTextBytes, decoded) ||
            !detail::FromUtf8(std::string(decoded.begin(), decoded.end()), *wide[i])) return false;
    }
    if (!detail::DecodeHex(fields[7], kMaxArtworkBytes, parsed.artwork)) return false;
    if (parsed.artwork.empty() ? !parsed.artworkMime.empty() :
        (ArtworkMime(parsed.artwork).empty() || ArtworkMime(parsed.artwork) != parsed.artworkMime)) return false;
    track = std::move(parsed);
    return true;
}

} // namespace ammod::metadata
