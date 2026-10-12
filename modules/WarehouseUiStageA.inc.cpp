// ========== WarehouseUiStageA.inc.cpp ==========
// 阶段 A：HD_SOD 版本/文件/内存字节门卫 + session 核心接线。
// 当前不注册任何 UI Hook；0x039E66 是函数内部指令，接入 ABI 仍待阶段 B。

#include "WarehouseUi.hpp"
#pragma comment(lib, "version.lib")
#pragma comment(lib, "bcrypt.lib")
#include <vector>
#include <new>
#include <cstring>
#include <cwchar>

namespace {
static bool g_warehouse_ui_gate_ok_ = false;
static warehouseui::UiSessionStore_ g_warehouse_ui_session_;

static bool GetVersionString_(const BYTE* data, const wchar_t* name,
    wchar_t* out, int outChars)
{
    if (!data || !out || outChars <= 0) return false;
    out[0] = 0;
    struct LangCodePage { WORD lang; WORD codePage; };
    LangCodePage* translations = nullptr;
    UINT translationBytes = 0;
    wchar_t key[96] = {};
    if (!VerQueryValueW(const_cast<BYTE*>(data), L"\\VarFileInfo\\Translation",
            reinterpret_cast<void**>(&translations), &translationBytes)
        || !translations || translationBytes < sizeof(LangCodePage)) return false;
    _snwprintf_s(key, _countof(key), _TRUNCATE,
        L"\\StringFileInfo\\%04X%04X\\%s",
        translations[0].lang, translations[0].codePage, name);
    wchar_t* value = nullptr;
    UINT valueBytes = 0;
    if (!VerQueryValueW(const_cast<BYTE*>(data), key,
            reinterpret_cast<void**>(&value), &valueBytes) || !value) return false;
    lstrcpynW(out, value, outChars);
    return out[0] != 0;
}

static bool ReadWholeFileW_(const wchar_t* path, std::vector<std::uint8_t>& out)
{
    out.clear();
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    try { out.resize(static_cast<std::size_t>(size.QuadPart)); }
    catch (...) { CloseHandle(file); return false; }
    std::size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(out.size() - done > (1u << 20)
            ? (1u << 20) : out.size() - done);
        if (!ReadFile(file, out.data() + done, want, &got, nullptr) || got == 0) {
            CloseHandle(file);
            out.clear();
            return false;
        }
        done += got;
    }
    CloseHandle(file);
    return true;
}

static bool Sha256Hex_(const std::vector<std::uint8_t>& data, char* out, int outSize)
{
    if (!out || outSize < 65) return false;
    out[0] = 0;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD resultLength = 0;
    std::vector<std::uint8_t> object;
    std::uint8_t digest[32] = {};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
        nullptr, 0) == 0;
    if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
        &resultLength, 0) == 0;
    if (ok) {
        try { object.resize(objectLength); }
        catch (...) { ok = false; }
    }
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), objectLength,
        nullptr, 0, 0) == 0;
    if (ok) ok = BCryptHashData(hash,
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data.data())),
        static_cast<ULONG>(data.size()), 0) == 0;
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    for (int i = 0; i < 32; ++i)
        _snprintf_s(out + i * 2, outSize - i * 2, _TRUNCATE, "%02X", digest[i]);
    out[64] = 0;
    return true;
}

static bool VerifyWarehouseUiHdSod_()
{
    HMODULE module = GetModuleHandleW(L"HD_SOD.dll");
    if (!module) {
        LogError("仓库UI: HD_SOD.dll 未加载，阶段A门卫失败");
        return false;
    }
    wchar_t* modulePath = new (std::nothrow) wchar_t[kPathCap_ / 2]();
    if (!modulePath || !GetModuleFileNameW(module, modulePath, kPathCap_ / 2)) {
        delete[] modulePath;
        LogError("仓库UI: 无法读取 HD_SOD.dll 路径，阶段A门卫失败");
        return false;
    }
    std::vector<std::uint8_t> file;
    const bool fileRead = ReadWholeFileW_(modulePath, file);
    if (!fileRead) {
        delete[] modulePath;
        LogError("仓库UI: 无法读取 HD_SOD.dll 文件，阶段A门卫失败");
        return false;
    }
    const DWORD versionSize = GetFileVersionInfoSizeW(modulePath, nullptr);
    std::vector<std::uint8_t> version;
    try { version.resize(versionSize ? versionSize : 1); }
    catch (...) { version.clear(); }
    wchar_t fileVersion[64] = {};
    wchar_t productVersion[64] = {};
    const bool versionOk = versionSize > 0 && !version.empty()
        && GetFileVersionInfoW(modulePath, 0, versionSize, version.data())
        && GetVersionString_(version.data(), L"FileVersion", fileVersion, _countof(fileVersion))
        && GetVersionString_(version.data(), L"ProductVersion", productVersion, _countof(productVersion));
    char hash[65] = {};
    const bool hashOk = Sha256Hex_(file, hash, sizeof(hash));
    const bool sizeOk = file.size() == warehouseui::kHsodFileSize;
    const bool versionMatch = versionOk
        && wcscmp(fileVersion, warehouseui::kHsodFileVersion) == 0
        && wcscmp(productVersion, warehouseui::kHsodProductVersion) == 0;
    const std::uint8_t* image = reinterpret_cast<const std::uint8_t*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    bool imageOk = dos && dos->e_magic == IMAGE_DOS_SIGNATURE;
    std::size_t imageSize = 0;
    if (imageOk) {
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            image + dos->e_lfanew);
        imageOk = nt->Signature == IMAGE_NT_SIGNATURE;
        if (imageOk) imageSize = nt->OptionalHeader.SizeOfImage;
    }
    imageOk = imageOk && warehouseui::MatchesAllFingerprints(image, imageSize);
    const bool hashMatch = hashOk
        && _stricmp(hash, "37FA9723F39349558CA82F5379531311C266E8DFFFA382F80BB7550902039C75") == 0;
    delete[] modulePath;
    LogInfo("仓库UI: HD_SOD 门卫 version=%d size=%d hash=%d bytes=%d",
        versionMatch ? 1 : 0, sizeOk ? 1 : 0, hashMatch ? 1 : 0, imageOk ? 1 : 0);
    if (!versionMatch || !sizeOk || !hashMatch || !imageOk) {
        LogError("仓库UI: HD_SOD 指纹不匹配，阶段A不安装任何UI Hook");
        return false;
    }
    return true;
}
}

void InstallWarehouseUiStageA_()
{
    g_warehouse_ui_session_.Reset();
    g_warehouse_ui_gate_ok_ = VerifyWarehouseUiHdSod_();
    if (g_warehouse_ui_gate_ok_)
        LogInfo("仓库UI: 阶段A门卫通过，生命周期核心已就绪；UI Hook暂未安装");
}

bool WarehouseUiStageAEnabled_() noexcept
{
    return g_warehouse_ui_gate_ok_;
}

warehouseui::UiSessionStore_* WarehouseUiTestSession_() noexcept
{
    return &g_warehouse_ui_session_;
}
