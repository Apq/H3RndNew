// ========== WarehouseUiStageB.inc.cpp ==========
// 阶段 B/C/D：同一完整背包 modal 的 owned-items 仓库外壳和事务接线。
// 仅安装在 WarehouseUiStageA 的文件/内存指纹全部通过时。

#include "WarehouseUiFlow.hpp"
#include "WarehouseUiCallback.hpp"

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

static constexpr std::size_t kWhUiPanelItemCapacity_ = 320;

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
    H3DlgItem* openButton = nullptr;
    H3DlgItem* closeButton = nullptr;
    H3DlgItem* takeAllButton = nullptr;
    H3DlgItem* putAllButton = nullptr;
    H3DlgItem* title = nullptr;
    H3DlgText* titleFallback = nullptr;
    H3DlgText* pagerText[warehouseui::kPagerButtonCount] = {};
    H3DlgDef* slots[kWhUiSlotCount_] = {};
    H3DlgItem* panelItems[kWhUiPanelItemCapacity_] = {};
    std::size_t panelItemCount = 0;
    std::vector<std::size_t> filtered;
    // 普通/卷轴控件各自拥有资源；原生旧图标保留但停用，不交换资源指针。
    H3DlgDef* nativeIcons[kWhUiSlotCount_][2] = {};
    H3DlgItem* originalNativeIcons[kWhUiSlotCount_] = {};
    bool nativeIconsReady = false;
    bool scrollDefEnabled = false;
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
static const int* g_wh_ui_scroll_enabled_ = nullptr;
static constexpr int kWhUiPanelSaveExtra_ = 8;
static const char kWhUiGbkWarehouse_[] = {
    static_cast<char>(0xB2), static_cast<char>(0xD6),
    static_cast<char>(0xBF), static_cast<char>(0xE2), 0x00 };
static const char kWhUiGbkClose_[] = {
    static_cast<char>(0xB9), static_cast<char>(0xD8),
    static_cast<char>(0xB1), static_cast<char>(0xD5), 0x00 };
static const char kWhUiGbkTakeAll_[] = {
    static_cast<char>(0xD2), static_cast<char>(0xBB), static_cast<char>(0xBC),
    static_cast<char>(0xFC), static_cast<char>(0xC8), static_cast<char>(0xA1),
    static_cast<char>(0xB3), static_cast<char>(0xF6), 0x00 };
static const char kWhUiGbkPutAll_[] = {
    static_cast<char>(0xD2), static_cast<char>(0xBB), static_cast<char>(0xBC),
    static_cast<char>(0xFC), static_cast<char>(0xB7), static_cast<char>(0xC5),
    static_cast<char>(0xC8), static_cast<char>(0xEB), 0x00 };
