#include "../modules/WarehouseUiPcx.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_checks_ = 0;

void Check_(bool ok, const char* label)
{
    ++g_checks_;
    if (!ok) {
        std::fprintf(stderr, "FAIL warehouse UI PCX: %s\n", label);
        std::exit(1);
    }
}

void Put16_(std::vector<unsigned char>& bytes, std::size_t offset,
            unsigned value)
{
    bytes[offset] = static_cast<unsigned char>(value & 0xFFu);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
}

void AppendLiteral_(std::vector<unsigned char>& encoded, unsigned char value)
{
    if ((value & 0xC0u) == 0xC0u) {
        encoded.push_back(0xC1u);
        encoded.push_back(value);
    } else {
        encoded.push_back(value);
    }
}

void AppendRun_(std::vector<unsigned char>& encoded, std::size_t count,
                unsigned char value)
{
    while (count != 0) {
        const std::size_t part = count > 63 ? 63 : count;
        encoded.push_back(static_cast<unsigned char>(0xC0u | part));
        encoded.push_back(value);
        count -= part;
    }
}

std::vector<unsigned char> MakePcx_(int width, int height, int bytesPerLine,
                                    const std::vector<unsigned char>& encoded,
                                    unsigned planes = 3)
{
    std::vector<unsigned char> result(128, 0);
    result[0] = 0x0A; // manufacturer
    result[1] = 5;    // PCX version 3.0
    result[2] = 1;    // RLE encoding
    result[3] = 8;    // bits per pixel per plane
    Put16_(result, 4, 0);
    Put16_(result, 6, 0);
    Put16_(result, 8, static_cast<unsigned>(width - 1));
    Put16_(result, 10, static_cast<unsigned>(height - 1));
    result[64] = 0; // reserved
    result[65] = static_cast<unsigned char>(planes);
    Put16_(result, 66, static_cast<unsigned>(bytesPerLine));
    Put16_(result, 68, 1); // color planes are RGB
    result.insert(result.end(), encoded.begin(), encoded.end());
    return result;
}

std::vector<unsigned char> MakePaddedFixture_()
{
    // Two pixels, one row, four bytes in each plane: the final two bytes are
    // PCX scanline padding and must not leak into the RGB output.
    std::vector<unsigned char> encoded;
    const unsigned char values[] = { 0x11, 0xC2, 0x33, 0x44, 0x55, 0x66 };
    // R plane: direct literal 0x11, one-byte RLE literal 0xC2, then padding.
    AppendLiteral_(encoded, values[0]);
    AppendLiteral_(encoded, values[1]);
    AppendRun_(encoded, 2, 0);
    // G/B planes have both literal bytes and two-byte RLE packets.
    AppendLiteral_(encoded, values[2]);
    AppendLiteral_(encoded, values[3]);
    AppendRun_(encoded, 2, 0);
    AppendLiteral_(encoded, values[4]);
    AppendLiteral_(encoded, values[5]);
    AppendRun_(encoded, 2, 0);
    return MakePcx_(2, 1, 4, encoded);
}

std::vector<unsigned char> MakeOnePixelFixture_()
{
    std::vector<unsigned char> encoded;
    AppendLiteral_(encoded, 0x12);
    AppendLiteral_(encoded, 0x34);
    AppendLiteral_(encoded, 0x56);
    return MakePcx_(1, 1, 1, encoded);
}

void CheckFixtureDecoding_()
{
    const std::vector<unsigned char> fixture = MakePaddedFixture_();
    std::vector<unsigned char> rgb;
    Check_(warehouseui::DecodePcxRgb(fixture.data(), fixture.size(), 2, 1, rgb),
          "literal and two-byte RLE decode");
    const std::vector<unsigned char> expected = {
        0x11, 0x33, 0x55, 0xC2, 0x44, 0x66
    };
    Check_(rgb == expected, "RGB order and padding are correct");

    const std::vector<unsigned char> one = MakeOnePixelFixture_();
    Check_(warehouseui::DecodePcxRgb(one.data(), one.size(), 1, 1, rgb),
          "one-pixel fixture decodes");
    Check_(rgb == std::vector<unsigned char>({0x12, 0x34, 0x56}),
          "one-pixel RGB values preserved");

    std::vector<unsigned char> twoRows;
    for (unsigned char base : {static_cast<unsigned char>(10),
                               static_cast<unsigned char>(40)}) {
        for (unsigned char plane = 0; plane < 3; ++plane) {
            AppendLiteral_(twoRows, static_cast<unsigned char>(base + plane));
            AppendLiteral_(twoRows, static_cast<unsigned char>(base + plane + 3));
            AppendRun_(twoRows, 2, 0xEE); // independent padding per plane/row
        }
    }
    const auto rows = MakePcx_(2, 2, 4, twoRows);
    Check_(warehouseui::DecodePcxRgb(rows.data(), rows.size(), 2, 2, rgb),
           "two padded rows decode");
    Check_(rgb == std::vector<unsigned char>({10, 11, 12, 13, 14, 15,
                                             40, 41, 42, 43, 44, 45}),
           "RGB planes and rows never bleed into one another");

    std::vector<unsigned char> runs;
    for (unsigned char value : {static_cast<unsigned char>(0),
                                static_cast<unsigned char>(0xC0),
                                static_cast<unsigned char>(0xFF)})
        AppendRun_(runs, 64, value);
    const auto longRun = MakePcx_(64, 1, 64, runs);
    Check_(warehouseui::DecodePcxRgb(longRun.data(), longRun.size(), 64, 1, rgb),
           "maximum 63-byte packet followed by a one-byte packet decodes");
    bool allPixelsMatch = rgb.size() == 192;
    for (std::size_t i = 0; i < rgb.size(); i += 3)
        allPixelsMatch = allPixelsMatch && rgb[i] == 0
            && rgb[i + 1] == 0xC0 && rgb[i + 2] == 0xFF;
    Check_(allPixelsMatch, "compressed data smaller than decoded size is valid");

    auto shiftedOrigin = one;
    Put16_(shiftedOrigin, 4, 12); Put16_(shiftedOrigin, 8, 12);
    Put16_(shiftedOrigin, 6, 34); Put16_(shiftedOrigin, 10, 34);
    Check_(warehouseui::DecodePcxRgb(shiftedOrigin.data(), shiftedOrigin.size(),
                                     1, 1, rgb), "nonzero header origin decodes");
}

