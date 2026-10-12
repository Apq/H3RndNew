// WarehouseNative 独立回归：gzip 边界读取器的合成流、尾部与破坏族验证。
// 可选真实样本核对：设 H3WARE_GM1_REAL=<GM1路径> H3WARE_GM1_EXPECTED_END=<十进制>
// 时对该文件断言 nativeEnd（用于离线验证真实存档副本，不随仓库分发样本）。
#include "../modules/WarehouseNative.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks_ = 0;

void Check(bool ok, const char* label)
{
    ++g_checks_;
    if (!ok) {
        std::fprintf(stderr, "FAIL warehouse native: %s\n", label);
        std::exit(1);
    }
}

std::vector<std::uint8_t> CompressRaw_(const std::vector<std::uint8_t>& input, int level,
                                       int strategy)
{
    z_stream stream{};
    std::vector<std::uint8_t> out(input.size() + input.size() / 2 + 128);
    if (deflateInit2(&stream, level, Z_DEFLATED, -15, 8, strategy) != Z_OK)
        std::exit(1);
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int status = deflate(&stream, Z_FINISH);
    deflateEnd(&stream);
    Check(status == Z_STREAM_END, "deflate produces one raw member");
    out.resize(stream.total_out);
    return out;
}

std::vector<std::uint8_t> BuildGzip_(const std::vector<std::uint8_t>& input,
                                     int level, int strategy, std::uint8_t flags)
{
    std::vector<std::uint8_t> file;
    file.push_back(0x1F); file.push_back(0x8B); file.push_back(0x08);
    file.push_back(flags);
    for (int i = 0; i < 6; ++i) file.push_back(0);
    if (flags & 0x08) { // FNAME
        const char name[] = "save.gm1";
        file.insert(file.end(), name, name + sizeof(name));
    }
    const std::vector<std::uint8_t> deflated = CompressRaw_(input, level, strategy);
    file.insert(file.end(), deflated.begin(), deflated.end());
    const uLong crc = crc32(0L, Z_NULL, 0);
    const uLong crcValue = crc32(crc, input.data(), static_cast<uInt>(input.size()));
    for (int i = 0; i < 4; ++i) file.push_back(
        static_cast<std::uint8_t>((crcValue >> (8 * i)) & 0xFF));
    const uLong size = static_cast<uLong>(input.size());
    for (int i = 0; i < 4; ++i) file.push_back(
        static_cast<std::uint8_t>((size >> (8 * i)) & 0xFF));
    return file;
}

std::vector<std::uint8_t> Pattern_(std::size_t size, std::uint8_t seed)
{
    std::vector<std::uint8_t> data(size);
    std::uint32_t state = seed * 2654435761u + 1;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1103515245u + 12345u;
        data[i] = static_cast<std::uint8_t>((state >> 16) & 0xFF);
    }
    return data;
}

void CheckRoundTrip_(const std::vector<std::uint8_t>& input, int level,
                     int strategy, const char* label)
{
    const std::vector<std::uint8_t> file = BuildGzip_(input, level, strategy, 0);
    warehouse::native::NativeBoundary boundary{};
    Check(warehouse::native::ScanNativeEnd(file, boundary), label);
    Check(boundary.nativeEnd == file.size(), label);
    Check(boundary.inflatedBytes == input.size(), label);
}

} // namespace