// 交互接点反汇编结论（见 H3Note 完整背包笔记）：
//  0x0395D0..0x0396AB 为 common/network/timeout 前置处理，结果在 [ebp-0x28]；
//  本模块通过替换 dlg+0x70 的 callback 指针接入，包装器先跑自有分支再调
//  原生 callback 恰好一次，保证原版路径不被吞掉（右键/Esc/网络/超时原样透传）。
static constexpr UINT32 kWhUiPostCommonRva_ = 0x0396AB; // 反汇编参考锚点
static LoHook* g_wh_ui_post_common_hook_ = nullptr;    // 预留：暂不安装
// 第五轮控件几何：按钮/页签素材常量由 WarehouseUiButtons.inc.cpp 提供。
static constexpr int kWhUiTabY_ = 54;
static constexpr int kWhUiTakeAllId_ = 30201;
static constexpr int kWhUiPutAllId_ = 30202;
// 分页栏页码按钮（round-7）：30400..30403。
static constexpr int kWhUiPagerBase_ = 30400;
// 布局偏移配置（round-7 用户裁定入配置文件 [WarehouseUI]，两层ini可覆盖）。
struct WarehouseUiLayoutConfig_ {
    int openDX;    // 仓库按钮距背包界面左上角
    int openDY;
    int closeDX;   // 关闭X距仓库界面右上角（水平缩进）
    int closeDY;   // 关闭X距仓库界面顶部
    int bulkLeftDX;  // 一键放入距仓库左边
    int bulkRightDX; // 一键取出距仓库右边
    int bulkDY;      // 两个一键按钮距仓库顶部
};
static WarehouseUiLayoutConfig_ g_wh_ui_layout_cfg_ = { 8, 18, 0, 0, 8, 8, 50 };
static int WarehouseUiClampOffset_(int value, int low, int high,
                                   int fallback) noexcept {
    return value >= low && value <= high ? value : fallback;
}
static void WarehouseUiLoadLayoutConfig_() noexcept {
    __try {
        const WarehouseUiLayoutConfig_ current = g_wh_ui_layout_cfg_;
        WarehouseUiLayoutConfig_ cfg = current;
        cfg.openDX = WarehouseUiClampOffset_(
            IniReadIntUtf8Layered("WarehouseUI", "OpenButtonDX", cfg.openDX), 0, 330, current.openDX);
        cfg.openDY = WarehouseUiClampOffset_(
            IniReadIntUtf8Layered("WarehouseUI", "OpenButtonDY", cfg.openDY), 0, 420, current.openDY);
        cfg.closeDX = WarehouseUiClampOffset_(
            IniReadIntUtf8Layered("WarehouseUI", "CloseButtonDX", cfg.closeDX), 0, 380, current.closeDX);
        cfg.closeDY = WarehouseUiClampOffset_(
            IniReadIntUtf8Layered("WarehouseUI", "CloseButtonDY", cfg.closeDY), 0, 440, current.closeDY);
        cfg.bulkLeftDX = WarehouseUiClampOffset_(
            IniReadIntUtf8Layered("WarehouseUI", "BulkButtonLeftDX", cfg.bulkLeftDX), 0, 330, current.bulkLeftDX);
        cfg.bulkRightDX = WarehouseUiClampOffset_(
            IniReadIntUtf8Layered("WarehouseUI", "BulkButtonRightDX", cfg.bulkRightDX), 0, 330, current.bulkRightDX);
        cfg.bulkDY = WarehouseUiClampOffset_(
            IniReadIntUtf8Layered("WarehouseUI", "BulkButtonDY", cfg.bulkDY), 30, 440, current.bulkDY);
        g_wh_ui_layout_cfg_ = cfg;
        LogInfo("仓库UI: 布局配置 open=%d,%d close=%d,%d bulk=%d,%d,%d",
            cfg.openDX, cfg.openDY, cfg.closeDX, cfg.closeDY,
            cfg.bulkLeftDX, cfg.bulkRightDX, cfg.bulkDY);
    } __except (GuardCrashFilter_(GUARD_WH_UI_MODAL, GetExceptionInformation())) {
        // 读取失败保持现值（默认或上次成功值），不阻断接线。
    }
}
static constexpr int kWhUiButtonWidth_ = 66;
static constexpr int kWhUiButtonHeight_ = 32;
static constexpr int kWhUiCloseButtonWidth_ = 24;
static constexpr int kWhUiCloseButtonHeight_ = 24;
static void WarehouseUiSetExpanded_(H3BaseDlg* dlg, bool expanded, bool redraw) noexcept;
static void WarehouseUiDetach_(H3BaseDlg* dlg) noexcept;
static void WarehouseUiRefreshNativeIcons_(H3BaseDlg* dlg) noexcept;
static void WarehouseUiSetNativeIconLayer_(H3BaseDlg* dlg, bool warehouseLayer) noexcept;
static int __stdcall WarehouseUiCallback_(void* self, H3Msg* msg);
static void WarehouseUiCopyNative_(H3Hero* hero,
    warehouse::NativeBackpack& native) noexcept;

// 当前页签命中的仓库记录在 records 中的下标。
static void WarehouseUiRebuildFilter_(void) noexcept {
    auto& live = g_wh_ui_live_;
    live.filtered.clear();
    if (!live.hero) return;
    const warehouseui::TabKind tab = WarehouseUiActiveTabKind_();
    const auto& records = g_warehouse_.heroes[
        static_cast<std::size_t>(live.hero->id)];
    try {
        for (std::size_t i = 0; i < records.size(); ++i) {
            const int id = records[i].artifactId;
            if (id < 0) continue;
            if (warehouseui::TabMatches(tab, id, WarehouseUiArtifactSlot_(id)))
                live.filtered.push_back(i);
        }
    } catch (...) {
        live.filtered.clear();
    }
}

// 背包侧：一键放入的可入仓掩码（round-7 用户裁定：批量操作固定为第一个
// Tab“常规”的全量语义，不随当前页签变化）。
static bool WarehouseUiBackpackFilter_(std::vector<char>& include) noexcept {
    try {
        include.assign(warehouse::kBackpackCapacity, 0);
    } catch (...) {
        return false;
    }
    if (!g_wh_ui_live_.hero) return false;
    warehouse::NativeBackpack native;
    WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
    for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i) {
        const int id = native[i].artifactId;
        if (id < 0) continue;
        include[i] = warehouseui::TabMatches(warehouseui::TabKind::Regular, id,
            WarehouseUiArtifactSlot_(id)) ? 1 : 0;
    }
    return true;
}

