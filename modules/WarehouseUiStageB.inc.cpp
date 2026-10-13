// ========== WarehouseUiStageB.inc.cpp ==========
// 阶段 B/C/D：同一完整背包 modal 的 owned-items 仓库外壳和事务接线。
// 仅安装在 WarehouseUiStageA 的文件/内存指纹全部通过时。

#include "WarehouseUiFlow.hpp"

namespace {

static const int GUARD_WH_UI_MODAL = GuardRegisterHook_("WarehouseUiModal");
static const int GUARD_WH_UI_MODAL_END = GuardRegisterHook_("WarehouseUiModalEnd");
static const int GUARD_WH_UI_POST_COMMON = GuardRegisterHook_("WarehouseUiPostCommon");

static constexpr UINT32 kWhUiModalSiteRva_ = 0x039E66;
static constexpr UINT32 kWhUiModalEndRva_ = 0x039E80;
static constexpr int kWhUiBackdropId_ = 30000;
static constexpr int kWhUiFrameId_ = 30003;
// HD 原生整窗背景 item 的 ID；正常背景由 HD_SOD+0x0FADB0 生成。
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

static constexpr std::size_t kWhUiPanelItemCapacity_ = 128;

struct WarehouseUiLive_ {
    void* dialog = nullptr;
    H3Hero* hero = nullptr;
    WarehouseCallback_ originalCallback = nullptr;
    bool expanded = false;
    H3LoadedPcx16* saveUnder = nullptr;
    int saveX = 0;
    int saveY = 0;
    int saveWidth = 0;
    int saveHeight = 0;
    std::uint64_t token = 0;
    warehouseui::Layout layout;
    std::size_t page = 0;
    warehouseui::Flow flow;
    bool flowActive = false;
    H3DlgCaptionButton* openButton = nullptr;
    H3DlgCaptionButton* closeButton = nullptr;
    H3DlgText* title = nullptr;
    H3DlgText* count = nullptr;
    H3DlgDef* slots[kWhUiSlotCount_] = {};
    H3DlgItem* panelItems[kWhUiPanelItemCapacity_] = {};
    std::size_t panelItemCount = 0;
};

static WarehouseUiLive_ g_wh_ui_live_;
static LoHook* g_wh_ui_modal_hook_ = nullptr;
static LoHook* g_wh_ui_modal_end_hook_ = nullptr;
static bool g_wh_ui_installed_ = false;
// HD_SOD+0x6620：完整背包外框阴影函数（stdcall x,y,w,h ret 0x10），Install 时缓存。
static UINT32 g_wh_ui_shadow_ = 0;
// HD 完整背包真正整窗底图工厂：fastcall(x,y,width,height,id,statusBar,color)，ret14。
// sb%04u%04u%1u%1u 是运行时缓存键，实际素材 DiBoxBck.PCX + dialgbox.def。
using WarehouseSkinFactory_ = H3DlgPcx16* (__fastcall *)(int, int, int, int, int, int, int);
static WarehouseSkinFactory_ g_wh_ui_skin_factory_ = nullptr;

static constexpr int kWhUiOpenId_ = 30200;
static constexpr int kWhUiCloseId_ = 30060;
static constexpr int kWhUiOpenX_ = 8;
static constexpr int kWhUiOpenY_ = 18;
static constexpr int kWhUiButtonWidth_ = 64;
static constexpr int kWhUiButtonHeight_ = 30;
static constexpr int kWhUiPanelSaveExtra_ = 8;
static const char kWhUiGbkWarehouse_[] = {
    static_cast<char>(0xB2), static_cast<char>(0xD6),
    static_cast<char>(0xBF), static_cast<char>(0xE2), 0x00 };
static const char kWhUiGbkClose_[] = {
    static_cast<char>(0xB9), static_cast<char>(0xD8),
    static_cast<char>(0xB1), static_cast<char>(0xD5), 0x00 };
// 交互接点反汇编结论（见 H3Note 完整背包笔记）：
//  0x0395D0..0x0396AB 为 common/network/timeout 前置处理，结果在 [ebp-0x28]；
//  本模块通过替换 dlg+0x70 的 callback 指针接入，包装器先跑自有分支再调
//  原生 callback 恰好一次，保证原版路径不被吞掉（右键/Esc/网络/超时原样透传）。
static constexpr UINT32 kWhUiPostCommonRva_ = 0x0396AB; // 反汇编参考锚点
static LoHook* g_wh_ui_post_common_hook_ = nullptr;    // 预留：暂不安装
static void WarehouseUiSetExpanded_(H3BaseDlg* dlg, bool expanded, bool redraw) noexcept;
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
            if (g_wh_ui_live_.expanded) slot->ShowActivate();
            else slot->HideDeactivate();
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
    // Esc（command=KEY_DOWN/UP, subtype=1）：仅仓库展开态消费——收起仓库层，
    // 消息被安全中和后原版 callback 仍执行一次但不进入关闭分支。
    // 仓库已关闭时不拦截，原生 Esc 关闭背包的行为保持不变。
    if (msg->command == eMsgCommand::KEY_UP && msg->subtype == static_cast<eMsgSubtype>(1)) {
        if (!g_wh_ui_live_.expanded) return false;
        WarehouseUiSetExpanded_(dlg, false, true);
        return true;
    }
    if (msg->command != eMsgCommand::ITEM_COMMAND)
        return false;
    const bool leftDown = msg->subtype == eMsgSubtype::LBUTTON_DOWN;
    const bool leftClick = msg->subtype == eMsgSubtype::LBUTTON_CLICK;
    if (!leftDown && !leftClick) return false;
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
    if (id >= 2000 && id < 2064 && g_wh_ui_live_.expanded) {
        // 展开态下点击宝物图标 (DEF id=2000..2063) 也应作为仓库事务的
        // 背包侧选择来源；right-click 交还原生信息路径（返回 false）。
        if (leftDown || leftClick) {
            const int slot = id - 2000;
            warehouse::NativeBackpack native;
            WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
            g_wh_ui_live_.flow = warehouseui::Flow(native,
                g_wh_ui_live_.hero->backpackCount, heroId, ownerId,
                g_warehouse_.mutationEpoch, g_wh_ui_live_.token);
            if (g_wh_ui_live_.flow.IsValid()) {
                g_wh_ui_live_.flow.Select(static_cast<std::size_t>(slot));
                g_wh_ui_live_.flowActive = true;
                LogInfo("仓库UI: 已从宝物图标选择背包槽=%d hero=%d", slot, heroId);
            }
            return true;
        }
        return false;
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
    if (id == kWhUiOpenId_) {
        WarehouseUiSetExpanded_(dlg, !g_wh_ui_live_.expanded, true);
        return true;
    }
    if (id == kWhUiCloseId_) {
        WarehouseUiSetExpanded_(dlg, false, true);
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

static void WarehouseUiAddPanelItem_(H3BaseDlg* dlg, H3DlgItem* item) noexcept
{
    if (!dlg || !item) return;
    dlg->AddItem(item);
    item->HideDeactivate();
    if (g_wh_ui_live_.panelItemCount < kWhUiPanelItemCapacity_)
        g_wh_ui_live_.panelItems[g_wh_ui_live_.panelItemCount++] = item;
}

static void WarehouseUiRestoreSaveUnder_() noexcept
{
    if (!g_wh_ui_live_.saveUnder) return;
    H3WindowManager* manager = H3WindowManager::Get();
    H3LoadedPcx16* screen = manager ? manager->GetDrawBuffer() : nullptr;
    if (screen && g_wh_ui_live_.saveX >= 0 && g_wh_ui_live_.saveY >= 0
        && g_wh_ui_live_.saveX + g_wh_ui_live_.saveWidth <= screen->width
        && g_wh_ui_live_.saveY + g_wh_ui_live_.saveHeight <= screen->height) {
        g_wh_ui_live_.saveUnder->DrawToPcx16(g_wh_ui_live_.saveX,
            g_wh_ui_live_.saveY, FALSE, screen, 0, 0);
        manager->H3Redraw(g_wh_ui_live_.saveX, g_wh_ui_live_.saveY,
            g_wh_ui_live_.saveWidth, g_wh_ui_live_.saveHeight);
    }
    g_wh_ui_live_.saveUnder->Destroy(TRUE);
    g_wh_ui_live_.saveUnder = nullptr;
}

static bool WarehouseUiCaptureSaveUnder_(H3BaseDlg* dlg) noexcept
{
    if (!dlg) return false;
    const warehouseui::Rect panel = g_wh_ui_live_.layout.warehouse;
    H3WindowManager* manager = H3WindowManager::Get();
    H3LoadedPcx16* screen = manager ? manager->GetDrawBuffer() : nullptr;
    if (!screen || panel.x < 0 || panel.y < 0
        || panel.Right() + kWhUiPanelSaveExtra_ > screen->width
        || panel.Bottom() + kWhUiPanelSaveExtra_ > screen->height)
        return false;
    g_wh_ui_live_.saveX = panel.x;
    g_wh_ui_live_.saveY = panel.y;
    g_wh_ui_live_.saveWidth = panel.width + kWhUiPanelSaveExtra_;
    g_wh_ui_live_.saveHeight = panel.height + kWhUiPanelSaveExtra_;
    g_wh_ui_live_.saveUnder = H3LoadedPcx16::Create(
        "", g_wh_ui_live_.saveWidth, g_wh_ui_live_.saveHeight);
    if (!g_wh_ui_live_.saveUnder) return false;
    g_wh_ui_live_.saveUnder->CopyRegion(screen, g_wh_ui_live_.saveX,
        g_wh_ui_live_.saveY);
    return true;
}

static void WarehouseUiSetExpanded_(H3BaseDlg* dlg, bool expanded,
                                    bool redraw) noexcept
{
    if (!dlg || g_wh_ui_live_.dialog != dlg || g_wh_ui_live_.expanded == expanded)
        return;
    if (!expanded) {
        for (std::size_t i = g_wh_ui_live_.panelItemCount; i > 0; --i)
            g_wh_ui_live_.panelItems[i - 1]->HideDeactivate();
        g_wh_ui_live_.expanded = false;
        g_wh_ui_live_.flowActive = false;
        WarehouseUiRestoreSaveUnder_();
        if (redraw) dlg->Redraw();
        LogInfo("仓库UI: 已关闭仓库层，背包恢复原生交互");
        return;
    }
    if (!WarehouseUiCaptureSaveUnder_(dlg)) {
        LogError("仓库UI: 无法保存仓库区域底图，取消展开");
        return;
    }
    g_wh_ui_live_.expanded = true;
    for (std::size_t i = 0; i < g_wh_ui_live_.panelItemCount; ++i)
        g_wh_ui_live_.panelItems[i]->ShowActivate();
    // 面板可见后再画外框阴影（原生 +0x6620 右/底暗化条），
    // 阴影已包含在 save-under 的 kWhUiPanelSaveExtra_ 边距内。
    if (g_wh_ui_shadow_ != 0 && g_wh_ui_live_.layout.fits) {
        reinterpret_cast<void (__stdcall *)(int, int, int, int)>(
            g_wh_ui_shadow_)(g_wh_ui_live_.layout.warehouse.x,
            g_wh_ui_live_.layout.warehouse.y,
            g_wh_ui_live_.layout.warehouse.width,
            g_wh_ui_live_.layout.warehouse.height);
    }
    WarehouseUiRefresh_(dlg, false);
    if (redraw) dlg->Redraw();
    LogInfo("仓库UI: 已展开置顶仓库层 dialog=%p", dlg);
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

    warehouse::NativeBackpack native;
    WarehouseUiCopyNative_(hero, native);
    const int count = hero->backpackCount;
    const std::uint64_t token = g_warehouse_ui_session_.BeginAttach();
    if (!g_warehouse_ui_session_.Activate(token)) return;

    g_wh_ui_live_ = {};
    g_wh_ui_live_.dialog = dlg;
    g_wh_ui_live_.hero = hero;
    g_wh_ui_live_.originalCallback = reinterpret_cast<WarehouseCallback_>(
        *reinterpret_cast<UINT32*>(reinterpret_cast<BYTE*>(dlg) + 0x70));
    g_wh_ui_live_.token = token;
    g_wh_ui_live_.layout = layout;
    g_wh_ui_live_.flow = warehouseui::Flow(native, count, hero->id, hero->owner,
        g_warehouse_.mutationEpoch, token);

    // 仓库入口属于原生背包同一 modal，默认可见；仓库面板控件稍后统一隐藏。
    g_wh_ui_live_.openButton = H3DlgCaptionButton::Create(
        originX + kWhUiOpenX_, originY + kWhUiOpenY_,
        kWhUiButtonWidth_, kWhUiButtonHeight_, kWhUiOpenId_,
        "", kWhUiGbkWarehouse_, "bigfont.fnt", 0, 0, FALSE, 0, 0);
    if (g_wh_ui_live_.openButton)
        WarehouseUiAdd_(dlg, 0, 0, g_wh_ui_live_.openButton);

    // 完整复用扩展窗口实际背景生成器，保留其 HD 32bit 画布/玩家色/底部状态凹槽。
    // 第三轮错误：把256x256源图的GetPcx当作405x523可写画布；已彻底删除该路径。
    H3DlgPcx16* panelBack = g_wh_ui_skin_factory_ ? g_wh_ui_skin_factory_(
        layout.warehouse.x, layout.warehouse.y, warehouseui::kPanelWidth,
        warehouseui::kPanelHeight, kWhUiPanelBackId_, 1, hero->owner) : nullptr;
    H3LoadedPcx16* panelPixels = panelBack ? panelBack->GetPcx() : nullptr;
    const bool panelBackOk = panelPixels && panelPixels->buffer
        && panelPixels->width == warehouseui::kPanelWidth
        && panelPixels->height == warehouseui::kPanelHeight;
    if (panelBackOk) {
        WarehouseUiAddPanelItem_(dlg, panelBack);
        LogInfo("仓库UI: HD整窗背景 factory=0x0FADB0 canvas=%dx%d stride=%d color=%d",
            panelPixels->width, panelPixels->height, panelPixels->scanlineSize, hero->owner);
    } else {
        LogError("仓库UI: HD整窗背景生成失败，不宣称底板已生效");
        H3DlgFrame* frame = H3DlgFrame::Create(layout.warehouse.x,
            layout.warehouse.y, layout.warehouse.width, layout.warehouse.height,
            kWhUiFrameId_, static_cast<RGB565>(kWhUiFrameColor_));
        if (WarehouseUiOwns_(dlg, frame)) {
            static_cast<H3DlgItem*>(frame)->ShowActivate();
        } else {
            WarehouseUiAddPanelItem_(dlg, frame);
        }
    }
    // 格子背景：与完整背包相同的 BkPack.pcx（369x369 @ 网格原点），叠在底板上。
    H3DlgPcx* backdrop = H3DlgPcx::Create(
        layout.warehouse.x + warehouseui::kGridOriginDX,
        layout.warehouse.y + warehouseui::kGridOriginDY,
        kWhUiBackdropId_, kWhUiBackdropPcx_);
    const bool backdropOk = backdrop && backdrop->GetPcx() != nullptr;
    if (backdropOk) {
        WarehouseUiAddPanelItem_(dlg, backdrop);
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
        WarehouseUiAddPanelItem_(dlg, g_wh_ui_live_.title);
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
        WarehouseUiAddPanelItem_(dlg, slot);
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
        WarehouseUiAddPanelItem_(dlg, g_wh_ui_live_.count);
    }
    H3DlgText* prev = H3DlgText::Create(layout.warehouse.x + 8,
        footerY, 52, warehouseui::kPagerRowHeight, "<", NH3Dlg::Text::SMALL,
        eTextColor::REGULAR, kWhUiPrevId_, eTextAlignment::MIDDLE_CENTER, 0);
    H3DlgText* next = H3DlgText::Create(layout.warehouse.x + layout.warehouse.width - 60,
        footerY, 52, warehouseui::kPagerRowHeight, ">", NH3Dlg::Text::SMALL,
        eTextColor::REGULAR, kWhUiNextId_, eTextAlignment::MIDDLE_CENTER, 0);
    WarehouseUiAddPanelItem_(dlg, prev);
    WarehouseUiAddPanelItem_(dlg, next);
    H3DlgTransparentItem* prevHit = H3DlgTransparentItem::Create(
        layout.warehouse.x + 8, footerY, 52, warehouseui::kPagerRowHeight,
        kWhUiPrevId_);
    H3DlgTransparentItem* nextHit = H3DlgTransparentItem::Create(
        layout.warehouse.x + layout.warehouse.width - 60,
        footerY, 52, warehouseui::kPagerRowHeight, kWhUiNextId_);
    WarehouseUiAddPanelItem_(dlg, prevHit);
    WarehouseUiAddPanelItem_(dlg, nextHit);
    // 仓库关闭按钮只属于仓库层：默认隐藏，展开后才能点。
    g_wh_ui_live_.closeButton = H3DlgCaptionButton::Create(
        layout.warehouse.x + layout.warehouse.width - kWhUiButtonWidth_ - 8,
        layout.warehouse.y + kWhUiOpenY_,
        kWhUiButtonWidth_, kWhUiButtonHeight_, kWhUiCloseId_,
        "", kWhUiGbkClose_, "bigfont.fnt", 0, 0, FALSE, 0, 0);
    if (g_wh_ui_live_.closeButton)
        WarehouseUiAddPanelItem_(dlg, g_wh_ui_live_.closeButton);

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
    const std::uint64_t token = g_wh_ui_live_.token;
    *reinterpret_cast<UINT32*>(reinterpret_cast<BYTE*>(dlg) + 0x70) =
        reinterpret_cast<UINT32>(g_wh_ui_live_.originalCallback);
    WarehouseUiRestoreSaveUnder_();
    g_wh_ui_live_ = {};
    g_warehouse_ui_session_.BeginClose(token, warehouseui::CloseReason::ExternalClose_);
    g_warehouse_ui_session_.FinishClose(token);
    LogInfo("仓库UI: detached token=%llu", static_cast<unsigned long long>(token));
}

static void WarehouseUiNeutralizeMessage_(H3Msg* msg) noexcept
{
    if (!msg) return;
    // command=4 未进入原生点击/关闭分支；保留 common/network/timeout 处理，
    // 同时让原生 callback 继续恰好执行一次。
    msg->command = eMsgCommand::MOUSE_OVER;
    msg->subtype = eMsgSubtype::NONE;
    msg->itemId = 0;
    msg->parameter = nullptr;
}

// 双层交互约定（用户 2026-10-13 裁定）：
//  1) 背包扩展 modal 打开时只显示“仓库”按钮，仓库层默认隐藏。
//  2) 点“仓库”按钮才展开仓库层：展开前先从 GetDrawBuffer() 拷贝
//     面板+阴影区域做 save-under，展开期间两层都可点击（同一 owned-items）。
//  3) 仓库打开时 Esc 只收起仓库层；仓库关闭时 Esc 走原生关闭路径。
//  4) 仓库打开时，面板外的左键按下/点击不结束背包 modal：
//     消费后把消息改成 MOUSE_OVER 再放行原生 callback（common/network/timeout
//     仍完整执行，右键/Esc 之外的原生命令不受影响）。
static int __stdcall WarehouseUiCallback_(void* self, H3Msg* msg)
{
    H3BaseDlg* dlg = static_cast<H3BaseDlg*>(self);
    WarehouseCallback_ original = nullptr;
    bool consume = false;
    __try {
        if (g_wh_ui_live_.dialog == dlg)
            original = g_wh_ui_live_.originalCallback;
        consume = WarehouseUiHandle_(dlg, msg);
        if (!consume && g_wh_ui_live_.dialog == dlg
            && g_wh_ui_live_.expanded && msg) {
            consume = warehouseui::SuppressPinnedClose(
                true, static_cast<int>(msg->command),
                static_cast<int>(msg->subtype), msg->itemId);
        }
        if (consume) WarehouseUiNeutralizeMessage_(msg);
    } __except (GuardCrashFilter_(GUARD_WH_UI_POST_COMMON, GetExceptionInformation())) {}
    // 原生 callback 永远在自有逻辑之外、恰好一次；异常不吞原版路径。
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
    g_wh_ui_skin_factory_ = reinterpret_cast<WarehouseSkinFactory_>(base + 0x0FADB0);
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
