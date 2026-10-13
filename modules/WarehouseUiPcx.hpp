// 24-bit PCX decoder shared by warehouse UI production code and offline tests.
#ifndef H3RNDNEW_WAREHOUSE_UI_PCX_HPP_
#define H3RNDNEW_WAREHOUSE_UI_PCX_HPP_

#include <cstddef>
#include <vector>

namespace warehouseui {

// Supports PCX v5, 8 bits per plane, three RGB planes, and RLE only.
// Each plane scanline is decoded to bytesPerLine (including padding), then
// visible pixels are interleaved into top-down RGB output. Output is committed
// only after all input has decoded; failure leaves rgb unchanged.
inline bool DecodePcxRgb(const unsigned char* data, std::size_t size,
                         int expectedW, int expectedH,
                         std::vector<unsigned char>& rgb) noexcept
{
    constexpr std::size_t kHeaderSize = 128;
    constexpr unsigned kMaxDimension = 4096;
    constexpr unsigned kMaxBytesPerLine = 8192;
    if (!data || size < kHeaderSize || expectedW <= 0 || expectedH <= 0)
        return false;
    if (data[0] != 0x0A || data[1] != 5 || data[2] != 1
        || data[3] != 8 || data[65] != 3)
        return false;

    const auto read16 = [data](std::size_t offset) noexcept -> unsigned {
        return static_cast<unsigned>(data[offset])
            | (static_cast<unsigned>(data[offset + 1]) << 8);
    };
    const unsigned xmin = read16(4), ymin = read16(6);
    const unsigned xmax = read16(8), ymax = read16(10);
    if (xmax < xmin || ymax < ymin) return false;
    const unsigned width = xmax - xmin + 1;
    const unsigned height = ymax - ymin + 1;
    const unsigned bytesPerLine = read16(66);
    if (width > kMaxDimension || height > kMaxDimension
        || width != static_cast<unsigned>(expectedW)
        || height != static_cast<unsigned>(expectedH)
        || bytesPerLine < width || bytesPerLine > kMaxBytesPerLine)
        return false;

    const std::size_t outputRowSize = static_cast<std::size_t>(width) * 3;
    const std::size_t planeRowSize = static_cast<std::size_t>(bytesPerLine);
    try {
        std::vector<unsigned char> decoded(outputRowSize * height);
        std::vector<unsigned char> planeRow(planeRowSize);
        std::size_t offset = kHeaderSize;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned plane = 0; plane < 3; ++plane) {
                std::size_t written = 0;
                while (written < planeRowSize) {
                    if (offset >= size) return false;
                    unsigned char value = data[offset++];
                    std::size_t count = 1;
                    if ((value & 0xC0u) == 0xC0u) {
                        count = value & 0x3Fu;
                        if (count == 0 || offset >= size) return false;
                        value = data[offset++];
                    }
                    if (count > planeRowSize - written) return false;
                    for (std::size_t i = 0; i < count; ++i)
                        planeRow[written++] = value;
                }
                const std::size_t outputBase = static_cast<std::size_t>(y) * outputRowSize;
                for (unsigned x = 0; x < width; ++x) {
                    decoded[outputBase + static_cast<std::size_t>(x) * 3 + plane]
                        = planeRow[x];
                }
            }
        }
        rgb.swap(decoded);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace warehouseui

#endif // H3RNDNEW_WAREHOUSE_UI_PCX_HPP_