static bool WarehouseUiBackpackHasMatch_() noexcept {
    std::vector<char> include;
    if (!WarehouseUiBackpackFilter_(include)) return false;
    for (char item : include) if (item) return true;
    return false;
}

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
    WarehouseUiRebuildFilter_();
    const std::size_t pages = warehouseui::PageCount(g_wh_ui_live_.filtered.size());
    if (pages == 0) g_wh_ui_live_.page = 0;
    else if (g_wh_ui_live_.page >= pages) g_wh_ui_live_.page = pages - 1;
    const warehouseui::PageView view = warehouseui::MakePageView(
        g_wh_ui_live_.filtered.size(), g_wh_ui_live_.page);
    if (g_wh_ui_live_.titleFallback) g_wh_ui_live_.titleFallback->SetText(kWhUiGbkWarehouse_);
    // 分页栏（round-7）：中间4个动态页码（当前页高亮），两侧箭头按可翻性换态。
    WarehouseUiRefreshPager_(dlg, g_wh_ui_live_.page, pages,
        g_wh_ui_live_.expanded);
    const std::size_t pagerBegin = WarehouseUiPagerBegin_();
    for (int i = 0; i < warehouseui::kPagerButtonCount; ++i) {
        H3DlgText* number = g_wh_ui_live_.pagerText[i];
        if (!number) continue;
        const std::size_t at = pagerBegin + static_cast<std::size_t>(i);
        if (at < pages) {
            char text[8];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%u",
                static_cast<unsigned>(at + 1));
            number->SetText(text);
            if (g_wh_ui_live_.expanded) number->ShowActivate();
            else number->HideDeactivate();
        } else {
            number->HideDeactivate();
        }
    }
    for (int i = 0; i < kWhUiSlotCount_; ++i) {
        H3DlgDef* slot = g_wh_ui_live_.slots[i];
        if (!slot) continue;
        const std::size_t at = view.begin + static_cast<std::size_t>(i);
        if (at < view.begin + view.count && at < g_wh_ui_live_.filtered.size()) {
            slot->SetFrame(records[g_wh_ui_live_.filtered[at]].artifactId);
            if (g_wh_ui_live_.expanded) slot->ShowActivate();
            else slot->HideDeactivate();
        } else {
            slot->HideDeactivate();
        }
    }
    // 一键操作是全量语义（round-7）：取出只看仓库是否全空/背包是否满。
    const bool takeDisabled = records.empty()
        || g_wh_ui_live_.hero->backpackCount >= 64 || WarehouseUiHeld_() || g_wh_locked_ != 0;
    const bool putDisabled = !WarehouseUiBackpackHasMatch_()
        || WarehouseUiHeld_() || g_wh_locked_ != 0;
    if (WarehouseUiOwns_(dlg, g_wh_ui_live_.putAllButton))
        g_wh_ui_live_.putAllButton->EnableItem(!putDisabled);
    if (WarehouseUiOwns_(dlg, g_wh_ui_live_.takeAllButton))
        g_wh_ui_live_.takeAllButton->EnableItem(!takeDisabled);
    WarehouseUiSetButtonDisabled_(3, putDisabled);
    WarehouseUiSetButtonDisabled_(2, takeDisabled);
    WarehouseUiRefreshTabs_(dlg, false);
    // Attach 阶段不重绘，也不让页签刷新重新显示已隐藏的面板控件。
    if (!g_wh_ui_live_.expanded) {
        for (std::size_t i = 0; i < g_wh_ui_live_.panelItemCount; ++i)
            g_wh_ui_live_.panelItems[i]->HideDeactivate();
    }
    if (redraw) dlg->Redraw();
}

static void WarehouseUiPrepareNativeIcons_(H3BaseDlg* dlg) noexcept
{
    if (!dlg || !g_wh_ui_live_.hero) return;
    g_wh_ui_live_.nativeIconsReady = false;
    g_wh_ui_live_.scrollDefEnabled = g_wh_ui_scroll_enabled_
        && *g_wh_ui_scroll_enabled_ != 0;
    for (H3DlgItem* item : dlg->GetList()) {
        if (!item) continue;
        const int itemId = item->GetID();
        // 原生命中项 1000..1063 是每格真实几何来源（覆盖图标必须与原生网格
        // 像素对齐，不能靠常量推导——第六轮曾误用仓库网格原点 82 导致背包
        // 物品整体下移 32px 与网格错位）。
        if (itemId >= 1000 && itemId < 1064) continue;
        if (itemId < 2000 || itemId >= 2064) continue;
        const int slot = itemId - 2000;
        if (g_wh_ui_live_.originalNativeIcons[slot]) continue; // 重复ID只保留首个
        if (item->Cast<H3DlgDef>() == nullptr) {
            // 原生此ID必须是DEF；否则不接管，继续走原生路径，防止误用。
            g_wh_ui_live_.nativeIconsReady = false;
            return;
        }
        g_wh_ui_live_.originalNativeIcons[slot] = item;
    }
    // 先收一次原生网格命中项（全部64格都存在），作为覆盖图标的定位真源。
    H3DlgItem* nativeHits[kWhUiSlotCount_] = {};
    for (H3DlgItem* item : dlg->GetList()) {
        if (!item) continue;
        const int itemId = item->GetID();
        if (itemId < 1000 || itemId >= 1064) continue;
        const int slot = itemId - 1000;
        if (!nativeHits[slot]) nativeHits[slot] = item;
    }
    for (int i = 0; i < kWhUiSlotCount_; ++i) {
        const int col = i % static_cast<int>(warehouseui::kGridColumns);
        const int row = i / static_cast<int>(warehouseui::kGridColumns);
        // 命中项存在则用其实际坐标（+2,+1 与原生 DEF 图标一致）；
        // 缺失才回退常量推导，且回退必须用背包原点 50（无Tab行偏移）。
        const int x = nativeHits[i]
            ? nativeHits[i]->GetX() + warehouseui::kGridDefDX
            : g_wh_ui_live_.layout.backpack.x + warehouseui::kGridOriginDX
                + col * warehouseui::kGridPitch + warehouseui::kGridDefDX;
        const int y = nativeHits[i]
            ? nativeHits[i]->GetY() + warehouseui::kGridDefDY
            : g_wh_ui_live_.layout.backpack.y + warehouseui::kBackpackGridOriginDY
                + row * warehouseui::kGridPitch + warehouseui::kGridDefDY;
        for (int kind = 0; kind < 2; ++kind) {
            H3DlgDef* icon = H3DlgDef::Create(x, y, warehouseui::kGridDefSize,
                warehouseui::kGridDefSize, 2000 + i,
                kind == 1 ? NH3Dlg::HDassets::SCROLL_SMALL : NH3Dlg::Assets::ARTIFACT_DEF,
                0, 0, FALSE, FALSE);
            if (!icon) {
                // 半途失败必须清掉本轮已经加入的对象，防止只在own-list中的
                // 悬垂指针继续参与item遍历/hit/析构。
                for (int created = 0; created < kWhUiSlotCount_; ++created)
                    for (int k = 0; k < 2; ++k) {
                        H3DlgDef* item = g_wh_ui_live_.nativeIcons[created][k];
                        g_wh_ui_live_.nativeIcons[created][k] = nullptr;
                        if (WarehouseUiOwns_(dlg, item)) item->HideDeactivate();
                    }
                for (H3DlgItem* item : g_wh_ui_live_.originalNativeIcons)
                    if (WarehouseUiOwns_(dlg, item)) item->ShowActivate();
                g_wh_ui_live_.nativeIconsReady = false;
                return;
            }
            dlg->AddItem(icon);
            icon->HideDeactivate();
            g_wh_ui_live_.nativeIcons[i][kind] = icon;
        }
    }
    g_wh_ui_live_.nativeIconsReady = true;
    WarehouseUiSetNativeIconLayer_(dlg, g_wh_ui_live_.expanded);
}

