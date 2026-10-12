// ========== WarehouseStore.inc.cpp ==========
// 仓库原生适配层：SaveGame/LoadGame 后置接线 + 实际路径捕获 + 独立边界读取器。
// 纯决策全在 WarehouseStore.hpp / WarehouseNative.hpp；本文件只做：
//   1) 0x4D6EB0 流打开钩子：在本仓库操作窗口内记录原生真正使用的流/路径；
//   2) 0x4D6FC0 流关闭钩子：记录该流确已被原生关闭（时序证据，见局限）；
//   3) 0x4BEB60 SaveGame 后置：读文件 → 边界扫描 → PlanSave → 临时文件原子替换；
//   4) 0x4BEFF0 LoadGame 后置：读文件 → 边界扫描 → ApplyLoad 策略；
//   5) 0x58BFB0 建档入口（与真随机链式共挂）：新局上下文 → 清空 156 仓库。
// CrashGuard L2：原函数调用在 __try 之外恰好一次；异常安全默认 = 放行原函数
// 返回值，仓库逻辑整体失败（原版存档不受影响）。
// 已知局限（设计文档 §6.2/§9.3）：
//   - 0x4D6FC0 返回 void，真实 close 失败码在内部 gz 状态对象中尚未逆向，
//     本层只核实"close 确实发生在本流上"，不能证实 close 返回成功；
//   - 存档尾部由 zlib 定位首个 gzip 成员边界（ISIZE 双核对，CRC 差异不校验）。

#include "WarehouseNative.hpp"
#include "WarehouseStore.hpp"
#include "ScenarioContext.hpp"

namespace whstore = warehouse::store;

static const UINT32 kSaveGame_    = 0x4BEB60; // thiscall(this, name, 4参) ret 0x14；AL==1 成功
static const UINT32 kLoadGame_    = 0x4BEFF0; // thiscall(this, 3栈参) ret 0x0C；EAX==1 成功
static const UINT32 kStreamOpen_  = 0x4D6EB0; // thiscall(stream对象, path, mode)
static const UINT32 kStreamClose_ = 0x4D6FC0; // thiscall(stream对象)
static const UINT32 kWhStartGame_ = 0x58BFB0; // fastcall(this)，与真随机链式共挂

// 模块钩子 id（静态初始化期注册；文件在 CrashGuard.hpp 之后包含）。
static const int GUARD_WH_SAVE   = GuardRegisterHook_("WarehouseSave");
static const int GUARD_WH_LOAD   = GuardRegisterHook_("WarehouseLoad");
static const int GUARD_WH_STREAM = GuardRegisterHook_("WarehouseStreamOpen");
static const int GUARD_WH_CLOSE  = GuardRegisterHook_("WarehouseStreamClose");
static const int GUARD_WH_START  = GuardRegisterHook_("WarehouseStartGame");

