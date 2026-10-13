// ========== WarehouseUiStageB.inc.cpp ==========
// 阶段 B/C/D：同一完整背包 modal 的 owned-items 仓库外壳和事务接线。
// 仅安装在 WarehouseUiStageA 的文件/内存指纹全部通过时。

#include "WarehouseUiFlow.hpp"

namespace {

static const int GUARD_WH_UI_MODAL = GuardRegisterHook_("WarehouseUiModal");
static const int GUARD_WH_UI_MODAL_END = GuardRegisterHook_("WarehouseUiModalEnd");
static const int GUARD_WH_UI_CALLBACK = GuardRegisterHook_("WarehouseUiCallback");

static constexpr UINT32 kWhUiModalSiteRva_ = 0x039E66;
static constexpr UINT32 kWhUiModalEndRva_ = 0x039E80;
static constexpr int kWhUiBackdropId_ = 30000;
static constexpr int kWhUiFrameId_ = 30003;
static constexpr int kWhUiPanelBackId_ = 30004;
static constexpr int kWhUiTitleId_ = 30001;
static constexpr int kWhUiCountId_ = 30002;
static constexpr int kWhUiPrevId_ = 30050;
static constexpr int kWhUiNextId_ = 30051;
static constexpr int kWhUiSlotBase_ = 30100;
// 64 槽 = 8x8，与 HD 完整背包网格同构（kPageSize 亦为 64）。
static constexpr int kWhUiSlotCount_ = 64;
static constexpr int kWhUiFrameColor_ = 0x7BEF;
// HD_SOD 0x039C03 实测引用的是 "BkPack.pcx"（不是 H3API HDassets 注释里的 .bmp）。
static constexpr const char* kWhUiBackdropPcx_ = "BkPack.pcx";
static constexpr const char* kWhUiTitleFont_ = "bigfont.fnt";

using WarehouseCallback_ = int (__stdcall *)(void*, H3Msg*);

struct WarehouseUiLive_ {
    void* dialog = nullptr;
    H3Hero* hero = nullptr;
    WarehouseCallback_ originalCallback = nullptr;
    std::uint64_t token = 0;
    warehouseui::Layout layout;
    std::size_t page = 0;
    warehouseui::Flow flow;
    bool flowActive = false;
    H3DlgText* title = nullptr;
    H3DlgText* count = nullptr;
    H3DlgDef* slots[kWhUiSlotCount_] = {};
};

static WarehouseUiLive_ g_wh_ui_live_;
static LoHook* g_wh_ui_modal_hook_ = nullptr;
static LoHook* g_wh_ui_modal_end_hook_ = nullptr;
static bool g_wh_ui_installed_ = false;
// HD_SOD+0x6620：完整背包外框阴影函数（stdcall x,y,w,h ret 0x10），Install 时缓存。
static UINT32 g_wh_ui_shadow_ = 0;

static int __stdcall WarehouseUiCallback_(void* self, H3Msg* msg);

static bool WarehouseUiOwns_(H3BaseDlg* dlg, H3DlgItem* item) noexcept
{
    if (!dlg || !item) return false;
    for (H3DlgItem* owned : dlg->GetList())
        if (owned == item) return true;
    return false;
}

static void WarehouseUiCopyNative_(H3Hero* hero,
                                   warehouse::NativeBackpack& native) noexcept
{
    native.fill({-1, -1});
    if (!hero) return;
    for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i) {
        native[i].artifactId = hero->backpackArtifacts[i].id;
        native[i].subtype = hero->backpackArtifacts[i].subtype;
    }
}

static void WarehouseUiWriteNative_(H3Hero* hero,
                                    const warehouse::NativeBackpack& native,
                                    int count) noexcept
{
    if (!hero) return;
    for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i) {
        hero->backpackArtifacts[i].id = native[i].artifactId;
        hero->backpackArtifacts[i].subtype = native[i].subtype;
    }
    hero->backpackCount = static_cast<INT8>(count);
}

static bool WarehouseUiHeld_() noexcept
{
    H3Artifact& held = H3HeldArtifact::Get();
    return held.id != -1;
}

