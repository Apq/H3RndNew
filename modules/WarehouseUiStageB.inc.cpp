// ========== WarehouseUiStageB.inc.cpp ==========
// 阶段 B 钩子层：完整背包 modal 的两只 LoHook（0x039E66 attach /
// 0x039E80 detach）与安装/回滚，仅安装在 WarehouseUiStageA 的文件/内存
// 指纹全部通过时。round-21 界面与功能分离：面板主体（HdWarehouseUi）
// 已迁至 WarehouseUiPanel.inc.cpp，本文件只做翻译（HookContext → 契约
// 参数）与铠甲，经 modules/UiPort.hpp 的 g_uiWarehouse 调用界面实现。

#include "UiPort.hpp"

namespace {

// 铠甲 id 在 WarehouseUiPanel.inc.cpp 顶部注册（同一未命名命名空间，
// 定义先于本文件），这里直接使用，避免二次注册占用 id 槽位。
static constexpr UINT32 kWhUiModalSiteRva_ = 0x039E66;
static constexpr UINT32 kWhUiModalEndRva_ = 0x039E80;
static LoHook* g_wh_ui_modal_hook_ = nullptr;
static LoHook* g_wh_ui_modal_end_hook_ = nullptr;
static bool g_wh_ui_installed_ = false;
// 反汇编参考锚点（common/network/timeout 前置处理结尾），预留：暂不安装。
static constexpr UINT32 kWhUiPostCommonRva_ = 0x0396AB;
static LoHook* g_wh_ui_post_common_hook_ = nullptr;

// 解析结果自证（2026-10-14 崩溃教训）：把指针的归属模块、偏移、页保护打进日志，
// 模块不符或“期望代码但落在不可执行页”时直接告警。
// 背景：round-14 初版把 H3.exe 的绝对地址（H3DlgTextPcx 构造 0x5BCB70、游戏 new
// 0x617492）当成 HD_SOD 的 RVA 再加基址，跳进 HD_SOD 别的代码并在 .data 段执行，
// 抛出 0xC0000005 让仓库面板整块创建失败（日志 addr=HD_SOD+0x10FB609）。
// 这类错误靠肉眼读常量看不出来，必须让日志自己说话。
static void WarehouseUiLogPointer_(const char* name, const void* address,
                                   const void* expectedModule, bool needsCode) noexcept {
    if (!address) {
        LogWarn("仓库UI: 指针 %s 解析为空", name);
        return;
    }
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        LogError("仓库UI: 指针 %s 不可查询 addr=%p", name, address);
        return;
    }
    const DWORD codePages = PAGE_EXECUTE | PAGE_EXECUTE_READ
        | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const bool executable = (info.Protect & codePages) != 0;
    const char* owner = "其它模块";
    if (info.AllocationBase == GetModuleHandleW(nullptr)) owner = "H3.exe";
    else if (info.AllocationBase == GetModuleHandleW(L"HD_SOD.dll")) owner = "HD_SOD.dll";
    else if (info.AllocationBase == GetModuleHandleW(L"H3RndNew.dll")) owner = "H3RndNew.dll";
    const unsigned offset = static_cast<unsigned>(
        static_cast<const BYTE*>(address) - static_cast<const BYTE*>(info.AllocationBase));
    const bool mismatch = expectedModule && info.AllocationBase != expectedModule;
    LogInfo("仓库UI: 指针 %s = %s+0x%X (%p) protect=0x%X%s%s", name, owner, offset,
        address, info.Protect,
        needsCode ? (executable ? "" : " [警告: 期望代码页但不可执行]") : "",
        mismatch ? " [警告: 归属模块不符]" : "");
}

// 由 WarehouseUiPanel.inc.cpp 的 WarehouseUiAttach_ 安装到 dlg+0x70 的回调
// （前向声明在 Panel 内）。铠甲与「原生 callback 恰好一次」约定都在实现里，
// 这里只做契约转发；g_uiWarehouse 在生产 TU 恒非空（UiPortWiring 静态接线）。
static int __stdcall WarehouseUiCallback_(void* self, H3Msg* msg)
{
    return g_uiWarehouse ? g_uiWarehouse->Callback(self, msg) : 0;
}