static void WarehouseUiRefreshNativeIcons_(H3BaseDlg* dlg) noexcept
{
    if (!dlg || !g_wh_ui_live_.hero || !g_wh_ui_live_.nativeIconsReady) return;
    g_wh_ui_live_.scrollDefEnabled = g_wh_ui_scroll_enabled_
        && *g_wh_ui_scroll_enabled_ != 0;
    for (int i = 0; i < kWhUiSlotCount_; ++i) {
        const auto& record = g_wh_ui_live_.hero->backpackArtifacts[i];
        const auto view = warehouseui::MakeIconView(record.id, record.subtype,
            g_wh_ui_live_.scrollDefEnabled);
        for (int kind = 0; kind < 2; ++kind) {
            H3DlgDef* icon = g_wh_ui_live_.nativeIcons[i][kind];
            if (!WarehouseUiOwns_(dlg, icon)) continue;
            if (view.visible && view.kind == kind) {
                icon->SetFrame(view.frame);
                icon->ShowActivate();
            } else icon->HideDeactivate();
        }
    }
}

static void WarehouseUiSetNativeIconLayer_(H3BaseDlg* dlg, bool) noexcept
{
    if (!dlg || !g_wh_ui_live_.nativeIconsReady) return;
    // 自有覆盖层在整个 modal 生命周期内作为唯一可见图标层。SetFrame 不会
    // 替换 loadedDef；重新启用原件会在转移后显示旧帧，且无法处理“普通宝物↔卷轴”资源切换。
    for (H3DlgItem* item : g_wh_ui_live_.originalNativeIcons)
        if (WarehouseUiOwns_(dlg, item)) item->HideDeactivate();
    WarehouseUiRefreshNativeIcons_(dlg);
}

static void WarehouseUiRefreshAfterMove_(H3BaseDlg* dlg) noexcept
{
    if (!dlg || g_wh_ui_live_.dialog != dlg) return;
    g_wh_ui_live_.flowActive = false;
    g_wh_ui_live_.flow = {};
    WarehouseUiRefreshNativeIcons_(dlg);
    WarehouseUiRefresh_(dlg, true);
}