static void WarehouseUiRefresh_(H3BaseDlg* dlg, bool redraw) noexcept
{
    if (!dlg || g_wh_ui_live_.dialog != dlg || !g_wh_ui_live_.hero) return;
    auto& records = g_warehouse_.heroes[static_cast<std::size_t>(
        g_wh_ui_live_.hero->id)];
    const std::size_t pages = warehouseui::PageCount(records.size());
    if (pages == 0) g_wh_ui_live_.page = 0;
    else if (g_wh_ui_live_.page >= pages) g_wh_ui_live_.page = pages - 1;
    const warehouseui::PageView view = warehouseui::MakePageView(
        records.size(), g_wh_ui_live_.page);
    if (g_wh_ui_live_.title) {
        char text[96];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "Warehouse - %s",
            g_wh_ui_live_.hero->name);
        g_wh_ui_live_.title->SetText(text);
    }
    if (g_wh_ui_live_.count) {
        char text[48];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%u/64  page %u/%u",
            static_cast<unsigned>(records.size()),
            static_cast<unsigned>(pages == 0 ? 1 : g_wh_ui_live_.page + 1),
            static_cast<unsigned>(pages == 0 ? 1 : pages));
        g_wh_ui_live_.count->SetText(text);
    }
    for (int i = 0; i < kWhUiSlotCount_; ++i) {
        H3DlgDef* slot = g_wh_ui_live_.slots[i];
        if (!slot) continue;
        const std::size_t at = view.begin + static_cast<std::size_t>(i);
        if (at < view.begin + view.count) {
            slot->SetFrame(records[at].artifactId);
            slot->ShowActivate();
        } else {
            slot->HideDeactivate();
        }
    }
    // Attach 阶段（ShowAndRun 之前）只更新内容；模态运行中的翻页才允许重绘。
    if (redraw) dlg->Redraw();
}

static bool WarehouseUiCloseAndRefresh_(H3BaseDlg* dlg, H3Msg* msg) noexcept
{
    if (!dlg || !msg) return false;
    // The native caller treats -2 as "rebuild this full backpack". This keeps
    // the game's own icon/hit-test/status-bar construction in charge after a
    // successful transaction, while the current dialog is still alive here.
    *reinterpret_cast<int*>(reinterpret_cast<BYTE*>(dlg) + 0x7C) = -2;
    msg->CloseDialog();
    return true;
}

