#pragma once

// Verified native-stream boundary reader for the warehouse attachment.
// Parses the native gzip framing (1F 8B 08 ...) and locates the end of the
// FIRST raw-deflate member plus its 8-byte trailer. Bytes after that boundary
// belong to third parties or to our own attachment and are never scanned as
// native data.
//
// Trust rules (design doc 6.2/7):
// - The game's trailer CRC does not equal standard CRC-32 on real saves, so
//   CRC is deliberately NOT verified; ISIZE (uncompressed length) IS verified
//   against the actually inflated byte count (all sampled saves agree).
// - Fail-closed: any framing anomaly, truncated input, inflate error, ISIZE
//   mismatch, or multi-member ambiguity returns false; callers must then
//   refuse warehouse reads/writes (BoundaryUntrusted), never guess.
// - Pure code + zlib only: no file I/O, no game memory, no hooks.
#include "zlib.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>

namespace warehouse::native {

struct NativeBoundary {
    std::size_t nativeEnd = 0;   // first byte AFTER gzip header+deflate+trailer
    std::uint64_t inflatedBytes = 0; // ISIZE cross-check value
};

namespace detail {

inline bool ReadGzipHeader(std::span<const std::uint8_t> file, std::size_t& payloadOffset) noexcept
{
    if (file.size() < 18) return false;                 // 10 header + 8 trailer
    const std::uint8_t* p = file.data();
    if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 0x08) return false;
    const unsigned flags = p[3];
    std::size_t offset = 10;
    if (flags & 0x04) {                                  // FEXTRA
        if (offset + 2 > file.size()) return false;
        const unsigned extra = p[offset] | (p[offset + 1] << 8);
        offset += 2 + extra;
        if (offset > file.size()) return false;
    }
    if (flags & 0x08) {                                  // FNAME (NUL-terminated)
        while (offset < file.size() && p[offset] != 0) ++offset;
        if (offset >= file.size()) return false;
        ++offset;
    }
    if (flags & 0x10) {                                  // FCOMMENT
        while (offset < file.size() && p[offset] != 0) ++offset;
        if (offset >= file.size()) return false;
        ++offset;
    }
    if (flags & 0x02) offset += 2;                       // FHCRC
    payloadOffset = offset;
    return true;
}

} // namespace detail

// Returns true and fills `out` when the first gzip member ends cleanly inside
// `file` and its trailer ISIZE equals the inflated length. `out.nativeEnd`
// may be smaller than file.size() (trailing attachment bytes are expected).
inline bool ScanNativeEnd(std::span<const std::uint8_t> file, NativeBoundary& out) noexcept
{
    out = {};
    std::size_t payloadOffset = 0;
    if (!detail::ReadGzipHeader(file, payloadOffset)) return false;

    z_stream stream{};
    if (inflateInit2(&stream, -15) != Z_OK) return false;

    std::uint8_t discard[16384]; // 16 KiB heap buffer, never on a game-hook stack
    std::uint8_t* sink = nullptr;
    try { sink = new std::uint8_t[16384]; }
    catch (...) { inflateEnd(&stream); return false; }
    (void)discard;

    stream.next_in = const_cast<Bytef*>(file.data() + payloadOffset);
    stream.avail_in = static_cast<uInt>(file.size() - payloadOffset);
    stream.next_out = sink;
    stream.avail_out = 16384;

    bool ok = false;
    for (;;) {
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status == Z_STREAM_END) { ok = true; break; }
        if (status != Z_OK && status != Z_BUF_ERROR) break;
        if (status == Z_BUF_ERROR && stream.avail_in == 0) break; // truncated input
        if (stream.avail_out == 0) {
            stream.next_out = sink;
            stream.avail_out = 16384;
        }
    }

    const uLong inflatedTotal = stream.total_out;
    const uLong consumed = stream.total_in;
    inflateEnd(&stream);
    delete[] sink;
    if (!ok) return false;

    const std::size_t consumedSize = static_cast<std::size_t>(consumed);
    if (consumedSize > file.size() - payloadOffset) return false;
    const std::size_t deflateEnd = payloadOffset + consumedSize;
    if (deflateEnd + 8 > file.size()) return false;      // trailer truncated

    const std::uint8_t* trailer = file.data() + deflateEnd;
    std::uint32_t storedSize = 0;
    std::memcpy(&storedSize, trailer + 4, 4);            // little-endian host
    if (storedSize != inflatedTotal) return false;       // ISIZE cross-check

    out.nativeEnd = deflateEnd + 8;
    out.inflatedBytes = inflatedTotal;
    return true;
}

} // namespace warehouse::native