void CheckFailureFixtures_()
{
    const std::vector<unsigned char> good = MakePaddedFixture_();
    const std::vector<unsigned char> sentinel = { 0xA1, 0xB2, 0xC3 };
    std::vector<unsigned char> rgb = sentinel;
    const auto RejectUnchanged_ = [&](const std::vector<unsigned char>& bytes,
                                      int width, int height,
                                      const char* label) {
        rgb = sentinel;
        Check_(!warehouseui::DecodePcxRgb(bytes.data(), bytes.size(), width,
                                         height, rgb), label);
        Check_(rgb == sentinel, "failure leaves output unchanged");
    };

    RejectUnchanged_(good, 3, 1, "dimension mismatch rejected");
    RejectUnchanged_(good, 2, 2, "height mismatch rejected");
    RejectUnchanged_(good, 4097, 1, "dimension upper bound rejected");
    RejectUnchanged_(std::vector<unsigned char>(), 2, 1,
                     "empty input rejected");

    std::vector<unsigned char> badHeader = good;
    badHeader[0] = 0;
    RejectUnchanged_(badHeader, 2, 1, "bad manufacturer rejected");
    badHeader = good;
    badHeader[65] = 2;
    RejectUnchanged_(badHeader, 2, 1, "non-RGB plane count rejected");
    badHeader = good;
    badHeader[3] = 4;
    RejectUnchanged_(badHeader, 2, 1, "non-8-bit plane rejected");

    std::vector<unsigned char> zeroRun(128, 0);
    zeroRun[0] = 0x0A; zeroRun[1] = 5; zeroRun[2] = 1; zeroRun[3] = 8;
    Put16_(zeroRun, 8, 0); Put16_(zeroRun, 10, 0);
    zeroRun[65] = 3; Put16_(zeroRun, 66, 1);
    zeroRun.push_back(0xC0); // RLE count zero is invalid, not a literal.
    RejectUnchanged_(zeroRun, 1, 1, "zero-length RLE rejected");

    std::vector<unsigned char> overflow = MakeOnePixelFixture_();
    overflow.resize(128);
    overflow.push_back(0xC2); overflow.push_back(0x12);
    RejectUnchanged_(overflow, 1, 1, "RLE row overflow rejected");

    std::vector<unsigned char> truncated = MakeOnePixelFixture_();
    truncated.pop_back();
    RejectUnchanged_(truncated, 1, 1, "truncated scanline rejected");

    std::vector<unsigned char> truncatedRun(128, 0);
    truncatedRun[0] = 0x0A; truncatedRun[1] = 5; truncatedRun[2] = 1;
    truncatedRun[3] = 8; truncatedRun[65] = 3; Put16_(truncatedRun, 66, 1);
    truncatedRun.push_back(0xC1); // count present, repeated value absent.
    RejectUnchanged_(truncatedRun, 1, 1, "truncated RLE pair rejected");

    std::vector<unsigned char> badLine = MakePaddedFixture_();
    Put16_(badLine, 66, 1); // bytesPerLine smaller than visible width.
    RejectUnchanged_(badLine, 2, 1, "short scanline padding rejected");
    badLine = good;
    Put16_(badLine, 66, 0);
    RejectUnchanged_(badLine, 2, 1, "zero bytesPerLine rejected");
    Put16_(badLine, 66, 65535);
    RejectUnchanged_(badLine, 2, 1, "oversized bytesPerLine rejected");

    auto dimensions = good;
    Put16_(dimensions, 8, 4096);
    RejectUnchanged_(dimensions, 4097, 1, "header width cap rejected");
    dimensions = good;
    Put16_(dimensions, 10, 4096);
    RejectUnchanged_(dimensions, 2, 4097, "header height cap rejected");
    dimensions = good;
    Put16_(dimensions, 8, 65535); Put16_(dimensions, 10, 65535);
    RejectUnchanged_(dimensions, 65536, 65536, "huge dimensions reject before multiplication");
    dimensions = good;
    Put16_(dimensions, 4, 2); Put16_(dimensions, 8, 1);
    RejectUnchanged_(dimensions, 2, 1, "inverted bounds rejected");

    const std::vector<unsigned char> shortInput(127, 0);
    RejectUnchanged_(shortInput, 1, 1, "input below 128-byte PCX header rejected");
    for (std::size_t length = 128; length < good.size(); ++length) {
        const std::vector<unsigned char> prefix(good.begin(), good.begin() + length);
        RejectUnchanged_(prefix, 2, 1, "every truncated payload rejected");
    }
    for (unsigned char first : {static_cast<unsigned char>(0xC0),
                                static_cast<unsigned char>(0xC5),
                                static_cast<unsigned char>(0xFF)}) {
        auto broken = good;
        broken[128] = first;
        RejectUnchanged_(broken, 2, 1, "zero or overflowing first packet rejected");
    }
    for (std::size_t field : {std::size_t(1), std::size_t(2)}) {
        auto unsupported = good;
        unsupported[field] = 0;
        RejectUnchanged_(unsupported, 2, 1, "unsupported PCX version or encoding rejected");
    }
    RejectUnchanged_(good, 0, 1, "zero expected width rejected");
    RejectUnchanged_(good, 2, -1, "negative expected height rejected");
    rgb = sentinel;
    Check_(!warehouseui::DecodePcxRgb(nullptr, good.size(), 2, 1, rgb)
               && rgb == sentinel, "null pointer rejects without changing output");
}