static bool WarehouseUiHandle_(H3BaseDlg* dlg, H3Msg* msg) noexcept
{
    if (!dlg || !msg || g_wh_ui_live_.dialog != dlg || !g_wh_ui_live_.hero)
        return false;
    if (msg->command != eMsgCommand::ITEM_COMMAND
        || msg->subtype != eMsgSubtype::LBUTTON_DOWN)
        return false;
    const int id = msg->itemId;
    const int heroId = g_wh_ui_live_.hero->id;
    const int ownerId = g_wh_ui_live_.hero->owner;
    if (id >= 1000 && id < 1064) {
        warehouse::NativeBackpack native;
        WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
        const int count = g_wh_ui_live_.hero->backpackCount;
        g_wh_ui_live_.flow = warehouseui::Flow(native, count, heroId, ownerId,
            g_warehouse_.mutationEpoch, g_wh_ui_live_.token);
        if (!g_wh_ui_live_.flow.IsValid()) return true;
        g_wh_ui_live_.flow.Select(static_cast<std::size_t>(id - 1000));
        g_wh_ui_live_.flowActive = true;
        LogInfo("仓库UI: 已选择原生背包槽=%d hero=%d token=%llu",
            id - 1000, heroId,
            static_cast<unsigned long long>(g_wh_ui_live_.token));
        return true;
    }
    if (id >= kWhUiSlotBase_ && id < kWhUiSlotBase_ + kWhUiSlotCount_) {
        const int local = id - kWhUiSlotBase_;
        auto& records = g_warehouse_.heroes[static_cast<std::size_t>(heroId)];
        const warehouseui::PageView view = warehouseui::MakePageView(
            records.size(), g_wh_ui_live_.page);
        const std::size_t at = view.begin + static_cast<std::size_t>(local);
        if (at >= view.begin + view.count) return true;
        warehouse::NativeBackpack native;
        WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
        int count = g_wh_ui_live_.hero->backpackCount;
        if (g_wh_ui_live_.flowActive) {
            const warehouseui::Result result = g_wh_ui_live_.flow.Deposit(
                g_warehouse_, native, count, heroId, ownerId,
                g_wh_ui_live_.token, g_wh_locked_ != 0, WarehouseUiHeld_());
            if (result == warehouseui::Result::Ok) {
                WarehouseUiWriteNative_(g_wh_ui_live_.hero, native, count);
                g_wh_ui_live_.flowActive = false;
                LogInfo("仓库UI: 放入成功 hero=%d slot=%d count=%d",
                    heroId, g_wh_ui_live_.flow.SelectedIndex(), count);
                WarehouseUiCloseAndRefresh_(dlg, msg);
            } else {
                LogInfo("仓库UI: 放入拒绝 result=%d hero=%d",
                    static_cast<int>(result), heroId);
            }
            return true;
        }
        const warehouseui::Flow retrieve(native, count, heroId, ownerId,
            g_warehouse_.mutationEpoch, g_wh_ui_live_.token);
        if (!retrieve.IsValid()) return true;
        const warehouseui::Result result = const_cast<warehouseui::Flow&>(retrieve)
            .Retrieve(at, g_warehouse_, native, count, heroId, ownerId,
                g_wh_ui_live_.token, g_wh_locked_ != 0, WarehouseUiHeld_());
        if (result == warehouseui::Result::Ok) {
            WarehouseUiWriteNative_(g_wh_ui_live_.hero, native, count);
            LogInfo("仓库UI: 取回成功 hero=%d index=%u count=%d", heroId,
                static_cast<unsigned>(at), count);
            WarehouseUiCloseAndRefresh_(dlg, msg);
        } else {
            LogInfo("仓库UI: 取回拒绝 result=%d hero=%d", static_cast<int>(result), heroId);
        }
        return true;
    }
    if (id == kWhUiPrevId_ || id == kWhUiNextId_) {
        const std::size_t pages = warehouseui::PageCount(
            g_warehouse_.heroes[static_cast<std::size_t>(heroId)].size());
        if (pages != 0) {
            if (id == kWhUiPrevId_ && g_wh_ui_live_.page > 0)
                --g_wh_ui_live_.page;
            if (id == kWhUiNextId_ && g_wh_ui_live_.page + 1 < pages)
                ++g_wh_ui_live_.page;
            WarehouseUiRefresh_(dlg, true);
        }
        return true;
    }
    return false;
}

static void WarehouseUiAdd_(H3BaseDlg* dlg, int x, int y,
                            H3DlgItem* item) noexcept
{
    if (!dlg || !item) return;
    dlg->AddItem(item);
    item->ShowActivate();
    (void)x;
    (void)y;
}

static bool WarehouseUiNativeAnchor_(H3BaseDlg* dlg, int& x, int& y) noexcept
{
    // Arg(1)/Arg(2) 的语义未经反汇编确证；以原生第一格 hit item (ID 1000)
    // 的实际坐标为布局锚点，缺失时才退回栈参。
    if (!dlg) return false;
    for (H3DlgItem* item : dlg->GetList()) {
        if (item && item->GetID() == 1000) {
            x = item->GetX();
            y = item->GetY();
            return true;
        }
    }
    return false;
}