static int __stdcall OnWarehouseUiModal_(LoHook*, HookContext* context)
{
    // 未完成安装（任一只 hook 失败已回滚）时绝不 attach，避免只装一半导致
    // 永不 detach 的残留状态。
    if (!g_wh_ui_installed_) return EXEC_DEFAULT;
    __try {
        if (context) {
            H3BaseDlg* dlg = reinterpret_cast<H3BaseDlg*>(context->Local(2));
            H3Hero* hero = *reinterpret_cast<H3Hero**>(
                reinterpret_cast<BYTE*>(dlg) + 0x78);
            g_uiWarehouse->Attach(dlg, hero,
                static_cast<int>(context->Arg(1)), static_cast<int>(context->Arg(2)));
        }
    } __except (GuardCrashFilter_(GUARD_WH_UI_MODAL, GetExceptionInformation())) {}
    return EXEC_DEFAULT;
}

static int __stdcall OnWarehouseUiModalEnd_(LoHook*, HookContext* context)
{
    __try {
        if (context) {
            H3BaseDlg* dlg = reinterpret_cast<H3BaseDlg*>(context->Local(2));
            g_uiWarehouse->Detach(dlg);
        }
    } __except (GuardCrashFilter_(GUARD_WH_UI_MODAL_END, GetExceptionInformation())) {}
    return EXEC_DEFAULT;
}

} // namespace

void InstallWarehouseUiStageB_()
{
    if (!WarehouseUiStageAEnabled_() || g_wh_ui_installed_ || !_PI) return;
    HMODULE module = GetModuleHandleW(L"HD_SOD.dll");
    if (!module) return;
    const UINT32 base = reinterpret_cast<UINT32>(module);
    g_wh_ui_scroll_enabled_ = reinterpret_cast<const int*>(base + 0x157FB8);
    g_wh_ui_shadow_ = base + 0x6620; // 完整背包外框阴影（与 0x039A00 构造同一函数）
    g_wh_ui_skin_factory_ = reinterpret_cast<WarehouseSkinFactory_>(base + 0x0FADB0);
    // round-14：状态条不再解析任何 H3.exe 侧的构造/分配/刷新指针（0x5BCB70、
    // 0x617492 等是 H3.exe 的绝对地址，加 HD_SOD 基址会跳到别处代码并崩溃——
    // 2026-10-14 实机崩溃复盘）。创建与刷新全部走 H3API：
    // H3ObjectAllocator<H3DlgTextPcx> + THISCALL_12(0x5BCB70) + SetText/Draw/Refresh。
    // 每个解析结果自证归属（模块/偏移/页属性），地址空间混淆当场暴露在日志里。
    WarehouseUiLogPointer_("ScrollEnabled", g_wh_ui_scroll_enabled_, module, false);
    WarehouseUiLogPointer_("Shadow", reinterpret_cast<const void*>(g_wh_ui_shadow_),
        module, true);
    WarehouseUiLogPointer_("SkinFactory",
        reinterpret_cast<const void*>(g_wh_ui_skin_factory_), module, true);
    WarehouseUiLogPointer_("StatusBarCtor(H3.exe 绝对地址)",
        reinterpret_cast<const void*>(kWhUiStatusCtorVa_), GetModuleHandleW(nullptr), true);
    g_wh_ui_modal_hook_ = _PI->WriteLoHook(base + kWhUiModalSiteRva_,
        &OnWarehouseUiModal_);
    g_wh_ui_modal_end_hook_ = _PI->WriteLoHook(base + kWhUiModalEndRva_,
        &OnWarehouseUiModalEnd_);
    if (!g_wh_ui_modal_hook_ || !g_wh_ui_modal_end_hook_) {
        // 两只 hook 必须同时生效：只装 attach 而 end 失败会让 dialog 析构后
        // 我的自有点击/视觉层永不恢复（vtable、PCX 裸指针残留）。失败即回滚。
        if (g_wh_ui_modal_hook_) {
            g_wh_ui_modal_hook_->Destroy();
            g_wh_ui_modal_hook_ = nullptr;
        }
        if (g_wh_ui_modal_end_hook_) {
            g_wh_ui_modal_end_hook_->Destroy();
            g_wh_ui_modal_end_hook_ = nullptr;
        }
        LogError("仓库UI: modal LoHook 安装失败，已回滚，保留原生背包");
        return;
    }
    g_wh_ui_installed_ = true;
    LogInfo("仓库UI: modal LoHook 已安装 RVA=0x%08X end=0x%08X",
        kWhUiModalSiteRva_, kWhUiModalEndRva_);
}