namespace {

warehouse::State g_warehouse_;
// 读到损坏/非法附件后置位：拒绝仓库保存，直到下次成功读档或新局清空。
volatile LONG g_wh_locked_ = 0;
volatile LONGLONG g_wh_token_ = 0;

struct WhCapture_ {
    volatile LONG active;
    LONG wantWriteMode;      // 保存期望 'w'，读档期望 'r'
    char* path;              // 堆副本，窗口结束时释放
    void* stream;
    volatile LONG closedSeen;
    LONG opensSeen;
    LONG modeMatches;
};

WhCapture_ g_wh_cap_ = { 0, 0, nullptr, nullptr, 0, 0, 0 };

std::uint64_t WhPathIdentity_(const char* path) noexcept
{
    // FNV-1a 64：身份只用于观察一致性比对，不反推路径。
    std::uint64_t hash = 14695981039346656037ull;
    for (const char* p = path; p && *p; ++p) {
        hash ^= static_cast<unsigned char>(*p);
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1;
}

const char* WhStatusName_(whstore::Status status) noexcept
{
    switch (status) {
        case whstore::Status::Ok: return "Ok";
        case whstore::Status::InvalidToken: return "InvalidToken";
        case whstore::Status::PathIdentityMissing: return "PathIdentityMissing";
        case whstore::Status::NativeOperationFailed: return "NativeOperationFailed";
        case whstore::Status::StreamCloseFailed: return "StreamCloseFailed";
        case whstore::Status::BoundaryUntrusted: return "BoundaryUntrusted";
        case whstore::Status::AttachmentUnsafe: return "AttachmentUnsafe";
        case whstore::Status::NoAttachmentLegacyUnknown: return "NoAttachmentLegacyUnknown";
        case whstore::Status::AttachmentInvalid: return "AttachmentInvalid";
        case whstore::Status::InventoryReplaceFailed: return "InventoryReplaceFailed";
        case whstore::Status::AllocationFailure: return "AllocationFailure";
    }
    return "?";
}

bool WhReadFile_(const char* path, std::vector<std::uint8_t>& out) noexcept
{
    out.clear();
    const HANDLE file = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    std::vector<std::uint8_t> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
    } catch (...) {
        CloseHandle(file);
        return false;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(
            bytes.size() - done > 0x40000000u ? 0x40000000u : bytes.size() - done);
        if (!ReadFile(file, bytes.data() + done, want, &chunk, nullptr) || chunk == 0) {
            CloseHandle(file);
            return false;
        }
        done += chunk;
    }
    CloseHandle(file);
    out.swap(bytes);
    return true;
}

bool WhWriteFileAtomic_(const char* path,
                        const std::vector<std::uint8_t>& bytes) noexcept
{
    const std::size_t length = std::strlen(path);
    char* temp = new (std::nothrow) char[length + 16];
    if (!temp) return false;
    std::memcpy(temp, path, length);
    std::memcpy(temp + length, ".h3whtmp", 9); // 含结尾 0
    const HANDLE file = CreateFileA(temp, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        delete[] temp;
        return false;
    }
    bool ok = true;
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(
            bytes.size() - done > 0x40000000u ? 0x40000000u : bytes.size() - done);
        if (!WriteFile(file, bytes.data() + done, want, &chunk, nullptr) || chunk == 0) {
            ok = false;
            break;
        }
        done += chunk;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (ok) ok = MoveFileExA(temp, path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileA(temp);
    delete[] temp;
    return ok;
}

LONG WhBeginCapture_(bool forSave) noexcept
{
    if (InterlockedCompareExchange(&g_wh_cap_.active, 1, 0) != 0)
        return 0; // 嵌套/重入：不开新窗口，外层负责
    delete[] g_wh_cap_.path;
    g_wh_cap_.path = nullptr;
    g_wh_cap_.stream = nullptr;
    g_wh_cap_.wantWriteMode = forSave ? 1 : 0;
    g_wh_cap_.closedSeen = 0;
    g_wh_cap_.opensSeen = 0;
    g_wh_cap_.modeMatches = 0;
    return 1;
}

void WhEndCapture_() noexcept
{
    delete[] g_wh_cap_.path;
    g_wh_cap_.path = nullptr;
    InterlockedExchange(&g_wh_cap_.active, 0);
}

// 组装观察值并执行保存后置。POD 入参；所有可能抛出的工作在内部捕获。
void WhAfterSave_(std::uint64_t token, bool nativeOk,
                  const char* path, bool closedSeen) noexcept
{
    if (!nativeOk) {
        LogInfo("仓库: 原生存档未成功，本次不附加仓库数据");
        return;
    }
    if (g_wh_locked_) {
        LogInfo("仓库: 处于锁定状态（此前读到非法附件），本次保存不附加");
        return;
    }
    if (!path) {
        LogInfo("仓库: 未捕获到本次存档实际路径，拒绝附加（fail-closed）");
        return;
    }
    std::vector<std::uint8_t> file;
    if (!WhReadFile_(path, file)) {
        LogInfo("仓库: 读取已保存文件失败，拒绝附加 path=%s", path);
        return;
    }
    warehouse::native::NativeBoundary boundary;
    if (!warehouse::native::ScanNativeEnd(file, boundary)) {
        LogInfo("仓库: 原生流边界不可信，拒绝附加 size=%zu", file.size());
        return;
    }
    whstore::Observation observation;
    observation.operationToken = token;
    observation.pathIdentity = WhPathIdentity_(path);
    observation.pathIdentityKnown = true;
    observation.nativeSucceeded = true;
    observation.streamClosedSuccessfully = closedSeen;
    observation.nativeEnd = boundary.nativeEnd;

    whstore::SavePlan plan;
    try {
        plan = whstore::PlanSave(file, observation, g_warehouse_.heroes, {});
    } catch (...) {
        LogInfo("仓库: 保存计划异常，拒绝附加");
        return;
    }
    if (plan.status != whstore::Status::Ok) {
        LogInfo("仓库: 保存被策略拒绝 status=%s codecError=%d 原文件保持原样",
            WhStatusName_(plan.status), static_cast<int>(plan.codecError));
        return;
    }
    if (!WhWriteFileAtomic_(path, plan.newFile)) {
        LogInfo("仓库: 附加写入失败，原文件保持原样 path=%s", path);
        return;
    }
    LogInfo("仓库: 已附加 %zu 字节（原生前缀 %zu 字节）path=%s",
        plan.newFile.size() - boundary.nativeEnd, boundary.nativeEnd, path);
}

// 读档后置：POD 入参，策略裁决 + 生命周期（NoAttachment→显式清空、
// Invalid→锁定）。g_warehouse_ 是唯一游戏侧状态。
void WhAfterLoad_(std::uint64_t token, bool nativeOk,
                  const char* path, bool closedSeen) noexcept
{
    if (!nativeOk) {
        LogInfo("仓库: 原生读档未成功，仓库状态不动");
        return;
    }
    if (!path) {
        LogInfo("仓库: 未捕获到本次读档实际路径，仓库状态不动（fail-closed）");
        return;
    }
    std::vector<std::uint8_t> file;
    if (!WhReadFile_(path, file)) {
        LogInfo("仓库: 读取已载入文件失败，仓库状态不动 path=%s", path);
        return;
    }
    warehouse::native::NativeBoundary boundary;
    if (!warehouse::native::ScanNativeEnd(file, boundary)) {
        LogInfo("仓库: 原生流边界不可信，仓库状态不动 size=%zu", file.size());
        return;
    }
    whstore::Observation observation;
    observation.operationToken = token;
    observation.pathIdentity = WhPathIdentity_(path);
    observation.pathIdentityKnown = true;
    observation.nativeSucceeded = true;
    observation.streamClosedSuccessfully = closedSeen;
    observation.nativeEnd = boundary.nativeEnd;

    whstore::LoadPlan plan;
    try {
        plan = whstore::ApplyLoad(file, observation, g_warehouse_, {});
    } catch (...) {
        LogInfo("仓库: 读档策略异常，仓库状态不动");
        return;
    }
    switch (plan.status) {
        case whstore::Status::Ok:
            InterlockedExchange(&g_wh_locked_, 0);
            LogInfo("仓库: 读档成功，156 仓库已整包替换 epoch=%llu",
                static_cast<unsigned long long>(g_warehouse_.mutationEpoch));
            return;
        case whstore::Status::NoAttachmentLegacyUnknown: {
            // 门禁全绿仍无自有块 = 确认从未有过仓库块的原生档：显式清空
            // （歧义情形的“绝不清空”由纯层保证；这里是有证据链的显式决策）。
            const warehouse::Result cleared = warehouse::ClearForNewGame(g_warehouse_);
            InterlockedExchange(&g_wh_locked_, 0);
            LogInfo("仓库: 原生档无仓库块，按旧档显式清空 result=%d",
                static_cast<int>(cleared));
            return;
        }
        case whstore::Status::AttachmentInvalid:
        case whstore::Status::InventoryReplaceFailed:
        case whstore::Status::AttachmentUnsafe:
            InterlockedExchange(&g_wh_locked_, plan.lockAdvised ? 1 : 0);
            LogInfo("仓库: 附件非法 status=%s codecError=%d replaceResult=%d "
                "锁定=%d（下次成功读档/新局解除）",
                WhStatusName_(plan.status), static_cast<int>(plan.codecError),
                static_cast<int>(plan.replaceResult), plan.lockAdvised ? 1 : 0);
            return;
        default:
            LogInfo("仓库: 读档被策略拒绝 status=%s（状态不动）",
                WhStatusName_(plan.status));
            return;
    }
}

} // namespace

// 钩子回调（EXTENDED_：this 与原参数上栈，替换函数统一 __stdcall）。
// 每个回调只含 POD，__try 块内不构造需要析构的对象。

static int __stdcall OnSaveGame_(HiHook* hook, void* self, const char* name,
    int a3, int a4, int a5, int a6)
{
    const std::uint64_t token =
        static_cast<std::uint64_t>(InterlockedIncrement64(&g_wh_token_));
    const LONG opened = WhBeginCapture_(true);
    const int result = THISCALL_6(int, hook->GetDefaultFunc(), self, name, a3, a4, a5, a6);
    const char* path = opened ? g_wh_cap_.path : nullptr;
    const bool closed = opened ? g_wh_cap_.closedSeen != 0 : false;
    __try {
        WhAfterSave_(token, (result & 0xFF) == 1, path, closed);
    } __except (GuardCrashFilter_(GUARD_WH_SAVE, GetExceptionInformation())) {}
    if (opened) WhEndCapture_();
    return result;
}

static int __stdcall OnLoadGame_(HiHook* hook, void* self,
    void* arg2, int arg3, int arg4)
{
    const std::uint64_t token =
        static_cast<std::uint64_t>(InterlockedIncrement64(&g_wh_token_));
    const LONG opened = WhBeginCapture_(false);
    const int result = THISCALL_4(int, hook->GetDefaultFunc(), self, arg2, arg3, arg4);
    const char* path = opened ? g_wh_cap_.path : nullptr;
    const bool closed = opened ? g_wh_cap_.closedSeen != 0 : false;
    __try {
        WhAfterLoad_(token, result == 1, path, closed);
    } __except (GuardCrashFilter_(GUARD_WH_LOAD, GetExceptionInformation())) {}
    if (opened) WhEndCapture_();
    return result;
}

static void* __stdcall OnStreamOpen_(HiHook* hook, void* streamObject,
    const char* path, const char* mode)
{
    void* const result = THISCALL_3(void*, hook->GetDefaultFunc(), streamObject, path, mode);
    __try {
        if (!g_wh_cap_.active || !streamObject || !path || !mode) return result;
        ++g_wh_cap_.opensSeen;
        const char first = *mode;
        const bool match = g_wh_cap_.wantWriteMode
            ? (first == 'w') : (first == 'r');
        if (match) {
            ++g_wh_cap_.modeMatches;
            const std::size_t length = std::strlen(path);
            char* copy = new (std::nothrow) char[length + 1];
            if (copy) {
                std::memcpy(copy, path, length + 1);
                delete[] g_wh_cap_.path;
                g_wh_cap_.path = copy;
                g_wh_cap_.stream = streamObject;
            }
        }
    } __except (GuardCrashFilter_(GUARD_WH_STREAM, GetExceptionInformation())) {}
    return result;
}

static void __stdcall OnStreamClose_(HiHook* hook, void* streamObject)
{
    __try {
        if (g_wh_cap_.active && streamObject
            && streamObject == g_wh_cap_.stream)
            InterlockedExchange(&g_wh_cap_.closedSeen, 1);
    } __except (GuardCrashFilter_(GUARD_WH_CLOSE, GetExceptionInformation())) {}
    typedef void(__thiscall* OriginalClose_)(void*);
    reinterpret_cast<OriginalClose_>(hook->GetDefaultFunc())(streamObject);
}

// 与真随机链式共挂建档入口（先装真随机、后装本钩子）。只窥视场景上下文
// 不消费——消费权在真随机 OnStartGame_。若链序导致窥视为 Unknown，本钩子
// 只记录不清空（hook ABI 回归会实证链序，见 tests/hook_abi_test.cpp）。
static int __stdcall OnWhStartGame_(HiHook* hook, int self)
{
    const LONG context = g_scenario_context_;
    const int result = FASTCALL_1(int, hook->GetDefaultFunc(), self);
    __try {
        if (context == kScenarioNewGame_) {
            const warehouse::Result cleared = warehouse::ClearForNewGame(g_warehouse_);
            InterlockedExchange(&g_wh_locked_, 0);
            LogInfo("仓库: 新局建档，156 仓库已清空 result=%d",
                static_cast<int>(cleared));
        }
    } __except (GuardCrashFilter_(GUARD_WH_START, GetExceptionInformation())) {}
    return result;
}

void InstallWarehouseHooks_()
{
    if (!_PI) return;
    _PI->WriteHiHook(kSaveGame_, SPLICE_, EXTENDED_, THISCALL_,
        reinterpret_cast<void*>(&OnSaveGame_));
    _PI->WriteHiHook(kLoadGame_, SPLICE_, EXTENDED_, THISCALL_,
        reinterpret_cast<void*>(&OnLoadGame_));
    _PI->WriteHiHook(kStreamOpen_, SPLICE_, EXTENDED_, THISCALL_,
        reinterpret_cast<void*>(&OnStreamOpen_));
    _PI->WriteHiHook(kStreamClose_, SPLICE_, EXTENDED_, THISCALL_,
        reinterpret_cast<void*>(&OnStreamClose_));
    _PI->WriteHiHook(kWhStartGame_, SPLICE_, EXTENDED_, THISCALL_,
        reinterpret_cast<void*>(&OnWhStartGame_));
    LogInfo("仓库: SaveGame/LoadGame/流打开/流关闭/建档 钩子已安装");
}

// 离线回归入口：仅测试 EXE 使用（不导出、游戏内无害）。
extern "C" LONG WarehouseTestBeginCapture_(int forSave)
{
    return WhBeginCapture_(forSave != 0);
}
extern "C" void WarehouseTestEndCapture_() { WhEndCapture_(); }
extern "C" const char* WarehouseTestCapturedPath_() { return g_wh_cap_.path; }
extern "C" LONG WarehouseTestCaptureClosed_() { return g_wh_cap_.closedSeen; }
extern "C" warehouse::State* WarehouseTestState_() { return &g_warehouse_; }
extern "C" LONG WarehouseTestLocked_() { return g_wh_locked_; }
extern "C" void WarehouseTestReset_()
{
    g_warehouse_ = warehouse::State();
    InterlockedExchange(&g_wh_locked_, 0);
    WhEndCapture_();
}