static void WarehouseUiAttach_(H3BaseDlg* dlg, H3Hero* hero,
                               int backpackX, int backpackY) noexcept
{
    if (!dlg || !hero || g_wh_ui_live_.dialog == dlg) return;
    if (!warehouseui::IsValidGameRecord({0, 0})) return;
    int anchorX = backpackX;
    int anchorY = backpackY;
    WarehouseUiNativeAnchor_(dlg, anchorX, anchorY);
    // 锚点是原生网格第一格 (ID 1000) 的左上角；完整背包 405x459 视觉区
    // 原点相对网格原点偏移 (-18, -50)（反汇编实测）。
    const int originX = anchorX - 18;
    const int originY = anchorY - 50;
    const warehouseui::Layout layout = warehouseui::MakeLayout(
        dlg->GetWidth(), dlg->GetHeight(), originX, originY);
    if (!layout.fits || !warehouse::IsValidHeroId(hero->id)) {
        LogInfo("仓库UI: 屏幕/背包布局不满足，当前弹窗只保留原生UI");
        return;
    }

    // 与完整背包同款的外框阴影（HD_SOD 0x01006620，右/底各两条暗化条）。
    // 该函数 stdcall(x,y,w,h) ret 0x10，构造函数自己也在 ShowAndRun 前调用，
    // 全局绘制对象此刻必已就绪；本函数整体运行在 modal 钩子的 CrashGuard __try 内。
    if (g_wh_ui_shadow_ != 0)
        reinterpret_cast<void (__stdcall *)(int, int, int, int)>(
            g_wh_ui_shadow_)(layout.warehouse.x, layout.warehouse.y,
            warehouseui::kPanelWidth, warehouseui::kPanelHeight);

    warehouse::NativeBackpack native;
    WarehouseUiCopyNative_(hero, native);
    const int count = hero->backpackCount;
    const std::uint64_t token = g_warehouse_ui_session_.BeginAttach();
    if (!g_warehouse_ui_session_.Activate(token)) return;

    g_wh_ui_live_ = {};
    g_wh_ui_live_.dialog = dlg;
    g_wh_ui_live_.hero = hero;
    g_wh_ui_live_.token = token;
    g_wh_ui_live_.layout = layout;
    g_wh_ui_live_.originalCallback = reinterpret_cast<WarehouseCallback_>(
        *reinterpret_cast<UINT32*>(reinterpret_cast<BYTE*>(dlg) + 0x70));
    g_wh_ui_live_.flow = warehouseui::Flow(native, count, hero->id, hero->owner,
        g_warehouse_.mutationEpoch, token);

    // 面板整块底板（原版对话框同款）：DIBOXBACK 256x256 平铺 + DLGBOX 玩家色边框，
    // 盖住面板全区域，标题/Tab 预留/底部不再露出底下的画面；失败退纯色框。
    H3DlgPcx16* panelBack = H3DlgPcx16::Create(layout.warehouse.x,
        layout.warehouse.y, warehouseui::kPanelWidth, warehouseui::kPanelHeight,
        kWhUiPanelBackId_, NH3Dlg::Assets::DIBOXBACK);
    const bool panelBackOk = panelBack && panelBack->GetPcx() != nullptr;
    if (panelBackOk) {
        panelBack->GetPcx()->BackgroundRegion(0, 0, warehouseui::kPanelWidth,
            warehouseui::kPanelHeight, FALSE);
        panelBack->GetPcx()->FrameRegion(0, 0, warehouseui::kPanelWidth,
            warehouseui::kPanelHeight, FALSE, hero->owner, FALSE);
        WarehouseUiAdd_(dlg, 0, 0, panelBack);
    } else {
        H3DlgFrame* frame = H3DlgFrame::Create(layout.warehouse.x,
            layout.warehouse.y, layout.warehouse.width, layout.warehouse.height,
            kWhUiFrameId_, static_cast<RGB565>(kWhUiFrameColor_));
        if (WarehouseUiOwns_(dlg, frame)) {
            static_cast<H3DlgItem*>(frame)->ShowActivate();
        } else {
            WarehouseUiAdd_(dlg, 0, 0, frame);
        }
    }
    // 格子背景：与完整背包相同的 BkPack.pcx（369x369 @ 网格原点），叠在底板上。
    H3DlgPcx* backdrop = H3DlgPcx::Create(
        layout.warehouse.x + warehouseui::kGridOriginDX,
        layout.warehouse.y + warehouseui::kGridOriginDY,
        kWhUiBackdropId_, kWhUiBackdropPcx_);
    const bool backdropOk = backdrop && backdrop->GetPcx() != nullptr;
    if (backdropOk) {
        WarehouseUiAdd_(dlg, 0, 0, backdrop);
    }
    // 标题条与 HD 完整背包同构：405x30 @ (0,18)，bigfont.fnt。
    g_wh_ui_live_.title = H3DlgText::Create(layout.warehouse.x,
        layout.warehouse.y + warehouseui::kTitleOffsetY,
        warehouseui::kPanelWidth, warehouseui::kTitleHeight,
        "Warehouse", kWhUiTitleFont_, eTextColor::REGULAR, kWhUiTitleId_,
        eTextAlignment::MIDDLE_CENTER, 0);
    if (WarehouseUiOwns_(dlg, g_wh_ui_live_.title)) {
        static_cast<H3DlgItem*>(g_wh_ui_live_.title)->ShowActivate();
    } else {
        WarehouseUiAdd_(dlg, 0, 0, g_wh_ui_live_.title);
    }
    // 64 槽 = 8x8 网格，DEF 图标 44x44 与 HD 完整背包同构
    // （步距 46，格内偏移 +2,+1）。
    const int gridX = layout.warehouse.x + warehouseui::kGridOriginDX;
    const int gridY = layout.warehouse.y + warehouseui::kGridOriginDY;
    for (int i = 0; i < kWhUiSlotCount_; ++i) {
        const int col = i % static_cast<int>(warehouseui::kGridColumns);
        const int row = i / static_cast<int>(warehouseui::kGridColumns);
        H3DlgDef* slot = H3DlgDef::Create(
            gridX + col * warehouseui::kGridPitch + warehouseui::kGridDefDX,
            gridY + row * warehouseui::kGridPitch + warehouseui::kGridDefDY,
            warehouseui::kGridDefSize, warehouseui::kGridDefSize,
            kWhUiSlotBase_ + i, NH3Dlg::Assets::ARTIFACT_DEF, 0, 0, FALSE, FALSE);
        g_wh_ui_live_.slots[i] = slot;
        WarehouseUiAdd_(dlg, 0, 0, slot);
    }
    // 底部条（网格下方 405x40 区域）：计数与翻页。
    // 分页行：BkPack 背景底(451)与底边距之间（kPagerRowY=459，高 32）。
    const int footerY = layout.warehouse.y + warehouseui::kPagerRowY;
    g_wh_ui_live_.count = H3DlgText::Create(layout.warehouse.x + 64,
        footerY, layout.warehouse.width - 128, warehouseui::kPagerRowHeight,
        "0/64", NH3Dlg::Text::SMALL, eTextColor::REGULAR, kWhUiCountId_,
        eTextAlignment::MIDDLE_CENTER, 0);
    if (WarehouseUiOwns_(dlg, g_wh_ui_live_.count)) {
        static_cast<H3DlgItem*>(g_wh_ui_live_.count)->ShowActivate();
    } else {
        WarehouseUiAdd_(dlg, 0, 0, g_wh_ui_live_.count);
    }
    H3DlgText* prev = H3DlgText::Create(layout.warehouse.x + 8,
        footerY, 52, warehouseui::kPagerRowHeight, "<", NH3Dlg::Text::SMALL,
        eTextColor::REGULAR, kWhUiPrevId_, eTextAlignment::MIDDLE_CENTER, 0);
    H3DlgText* next = H3DlgText::Create(layout.warehouse.x + layout.warehouse.width - 60,
        footerY, 52, warehouseui::kPagerRowHeight, ">", NH3Dlg::Text::SMALL,
        eTextColor::REGULAR, kWhUiNextId_, eTextAlignment::MIDDLE_CENTER, 0);
    WarehouseUiAdd_(dlg, 0, 0, prev);
    WarehouseUiAdd_(dlg, 0, 0, next);
    H3DlgTransparentItem* prevHit = H3DlgTransparentItem::Create(
        layout.warehouse.x + 8, footerY, 52, warehouseui::kPagerRowHeight,
        kWhUiPrevId_);
    H3DlgTransparentItem* nextHit = H3DlgTransparentItem::Create(
        layout.warehouse.x + layout.warehouse.width - 60,
        footerY, 52, warehouseui::kPagerRowHeight, kWhUiNextId_);
    WarehouseUiAdd_(dlg, 0, 0, prevHit);
    WarehouseUiAdd_(dlg, 0, 0, nextHit);

    *reinterpret_cast<UINT32*>(reinterpret_cast<BYTE*>(dlg) + 0x70) =
        reinterpret_cast<UINT32>(&WarehouseUiCallback_);
    WarehouseUiRefresh_(dlg, false);
    LogInfo("仓库UI: attached dialog=%p hero=%d token=%llu side=%d",
        dlg, hero->id, static_cast<unsigned long long>(token),
        static_cast<int>(layout.side));
}