void RunWarehouseNativeTests_()
{
    // 各 deflate 策略/等级的合成成员：边界=文件尾，ISIZE=原始长度。
    CheckRoundTrip_(Pattern_(64, 1), 0, Z_DEFAULT_STRATEGY, "stored-block member");
    CheckRoundTrip_(Pattern_(3000, 2), 9, Z_DEFAULT_STRATEGY, "dynamic-huffman member");
    CheckRoundTrip_(Pattern_(500, 3), 6, Z_FIXED, "fixed-huffman member");
    std::vector<std::uint8_t> big(262144, 0x5A); // >16KiB 输出循环
    for (std::size_t i = 0; i < big.size(); i += 97)
        big[i] = static_cast<std::uint8_t>(i & 0xFF);
    CheckRoundTrip_(big, 9, Z_DEFAULT_STRATEGY, "multi-window member");

    // 成员后带第三方尾部：边界必须停在首个成员尾。
    {
        const std::vector<std::uint8_t> file = BuildGzip_(Pattern_(1000, 4),
            9, Z_DEFAULT_STRATEGY, 0);
        std::vector<std::uint8_t> tailed = file;
        const std::uint8_t tail[] = { 0x48, 0x33, 0x57, 0x48, 0x2E, 0x2E, 0x2E };
        tailed.insert(tailed.end(), tail, tail + sizeof(tail));
        warehouse::native::NativeBoundary boundary{};
        Check(warehouse::native::ScanNativeEnd(tailed, boundary), "tailed member scans");
        Check(boundary.nativeEnd == file.size(), "tail excluded from native boundary");
    }

    // FNAME 头：解析器必须跳过文件名字段。
    {
        const std::vector<std::uint8_t> file = BuildGzip_(Pattern_(500, 5),
            9, Z_DEFAULT_STRATEGY, 0x08);
        warehouse::native::NativeBoundary boundary{};
        Check(warehouse::native::ScanNativeEnd(file, boundary)
            && boundary.nativeEnd == file.size(), "FNAME header skipped");
    }

    // 双成员文件：边界只取第一个成员尾（第二个成员视作尾部字节）。
    {
        const std::vector<std::uint8_t> first = BuildGzip_(Pattern_(800, 6),
            9, Z_DEFAULT_STRATEGY, 0);
        const std::vector<std::uint8_t> second = BuildGzip_(Pattern_(300, 7),
            1, Z_DEFAULT_STRATEGY, 0);
        std::vector<std::uint8_t> both = first;
        both.insert(both.end(), second.begin(), second.end());
        warehouse::native::NativeBoundary boundary{};
        Check(warehouse::native::ScanNativeEnd(both, boundary)
            && boundary.nativeEnd == first.size(), "first member boundary only");
    }

    // 破坏族：魔数/截断/ISIZE 不符 → 拒绝。
    {
        std::vector<std::uint8_t> file = BuildGzip_(Pattern_(700, 8),
            9, Z_DEFAULT_STRATEGY, 0);
        warehouse::native::NativeBoundary boundary{};
        file[0] = 0x2E;
        Check(!warehouse::native::ScanNativeEnd(file, boundary), "bad magic rejected");
        file[0] = 0x1F;
        const std::size_t full = file.size();
        file.resize(full - 4); // 尾部截断
        Check(!warehouse::native::ScanNativeEnd(file, boundary), "truncated trailer rejected");
        file.resize(full);
        file[full - 1] ^= 0x01; // ISIZE 低位翻转
        Check(!warehouse::native::ScanNativeEnd(file, boundary), "ISIZE mismatch rejected");
        file[full - 1] ^= 0x01;
        file.resize(10); // 只有头
        Check(!warehouse::native::ScanNativeEnd(file, boundary), "header-only rejected");
        const std::vector<std::uint8_t> empty;
        Check(!warehouse::native::ScanNativeEnd(empty, boundary), "empty input rejected");
    }

    // 可选：真实 GM1 样本核对（离线验证用，不入仓库）。
    const char* realPath = std::getenv("H3WARE_GM1_REAL");
    if (realPath && *realPath) {
        FILE* file = std::fopen(realPath, "rb");
        Check(file != nullptr, "open real GM1 sample");
        if (file) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            Check(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size(),
                "read real GM1 sample");
            std::fclose(file);
            warehouse::native::NativeBoundary boundary{};
            Check(warehouse::native::ScanNativeEnd(bytes, boundary),
                "real GM1 native boundary found");
            const char* expectedText = std::getenv("H3WARE_GM1_EXPECTED_END");
            if (expectedText && *expectedText) {
                const unsigned long long expected = std::strtoull(expectedText, nullptr, 10);
                Check(boundary.nativeEnd == expected,
                    "real GM1 nativeEnd matches expected");
                std::printf("real GM1 %s: nativeEnd=%zu inflated=%llu size=%zu\n",
                    realPath, boundary.nativeEnd, boundary.inflatedBytes, bytes.size());
            }
        }
    }

    std::printf("PASS warehouse native: %d checks\n", g_checks_);
}