// 落点无关的放入提交（round-7 用户裁定：移入仓库不必精确落点，只要在
// 目标界面内点击且仓库有空位即自动追加；先选背包道具再点仓库任意处完成）。
static void WarehouseUiCommitDeposit_(H3BaseDlg* dlg) noexcept
{
    if (!dlg || g_wh_ui_live_.dialog != dlg || !g_wh_ui_live_.hero
        || !g_wh_ui_live_.flowActive)
        return;
    warehouse::NativeBackpack native;
    WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
    int count = g_wh_ui_live_.hero->backpackCount;
    const warehouseui::Result result = g_wh_ui_live_.flow.Deposit(
        g_warehouse_, native, count, g_wh_ui_live_.hero->id,
        g_wh_ui_live_.hero->owner, g_wh_ui_live_.token,
        g_wh_locked_ != 0, WarehouseUiHeld_());
    if (result == warehouseui::Result::Ok) {
        WarehouseUiWriteNative_(g_wh_ui_live_.hero, native, count);
        LogInfo("仓库UI: 放入成功(界面内任意落点) hero=%d slot=%d count=%d",
            g_wh_ui_live_.hero->id,
            static_cast<int>(g_wh_ui_live_.flow.SelectedIndex()), count);
        WarehouseUiRefreshAfterMove_(dlg);
    } else {
        LogInfo("仓库UI: 放入拒绝 result=%d hero=%d",
            static_cast<int>(result), g_wh_ui_live_.hero->id);
    }
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
    if (msg->command == eMsgCommand::MOUSE_OVER) {
        WarehouseUiUpdateHover_(dlg, msg);
        return false;
    }
    if (msg->command != eMsgCommand::ITEM_COMMAND)
        return false;
    const int id = msg->itemId;
    const int heroId = g_wh_ui_live_.hero->id;
    const int ownerId = g_wh_ui_live_.hero->owner;
    const bool leftDown = msg->subtype == eMsgSubtype::LBUTTON_DOWN;
    const bool leftClick = msg->subtype == eMsgSubtype::LBUTTON_CLICK;
    if (!leftDown && !leftClick) return false;
    // DOWN只中和，不做事务；所有仓库动作只在 CLICK 运行一次。
    if (leftDown) {
        // 展开态：覆盖层宝物图标（2000..2063）的按下由本模块接管；
        // 原生槽命中区 1000..1063 的按下仍透传，保持原版按压/提示路径。
        if (g_wh_ui_live_.expanded && id >= 2000 && id < 2064) return true;
        return warehouseui::ConsumeWarehouseDown(id);
    }
    // item状态机已在callback之前处理按下；只中和callback的未知ID关闭分支。
    // 按钮及页签动作只在释放CLICK执行一次。
    if (!g_wh_ui_live_.expanded && id != kWhUiOpenId_) return false;
    if (!warehouseui::ExecutesWarehouseAction(static_cast<int>(msg->subtype))) return true;
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
    if (id >= 2000 && id < 2064) {
        // 展开态下宝物图标（原生 DEF id=2000..2063，覆盖层同 ID）就是背包侧
        // 选择来源；不中和就会落到原生“未知 ID”关闭分支。
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
    // 放入落点无关（round-7）：选中背包道具后，点仓库面板任意背景
    // （底板/边框/标题）同样提交放入；页签与按钮保持各自语义。
    if (g_wh_ui_live_.flowActive
        && (id == kWhUiPanelBackId_ || id == kWhUiBackdropId_
            || id == kWhUiFrameId_ || id == kWhUiTitleId_)) {
        WarehouseUiCommitDeposit_(dlg);
        return true;
    }
    if (id >= kWhUiSlotBase_ && id < kWhUiSlotBase_ + kWhUiSlotCount_) {
        const int local = id - kWhUiSlotBase_;
        const warehouseui::PageView view = warehouseui::MakePageView(
            g_wh_ui_live_.filtered.size(), g_wh_ui_live_.page);
        const std::size_t at = view.begin + static_cast<std::size_t>(local);
        warehouse::NativeBackpack native;
        WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
        int count = g_wh_ui_live_.hero->backpackCount;
        if (g_wh_ui_live_.flowActive) {
            WarehouseUiCommitDeposit_(dlg);
            return true;
        }
        if (at >= view.begin + view.count || at >= g_wh_ui_live_.filtered.size())
            return true;
        const std::size_t recordIndex = g_wh_ui_live_.filtered[at];
        warehouseui::Flow retrieve(native, count, heroId, ownerId,
            g_warehouse_.mutationEpoch, g_wh_ui_live_.token);
        if (!retrieve.IsValid()) return true;
        const warehouseui::Result result = retrieve
            .Retrieve(recordIndex, g_warehouse_, native, count, heroId, ownerId,
                g_wh_ui_live_.token, g_wh_locked_ != 0, WarehouseUiHeld_());
        if (result == warehouseui::Result::Ok) {
            WarehouseUiWriteNative_(g_wh_ui_live_.hero, native, count);
            LogInfo("仓库UI: 取回成功 hero=%d index=%u count=%d", heroId,
                static_cast<unsigned>(recordIndex), count);
            WarehouseUiRefreshAfterMove_(dlg);
        } else {
            LogInfo("仓库UI: 取回拒绝 result=%d hero=%d", static_cast<int>(result), heroId);
        }
        return true;
    }
    if (id == kWhUiPrevId_ || id == kWhUiNextId_) {
        const std::size_t pages = warehouseui::PageCount(
            g_wh_ui_live_.filtered.size());
        if (pages != 0) {
            if (id == kWhUiPrevId_ && g_wh_ui_live_.page > 0)
                --g_wh_ui_live_.page;
            if (id == kWhUiNextId_ && g_wh_ui_live_.page + 1 < pages)
                ++g_wh_ui_live_.page;
            WarehouseUiRefresh_(dlg, true);
        }
        return true;
    }
    // 页码直跳（round-7）：30400..30403 对应当前页码窗口的第1..4页。
    if (id >= kWhUiPagerBase_
        && id < kWhUiPagerBase_ + warehouseui::kPagerButtonCount) {
        const std::size_t pages = warehouseui::PageCount(
            g_wh_ui_live_.filtered.size());
        const std::size_t at = WarehouseUiPagerBegin_()
            + static_cast<std::size_t>(id - kWhUiPagerBase_);
        if (at < pages && at != g_wh_ui_live_.page) {
            g_wh_ui_live_.page = at;
            WarehouseUiRefresh_(dlg, true);
        }
        return true;
    }
    if (id == kWhUiOpenId_) {
        WarehouseUiSetExpanded_(dlg, true, true);
        return true;
    }
    if (id == kWhUiCloseId_) {
        WarehouseUiSetExpanded_(dlg, false, true);
        return true;
    }
    // 页签：左/右滚动只改窗口偏移；点页签改活动分类并回到第一页。
    const int tabCommand = WarehouseUiTabCommand_(id);
    if (tabCommand >= 0) {
        if (tabCommand == 1) g_wh_ui_live_.page = 0;
        g_wh_ui_live_.flowActive = false;
        WarehouseUiRefresh_(dlg, true);
        return true;
    }
    if (id == kWhUiTakeAllId_ || id == kWhUiPutAllId_) {
        if (!g_wh_ui_live_.expanded) return true;
        if (WarehouseUiHeld_() || g_wh_locked_ != 0) return true;
        warehouse::NativeBackpack native;
        WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
        int count = g_wh_ui_live_.hero->backpackCount;
        g_wh_ui_live_.flow = warehouseui::Flow(native, count, heroId, ownerId,
            g_warehouse_.mutationEpoch, g_wh_ui_live_.token);
        if (!g_wh_ui_live_.flow.IsValid()) return true;
        std::size_t moved = 0;
        warehouseui::Result result = warehouseui::Result::InvalidIndex;
        if (id == kWhUiTakeAllId_) {
            // 取出是全量语义（round-7）：不受当前页签过滤，仓库内全部记录参与。
            auto& records = g_warehouse_.heroes[static_cast<std::size_t>(heroId)];
            std::vector<char> include;
            try {
                include.assign(records.size(), 1);
            } catch (...) {
                return true;
            }
            result = g_wh_ui_live_.flow.RetrieveAll(g_warehouse_, native, count,
                heroId, ownerId, g_wh_ui_live_.token, g_wh_locked_ != 0,
                WarehouseUiHeld_(), include, moved);
        } else {
            std::vector<char> include;
            if (!WarehouseUiBackpackFilter_(include)) return true;
            result = g_wh_ui_live_.flow.DepositAll(g_warehouse_, native, count,
                heroId, ownerId, g_wh_ui_live_.token, g_wh_locked_ != 0,
                WarehouseUiHeld_(), include, moved);
        }
        g_wh_ui_live_.flowActive = false;
        LogInfo("仓库UI: 批量操作 id=%d tab=%d result=%d moved=%u hero=%d",
            id, WarehouseUiActiveTab_(), static_cast<int>(result),
            static_cast<unsigned>(moved), heroId);
        if (result == warehouseui::Result::Ok && moved != 0) {
            WarehouseUiWriteNative_(g_wh_ui_live_.hero, native, count);
            WarehouseUiRefreshAfterMove_(dlg);
        } else {
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
        WarehouseUiSetNativeIconLayer_(dlg, false);
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
    WarehouseUiSetNativeIconLayer_(dlg, true);
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
    // 布局只做几何规划，从未移动原生背包控件（本模块没有任何 SetX/SetY 平移
    // 原生背景/标题/状态条/命中项）。因此只有当规划出的背包位置与原生实际
    // 位置完全一致时才能接线；否则面板会与原背包重叠，两层命中项同处一个
    // dialog，点击会落到错误的道具上。此时 fail closed，保留原生背包。
    if (!warehouseui::LayoutPreservesBackpackPosition(layout, originX, originY)) {
        LogError("仓库UI: 布局要求移动原生背包(%d,%d)->(%d,%d)但无平移实现，拒绝接线",
            originX, originY, layout.backpack.x, layout.backpack.y);
        return;
    }

    warehouse::NativeBackpack native;
    WarehouseUiCopyNative_(hero, native);
    const int count = hero->backpackCount;
    const std::uint64_t token = g_warehouse_ui_session_.BeginAttach();
    if (!g_warehouse_ui_session_.Activate(token)) return;

    WarehouseUiResetTabs_();
    g_wh_ui_visual_dialog_ = dlg;
    g_wh_ui_live_ = {};
    g_wh_ui_live_.dialog = dlg;
    g_wh_ui_live_.hero = hero;
    g_wh_ui_live_.originalCallback = reinterpret_cast<WarehouseCallback_>(
        *reinterpret_cast<UINT32*>(reinterpret_cast<BYTE*>(dlg) + 0x70));
    g_wh_ui_live_.token = token;
    g_wh_ui_live_.layout = layout;
    g_wh_ui_live_.flow = warehouseui::Flow(native, count, hero->id, hero->owner,
        g_warehouse_.mutationEpoch, token);
    // 每次弹窗打开时重读布局偏移配置（round-7：用户可随时改 user.ini 生效，
    // 无需重启游戏）。
    WarehouseUiLoadLayoutConfig_();
    WarehouseUiPrepareNativeIcons_(dlg);
    if (!g_wh_ui_live_.nativeIconsReady) {
        LogError("仓库UI: 无法建立完整原生背包图标，保留原生模式");
        WarehouseUiSetNativeIconLayer_(dlg, false);
        WarehouseUiDetach_(dlg);
        return;
    }

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
    // 只在当前modal所属素材有效时加载自有PCX；图片缓存不保存dialog指针。
    if (backdropOk) WarehouseUiCacheImages_(backdrop);
    const int openX = originX + g_wh_ui_layout_cfg_.openDX;
    const int openY = originY + g_wh_ui_layout_cfg_.openDY;
    const WarehouseButtonPair_ open = WarehouseUiCreateButton_(0,
        openX, openY, kWhUiOpenId_);
    if (open.visual && open.def) {
        WarehouseUiAdd_(dlg, 0, 0, open.visual);
        WarehouseUiAdd_(dlg, 0, 0, open.def);
        g_wh_ui_live_.openButton = open.def;
    } else {
        g_wh_ui_live_.openButton = H3DlgCaptionButton::Create(openX, openY,
            kWhUiButtonWidth_, kWhUiButtonHeight_, kWhUiOpenId_,
            "", kWhUiGbkWarehouse_, "bigfont.fnt", 0, 0, FALSE, 0, 0);
        WarehouseUiAdd_(dlg, 0, 0, g_wh_ui_live_.openButton);
    }
    // 标题完全复用原生背包：405x30、bigfont、REGULAR、居中。
    g_wh_ui_live_.titleFallback = H3DlgText::Create(layout.warehouse.x,
        layout.warehouse.y + warehouseui::kTitleOffsetY,
        warehouseui::kPanelWidth, warehouseui::kTitleHeight,
        kWhUiGbkWarehouse_, kWhUiTitleFont_, eTextColor::REGULAR,
        kWhUiTitleId_, eTextAlignment::MIDDLE_CENTER, 0);
    g_wh_ui_live_.title = g_wh_ui_live_.titleFallback;
    WarehouseUiAddPanelItem_(dlg, g_wh_ui_live_.title);
    const int buttonIds[3] = { kWhUiCloseId_, kWhUiPutAllId_, kWhUiTakeAllId_ };
    const int visualKinds[3] = { 1, 3, 2 };
    for (int kind = 1; kind < 4; ++kind) {
        const int visualKind = visualKinds[kind - 1];
        const bool close = kind == 1;
        const int width = close ? kWhUiCloseButtonWidth_ : kWhUiButtonWidth_;
        const int height = close ? kWhUiCloseButtonHeight_ : kWhUiButtonHeight_;
        // round-7：关闭X距仓库右上角(closeDX/closeDY)、一键放入距左边(bulkLeftDX)、
        // 一键取出距右边(bulkRightDX)、两键距顶部(bulkDY)全部来自 [WarehouseUI]。
        const int x = close
            ? layout.warehouse.x + layout.warehouse.width - width - g_wh_ui_layout_cfg_.closeDX
            : kind == 2 ? layout.warehouse.x + g_wh_ui_layout_cfg_.bulkLeftDX
            : layout.warehouse.x + layout.warehouse.width - width - g_wh_ui_layout_cfg_.bulkRightDX;
        const int y = close
            ? layout.warehouse.y + g_wh_ui_layout_cfg_.closeDY
            : layout.warehouse.y + g_wh_ui_layout_cfg_.bulkDY;
        const WarehouseButtonPair_ pair = WarehouseUiCreateButton_(visualKind,
            x, y, buttonIds[kind - 1]);
        WarehouseUiAddPanelItem_(dlg, pair.visual);
        WarehouseUiAddPanelItem_(dlg, pair.def);
        H3DlgItem* button = pair.def;
        if (!button) {
            const char* caption = kind == 1 ? "X"
                : kind == 2 ? kWhUiGbkPutAll_ : kWhUiGbkTakeAll_;
            button = H3DlgCaptionButton::Create(x, y, width, height,
                buttonIds[kind - 1], "", caption, "smallfont.fnt", 0, 0, FALSE, 0, 0);
            WarehouseUiAddPanelItem_(dlg, button);
        }
        if (kind == 1) g_wh_ui_live_.closeButton = button;
        if (kind == 2) g_wh_ui_live_.putAllButton = button;
        if (kind == 3) g_wh_ui_live_.takeAllButton = button;
    }
    // 中间页签行：左右箭头 + 四个可见标题槽，12项可横向滚动。
    for (int i = 0; i < kWhUiTabSlotCount_; ++i) {
        const WarehouseTabPair_ pair = WarehouseUiCreateTab_(i,
            layout.warehouse.x + 102 + i * 48,
            layout.warehouse.y + kWhUiTabY_, 30300 + i);
        WarehouseUiAddPanelItem_(dlg, pair.label);
        WarehouseUiAddPanelItem_(dlg, pair.hit);
    }
    for (int i = 0; i < 2; ++i) {
        const WarehouseTabPair_ pair = WarehouseUiCreateArrow_(i,
            layout.warehouse.x + (i == 0 ? 80 : 311),
            layout.warehouse.y + kWhUiTabY_, 30310 + i);
        WarehouseUiAddPanelItem_(dlg, pair.label);
        WarehouseUiAddPanelItem_(dlg, pair.hit);
    }
    // 64 槽 = 8x8 网格，DEF 图标 44x44 与 HD 完整背包同构
    // （步距 46，格内偏移 +2,+1）。
    const int gridX = layout.warehouse.x + warehouseui::kGridOriginDX;
    const int gridY = layout.warehouse.y + warehouseui::kGridOriginDY;
    for (int i = 0; i < kWhUiSlotCount_; ++i) {
        const int col = i % static_cast<int>(warehouseui::kGridColumns);
        const int row = i / static_cast<int>(warehouseui::kGridColumns);
        H3DlgTransparentItem* hit = H3DlgTransparentItem::Create(
            gridX + col * warehouseui::kGridPitch,
            gridY + row * warehouseui::kGridPitch, 46, 46, kWhUiSlotBase_ + i);
        WarehouseUiAddPanelItem_(dlg, hit);
        H3DlgDef* slot = H3DlgDef::Create(
            gridX + col * warehouseui::kGridPitch + warehouseui::kGridDefDX,
            gridY + row * warehouseui::kGridPitch + warehouseui::kGridDefDY,
            warehouseui::kGridDefSize, warehouseui::kGridDefSize,
            kWhUiSlotBase_ + i, NH3Dlg::Assets::ARTIFACT_DEF, 0, 0, FALSE, FALSE);
        g_wh_ui_live_.slots[i] = slot;
        WarehouseUiAddPanelItem_(dlg, slot);
    }
    // 分页行（round-7）：两侧箭头按钮 + 中间4个页码按钮，纯数字、无文字。
    const int footerY = layout.warehouse.y + warehouseui::kPagerRowY
        + (warehouseui::kPagerRowHeight - warehouseui::kPagerButtonHeight) / 2;
    const int pagerTotal = warehouseui::kPagerButtonCount
        * warehouseui::kPagerButtonWidth
        + (warehouseui::kPagerButtonCount - 1) * warehouseui::kPagerButtonGap;
    const int pagerX = layout.warehouse.x
        + (layout.warehouse.width - pagerTotal) / 2;
    for (int i = 0; i < warehouseui::kPagerButtonCount; ++i) {
        const int x = pagerX + i
            * (warehouseui::kPagerButtonWidth + warehouseui::kPagerButtonGap);
        const WarehouseTabPair_ pair = WarehouseUiCreatePager_(i, x, footerY,
            kWhUiPagerBase_ + i);
        WarehouseUiAddPanelItem_(dlg, pair.label);
        WarehouseUiAddPanelItem_(dlg, pair.hit);
        // 数字文本 id=0：H3DlgText 不参与命中，点击落在下层透明命中项上。
        H3DlgText* number = H3DlgText::Create(x, footerY,
            warehouseui::kPagerButtonWidth, warehouseui::kPagerButtonHeight, "",
            NH3Dlg::Text::SMALL, eTextColor::REGULAR, 0,
            eTextAlignment::MIDDLE_CENTER, 0);
        g_wh_ui_live_.pagerText[i] = number;
        WarehouseUiAddPanelItem_(dlg, number);
    }
    for (int i = 0; i < 2; ++i) {
        const WarehouseTabPair_ pair = WarehouseUiCreateFooterArrow_(i,
            layout.warehouse.x + (i == 0 ? 8 : layout.warehouse.width - 26),
            footerY, i == 0 ? kWhUiPrevId_ : kWhUiNextId_);
        WarehouseUiAddPanelItem_(dlg, pair.label);
        WarehouseUiAddPanelItem_(dlg, pair.hit);
    }
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
    WarehouseUiSetNativeIconLayer_(dlg, false);
    WarehouseUiRestoreSaveUnder_();
    WarehouseUiRestoreButtons_(dlg);
    WarehouseUiResetTabs_();
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
    // 原生 callback 永远在自有逻辑之外、恰好一次；不再因转移写 -2 或 CloseDialog。
    return original ? original(self, msg) : 0;
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
    g_wh_ui_scroll_enabled_ = reinterpret_cast<const int*>(base + 0x157FB8);
    g_wh_ui_shadow_ = base + 0x6620; // 完整背包外框阴影（与 0x039A00 构造同一函数）
    g_wh_ui_skin_factory_ = reinterpret_cast<WarehouseSkinFactory_>(base + 0x0FADB0);
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