static void WarehouseUiDetach_(H3BaseDlg* dlg) noexcept
{
    if (!dlg || g_wh_ui_live_.dialog != dlg) return;
    *reinterpret_cast<UINT32*>(reinterpret_cast<BYTE*>(dlg) + 0x70) =
        reinterpret_cast<UINT32>(g_wh_ui_live_.originalCallback);
    const std::uint64_t token = g_wh_ui_live_.token;
    g_wh_ui_live_ = {};
    g_warehouse_ui_session_.BeginClose(token, warehouseui::CloseReason::ExternalClose_);
    g_warehouse_ui_session_.FinishClose(token);
    LogInfo("仓库UI: detached token=%llu", static_cast<unsigned long long>(token));
}

static int __stdcall WarehouseUiCallback_(void* self, H3Msg* msg)
{
    H3BaseDlg* dlg = static_cast<H3BaseDlg*>(self);
    bool handled = false;
    __try {
        handled = WarehouseUiHandle_(dlg, msg);
    } __except (GuardCrashFilter_(GUARD_WH_UI_CALLBACK, GetExceptionInformation())) {}
    if (handled) return 1;
    WarehouseCallback_ original = g_wh_ui_live_.originalCallback;
    if (!original) return 0;
    return original(self, msg);
}

static int __stdcall OnWarehouseUiModal_(LoHook*, HookContext* context)
{
    __try {
        if (context) {
            H3BaseDlg* dlg = reinterpret_cast<H3BaseDlg*>(context->Local(2));
            H3Hero* hero = *reinterpret_cast<H3Hero**>(
                reinterpret_cast<BYTE*>(dlg) + 0x78);
            WarehouseUiAttach_(dlg, hero, context->Arg(1), context->Arg(2));
        }
    } __except (GuardCrashFilter_(GUARD_WH_UI_MODAL, GetExceptionInformation())) {}
    return EXEC_DEFAULT;
}

static int __stdcall OnWarehouseUiModalEnd_(LoHook*, HookContext* context)
{
    __try {
        if (context) {
            H3BaseDlg* dlg = reinterpret_cast<H3BaseDlg*>(context->Local(2));
            WarehouseUiDetach_(dlg);
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
    g_wh_ui_shadow_ = base + 0x6620; // 完整背包外框阴影（与 0x039A00 构造同一函数）
    g_wh_ui_modal_hook_ = _PI->WriteLoHook(base + kWhUiModalSiteRva_,
        &OnWarehouseUiModal_);
    g_wh_ui_modal_end_hook_ = _PI->WriteLoHook(base + kWhUiModalEndRva_,
        &OnWarehouseUiModalEnd_);
    if (!g_wh_ui_modal_hook_ || !g_wh_ui_modal_end_hook_) {
        LogError("仓库UI: modal LoHook 安装失败，保留原生背包");
        return;
    }
    g_wh_ui_installed_ = true;
    LogInfo("仓库UI: modal LoHook 已安装 RVA=0x%08X end=0x%08X",
        kWhUiModalSiteRva_, kWhUiModalEndRva_);
}