bool ReadFile_(const std::string& path, std::vector<unsigned char>& bytes)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) return false;
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (bytes.empty()) return false;
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file.good() || file.eof();
}

void CheckRealAssets_()
{
    std::vector<std::string> names;
    const char* buttons[] = { "open", "close", "take", "put" };
    for (const char* button : buttons) {
        for (int state = 0; state < 4; ++state)
            names.push_back(std::string("btn_") + button
                + static_cast<char>('0' + state) + ".pcx");
    }
    for (int tab = 0; tab < 12; ++tab) {
        char name[32];
        std::snprintf(name, sizeof(name), "tab%02dn.pcx", tab);
        names.push_back(name);
        std::snprintf(name, sizeof(name), "tab%02ds.pcx", tab);
        names.push_back(name);
    }
    names.push_back("arrow_prev0.pcx");
    names.push_back("arrow_prev1.pcx");
    names.push_back("arrow_next0.pcx");
    names.push_back("arrow_next1.pcx");
    names.push_back("pager_n.pcx");
    names.push_back("pager_s.pcx");
    names.push_back("tidy_n.pcx");
    names.push_back("tidy_s.pcx");
    Check_(names.size() == 48, "48 warehouse PCX names listed");

    std::size_t nonEmpty = 0;
    for (const std::string& name : names) {
        std::vector<unsigned char> file;
        Check_(ReadFile_(std::string("whui/") + name, file), "read warehouse PCX");
        int width = 0;
        int height = 0;
        if (name.find("btn_close") == 0) {
            width = 24; height = 24;
        } else if (name.find("btn_") == 0) {
            width = 66; height = 32;
        } else if (name.find("tab") == 0) {
            // round-9：页签收窄到 36x22（两字标题），与页码/整理按钮同宽。
            width = 36; height = 22;
        } else if (name.find("pager_") == 0) {
            width = 36; height = 22;
        } else if (name.find("tidy_") == 0) {
            width = 36; height = 22;
        } else {
            width = 18; height = 22;
        }
        std::vector<unsigned char> rgb;
        Check_(warehouseui::DecodePcxRgb(file.data(), file.size(), width, height, rgb),
              "decode warehouse PCX");
        Check_(rgb.size() == static_cast<std::size_t>(width) * height * 3,
              "decoded warehouse dimensions");
        bool hasNonZero = false;
        bool hasMultipleColors = false;
        const unsigned char firstR = rgb[0], firstG = rgb[1], firstB = rgb[2];
        for (std::size_t i = 0; i < rgb.size(); i += 3) {
            if (rgb[i] != 0 || rgb[i + 1] != 0 || rgb[i + 2] != 0)
                hasNonZero = true;
            if (rgb[i] != firstR || rgb[i + 1] != firstG || rgb[i + 2] != firstB)
                hasMultipleColors = true;
        }
        Check_(hasNonZero && hasMultipleColors, "warehouse PCX has color distribution");
        ++nonEmpty;
    }
    Check_(nonEmpty == 48, "all 48 warehouse PCX files decoded");
}

} // namespace

void RunWarehouseUiPcxTests_()
{
    CheckFixtureDecoding_();
    CheckFailureFixtures_();
    CheckRealAssets_();
    std::printf("PASS warehouse UI PCX: 48 assets and malformed-stream gates (%d checks)\n",
                g_checks_);
}
