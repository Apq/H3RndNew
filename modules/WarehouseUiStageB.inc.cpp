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
// round-12：原分页栏两端箭头 ID 30050/30051 已随用户裁定删除（只留数字页码）。
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
    H3DlgItem* openButton = nullptr;
    H3DlgItem* closeButton = nullptr;
    H3DlgItem* takeAllButton = nullptr;
    H3DlgItem* putAllButton = nullptr;
    H3DlgItem* title = nullptr;
    H3DlgText* titleFallback = nullptr;
    H3DlgText* pagerText[warehouseui::kPagerMaxButtons] = {};
    H3DlgDef* slots[kWhUiSlotCount_] = {};
    // 底部状态条（round-14）：原生 H3DlgTextPcx（HD_TStat.pcx 底 + smalfont.fnt），
    // 与背包状态条同一构造、同一悬停文案源。类型取 H3DlgTextPcx 以便用 H3API
    // 的 SetText/Draw/Refresh（与原生 vtable[13]/vtable[4]+刷屏序列逐槽对应）。
    H3DlgTextPcx* statusBar = nullptr;
    H3DlgItem* panelItems[kWhUiPanelItemCapacity_] = {};
    std::size_t panelItemCount = 0;
    std::vector<std::size_t> filtered;
    // 普通/卷轴控件各自拥有资源；原生旧图标保留但停用，不交换资源指针。
    H3DlgDef* nativeIcons[kWhUiSlotCount_][2] = {};
    H3DlgItem* originalNativeIcons[kWhUiSlotCount_] = {};
    bool nativeIconsReady = false;
    bool scrollDefEnabled = false;
    // 背包网格命中项几何（纯整数，仅展开态参与点坐标→槽位定位）。
    // 空槽没有原生命中项（native 只为有物品的格建 hit），此时靠 GridSlotFromPoint_
    // 的常量回退，保证“点空格”也能识别为 backpack 区域而不落进关闭分支。
    bool hitHas[kWhUiSlotCount_] = {};
    int hitX[kWhUiSlotCount_] = {};
    int hitY[kWhUiSlotCount_] = {};
    int hitWidth[kWhUiSlotCount_] = {};
    int hitHeight[kWhUiSlotCount_] = {};
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
// round-14：原生背包状态条创建**不再自己解析函数指针**（2026-10-14 崩溃复盘）。
//   H3DlgTextPcx 构造 0x5BCB70 / H3DlgText 构造 0x5BC6A0 / 游戏 operator new
//   0x617492 都是 **H3.exe 的绝对地址**——H3API 本身就 THISCALL_12(..., 0x5BCB70, ...)
//   直接调用；round-14 初版误把它们当 HD_SOD 的 RVA 再加基址，跳进 HD_SOD 别的
//   代码并最终在 .data 段执行而崩溃（addr=HD_SOD+0x10FB609）。现改用 H3API 官方
//   路径：H3ObjectAllocator<H3DlgTextPcx>().allocate(1)（游戏分配器，析构与
//   dialog owned-list 的 delete 匹配，size 与原生 new(0x54) 完全一致）
//   + THISCALL_12 按原生 11 个栈原样传（末位背包用 0，H3API 的 Create 固定写 8，
//   这里与背包逐参同构）。
//   屏幕刷新同样走 H3API：H3DlgText::SetText → vSetText(vtable[13]，原生同槽)、
//   H3DlgItem::Draw() → vDrawToWindow(vtable[4])、H3DlgItem::Refresh() 经父对话框
//   刷屏（对应原生 0x10FAA10 的效果），不再解析 HD_SOD 的刷新助手。
static constexpr unsigned kWhUiStatusCtorVa_ = 0x5BCB70;  // H3.exe 绝对 VA，勿加基址
static constexpr int kWhUiStatusColor_ = 1;               // 原生构造第 8 参（背包同色）

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
// 分页栏页码按钮（round-7/12）：30400..30407（上限 8 个，实际可见数按宽度取）。
static constexpr int kWhUiPagerBase_ = 30400;
// 状态条（round-14）：ID 只在日志里用（原生文本项不参与 0x5FF9A0 命中测试，不会以
// 命令 itemId 出现）。对象大小由 H3ObjectAllocator<H3DlgTextPcx> 保证与原生 0x54 一致。
static constexpr int kWhUiStatusId_ = 30500;
// 整理按钮（round-8）：分页条左侧预留按钮位，仅常规页签显示，做存储层排序。
static constexpr int kWhUiOrganizeId_ = 30052;
// 整理按钮 visible 状态日志去抖（-1 = 未打过）。
static int g_wh_ui_tidy_logged_ = -1;
// 布局偏移配置（round-7 用户裁定入配置文件 [WarehouseUI]，两层ini可覆盖）。
struct WarehouseUiLayoutConfig_ {
    int openDX;    // 仓库按钮距背包界面左上角
    int openDY;
    int closeDX;   // 关闭X距仓库界面右上角（水平缩进）
    int closeDY;   // 关闭X距仓库界面顶部
    int bulkLeftDX;  // 一键放入距仓库左边
    int bulkRightDX; // 一键取出距仓库右边
    int bulkDY;      // 两个一键按钮距仓库顶部
    int pagerRowBottomGap; // 分页行（整理+页码）距仓库面板底边的空隙
};
static WarehouseUiLayoutConfig_ g_wh_ui_layout_cfg_ = {
    8, 18, 0, 0, 8, 8, 50, warehouseui::kPagerRowBottomGapDefault };
// round-8：越界不再静默回退默认值（2026-10-14 用户实测 BulkButtonDY 被旧下限
// 30 静默弃成默认 50），改为就近钳制并记录原始读数；用户给多少就尽量照做。
static int WarehouseUiClampOffset_(int value, int low, int high) noexcept {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}
static void WarehouseUiLoadLayoutConfig_() noexcept {
    __try {
        const WarehouseUiLayoutConfig_ current = g_wh_ui_layout_cfg_;
        WarehouseUiLayoutConfig_ cfg = current;
        WarehouseUiLayoutConfig_ raw = current;
        raw.openDX = IniReadIntUtf8Layered("WarehouseUI", "OpenButtonDX", current.openDX);
        raw.openDY = IniReadIntUtf8Layered("WarehouseUI", "OpenButtonDY", current.openDY);
        raw.closeDX = IniReadIntUtf8Layered("WarehouseUI", "CloseButtonDX", current.closeDX);
        raw.closeDY = IniReadIntUtf8Layered("WarehouseUI", "CloseButtonDY", current.closeDY);
        raw.bulkLeftDX = IniReadIntUtf8Layered("WarehouseUI", "BulkButtonLeftDX", current.bulkLeftDX);
        raw.bulkRightDX = IniReadIntUtf8Layered("WarehouseUI", "BulkButtonRightDX", current.bulkRightDX);
        raw.bulkDY = IniReadIntUtf8Layered("WarehouseUI", "BulkButtonDY", current.bulkDY);
        raw.pagerRowBottomGap = IniReadIntUtf8Layered("WarehouseUI",
            "PagerRowBottomGap", current.pagerRowBottomGap);
        cfg.openDX = WarehouseUiClampOffset_(raw.openDX, 0, 330);
        cfg.openDY = WarehouseUiClampOffset_(raw.openDY, 0, 420);
        cfg.closeDX = WarehouseUiClampOffset_(raw.closeDX, 0, 380);
        cfg.closeDY = WarehouseUiClampOffset_(raw.closeDY, 0, 440);
        cfg.bulkLeftDX = WarehouseUiClampOffset_(raw.bulkLeftDX, 0, 330);
        cfg.bulkRightDX = WarehouseUiClampOffset_(raw.bulkRightDX, 0, 330);
        cfg.bulkDY = WarehouseUiClampOffset_(raw.bulkDY, 0, 440);
        cfg.pagerRowBottomGap = WarehouseUiClampOffset_(raw.pagerRowBottomGap,
            warehouseui::kPagerRowBottomGapMin, warehouseui::kPagerRowBottomGapMax);
        g_wh_ui_layout_cfg_ = cfg;
        LogInfo("仓库UI: 布局配置 open=%d,%d close=%d,%d bulk=%d,%d,%d pager_gap=%d",
            cfg.openDX, cfg.openDY, cfg.closeDX, cfg.closeDY,
            cfg.bulkLeftDX, cfg.bulkRightDX, cfg.bulkDY, cfg.pagerRowBottomGap);
        LogInfo("仓库UI: 布局配置原始读数 open=%d,%d close=%d,%d bulk=%d,%d,%d pager_gap=%d",
            raw.openDX, raw.openDY, raw.closeDX, raw.closeDY,
            raw.bulkLeftDX, raw.bulkRightDX, raw.bulkDY, raw.pagerRowBottomGap);
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
// 刷新由“整理”按钮（round-8，定义在前）调用；同 TU 内 static 需前置声明（C3861）。
static void WarehouseUiRefresh_(H3BaseDlg* dlg, bool redraw) noexcept;
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
    // 展示排序（round-13 用户裁定）：分类页在筛选结果内保持“相同道具连续”
    // （round-8）；常规页展示**纯存储顺序**——放入逐个追加到末尾、取出后剩余
    // 自动前移，任何展示层重排都会让“放入的位置”看起来不对。存储顺序只有
    // “整理”按钮（存储层排序）能改变。
    if (warehouseui::TabSortsDisplay(tab)) {
        try {
            warehouseui::SortRecordIndicesByIdentity(live.filtered, records);
        } catch (...) {
            // 排序失败保留未排序视图，不清空已有内容。
        }
    }
}

// 整理（round-8 用户裁定）：对仓库记录做**存储层**排序，相同道具连续；
// 不只重排展示视图。排序是同一性重排，不新增/删除记录，因此不触碰原生背包；
// epoch 递增让任何在途 Flow 失效。
static void WarehouseUiOrganizeStorage_(H3BaseDlg* dlg) noexcept {
    if (!g_wh_ui_live_.hero) return;
    auto& records = g_warehouse_.heroes[
        static_cast<std::size_t>(g_wh_ui_live_.hero->id)];
    if (records.size() < 2) {
        LogInfo("仓库UI: 整理跳过（记录不足2条） hero=%d size=%zu",
            g_wh_ui_live_.hero->id, records.size());
        return;
    }
    try {
        warehouseui::SortRecordsByIdentity(records);
    } catch (...) {
        LogInfo("仓库UI: 整理失败（内存不足） hero=%d", g_wh_ui_live_.hero->id);
        return;
    }
    if (g_warehouse_.mutationEpoch != (std::numeric_limits<std::uint64_t>::max)())
        ++g_warehouse_.mutationEpoch;
    LogInfo("仓库UI: 整理完成（存储层排序） hero=%d size=%zu epoch=%llu",
        g_wh_ui_live_.hero->id, records.size(),
        static_cast<unsigned long long>(g_warehouse_.mutationEpoch));
    g_wh_ui_live_.page = 0;
    WarehouseUiRefresh_(dlg, true);
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
    // 分页栏（round-12）：只显示数字页码，数量按可用宽度取；窗口整块对齐。
    WarehouseUiRefreshPager_(dlg, g_wh_ui_live_.page, pages,
        g_wh_ui_live_.expanded);
    const std::size_t pagerBegin = WarehouseUiPagerBegin_();
    const std::size_t pagerVisible = WarehouseUiPagerVisible_();
    for (int i = 0; i < warehouseui::kPagerMaxButtons; ++i) {
        H3DlgText* number = g_wh_ui_live_.pagerText[i];
        if (!number) continue;
        const std::size_t at = pagerBegin + static_cast<std::size_t>(i);
        if (at < pages && static_cast<std::size_t>(i) < pagerVisible) {
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
    // 整理按钮（round-8/13）：仅“常规”页签且展开时可见。判定按“页签种类”而不是顺序
    // 下标——TabOrder 可配置，常规不一定在首位。round-13 起并入主按钮模型
    // （原生 H3DlgDefButton 四态 + PCX 视觉），显隐走按钮对的统一路径。
    // 日志只在状态翻转时打一条，便于实机核对“按钮存在但没显示”究竟卡在判定、
    // 所有权还是层级。
    {
        const bool tidyVisible = warehouseui::TidyVisible(
            g_wh_ui_live_.expanded, WarehouseUiActiveTabKind_());
        WarehouseUiSetButtonVisible_(4, tidyVisible);
        if ((tidyVisible ? 1 : 0) != g_wh_ui_tidy_logged_) {
            g_wh_ui_tidy_logged_ = tidyVisible ? 1 : 0;
            LogInfo("仓库UI: 整理按钮 visible=%d owns=%d expanded=%d tab=%s pager_begin=%u pager_visible=%u",
                g_wh_ui_tidy_logged_, WarehouseUiTidyOwned_(dlg) ? 1 : 0,
                g_wh_ui_live_.expanded ? 1 : 0,
                warehouseui::TabKindName(WarehouseUiActiveTabKind_()),
                static_cast<unsigned>(pagerBegin),
                static_cast<unsigned>(pagerVisible));
        }
    }
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
        if (nativeHits[i]) {
            g_wh_ui_live_.hitHas[i] = true;
            g_wh_ui_live_.hitX[i] = nativeHits[i]->GetX();
            g_wh_ui_live_.hitY[i] = nativeHits[i]->GetY();
            g_wh_ui_live_.hitWidth[i] = nativeHits[i]->GetWidth();
            g_wh_ui_live_.hitHeight[i] = nativeHits[i]->GetHeight();
        }
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
    g_wh_ui_live_.flow = {};
    WarehouseUiRefreshNativeIcons_(dlg);
    WarehouseUiRefresh_(dlg, true);
}

// round-8：背包侧单击即事务性放入；不再要求先选中再点击仓库落点。
// 空槽只消费左键、不产生事务，因此不会落入原生未知 ID 关闭分支。
static void WarehouseUiDepositSlot_(H3BaseDlg* dlg, int slot) noexcept
{
    if (!dlg || g_wh_ui_live_.dialog != dlg || !g_wh_ui_live_.hero
        || slot < 0 || slot >= static_cast<int>(warehouse::kBackpackCapacity))
        return;
    warehouse::NativeBackpack native;
    WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
    const int count = g_wh_ui_live_.hero->backpackCount;
    if (slot >= count || native[static_cast<std::size_t>(slot)].artifactId < 0)
        return;
    g_wh_ui_live_.flow = warehouseui::Flow(native, count,
        g_wh_ui_live_.hero->id, g_wh_ui_live_.hero->owner,
        g_warehouse_.mutationEpoch, g_wh_ui_live_.token);
    if (!g_wh_ui_live_.flow.IsValid()) return;
    g_wh_ui_live_.flow.Select(static_cast<std::size_t>(slot));
    int updatedCount = count;
    const warehouseui::Result result = g_wh_ui_live_.flow.Deposit(
        g_warehouse_, native, updatedCount, g_wh_ui_live_.hero->id,
        g_wh_ui_live_.hero->owner, g_wh_ui_live_.token,
        g_wh_locked_ != 0, WarehouseUiHeld_());
    if (result == warehouseui::Result::Ok) {
        WarehouseUiWriteNative_(g_wh_ui_live_.hero, native, updatedCount);
        LogInfo("仓库UI: 单击放入成功 hero=%d slot=%d count=%d",
            g_wh_ui_live_.hero->id, slot, updatedCount);
        WarehouseUiRefreshAfterMove_(dlg);
    } else {
        LogInfo("仓库UI: 单击放入拒绝 result=%d hero=%d slot=%d",
            static_cast<int>(result), g_wh_ui_live_.hero->id, slot);
    }
}

// 优先使用 attach 时读取的原生命中项几何；空槽没有命中项时回退原生8x8网格。
static int WarehouseUiBackpackSlotAtPoint_(H3Msg* msg) noexcept
{
    if (!msg) return -1;
    const int x = msg->GetX();
    const int y = msg->GetY();
    for (int i = 0; i < kWhUiSlotCount_; ++i) {
        if (!g_wh_ui_live_.hitHas[i]) continue;
        const int width = g_wh_ui_live_.hitWidth[i] > 0
            ? g_wh_ui_live_.hitWidth[i] : warehouseui::kGridPitch + 1;
        const int height = g_wh_ui_live_.hitHeight[i] > 0
            ? g_wh_ui_live_.hitHeight[i] : warehouseui::kGridPitch + 1;
        if (x >= g_wh_ui_live_.hitX[i] && x < g_wh_ui_live_.hitX[i] + width
            && y >= g_wh_ui_live_.hitY[i] && y < g_wh_ui_live_.hitY[i] + height)
            return i;
    }
    const int originX = g_wh_ui_live_.layout.backpack.x + warehouseui::kGridOriginDX;
    const int originY = g_wh_ui_live_.layout.backpack.y
        + warehouseui::kBackpackGridOriginDY;
    return warehouseui::GridSlotFromPoint(x, y, originX, originY,
        warehouseui::kGridPitch, static_cast<int>(warehouseui::kGridColumns),
        static_cast<int>(warehouseui::kGridRows), kWhUiSlotCount_);
}

// 仓库状态条悬停文案（round-14，与原生背包 mouse-over 0x010396BD 分支同构）：
//  1) 用坐标（不是 msg->itemId）定位仓库槽——原生也是 0x5FF970 现算命中项；
//  2) 槽内有记录才更新：宝物名取同一个原生宝物表（H3Artifact::GetName →
//     0x660B68 表 +id*0x20+0），法术卷轴(id==1)按原生 wsprintf "%s. {%s.}"
//     拼“宝物名. {法术名.}”，法术名取 [0x687FA8]+subtype*0x88+0x10（H3Spell.name）；
//  3) 更新序列与原生完全相同：SetText(vtable[13]) → Draw(vtable[4]) → 0x10FAA10
//     只刷该 item 的屏幕矩形；离开物品不清空（背包侧同样保留最后文案）。
static void WarehouseUiUpdateSlotStatus_(H3BaseDlg* dlg, H3Msg* msg) noexcept {
    H3DlgTextPcx* bar = g_wh_ui_live_.statusBar;
    if (!dlg || !msg || !bar || !g_wh_ui_live_.expanded) return;
    if (g_wh_ui_live_.dialog != dlg) return;
    const int local = warehouseui::GridSlotFromPoint(msg->GetX(), msg->GetY(),
        g_wh_ui_live_.layout.warehouse.x + warehouseui::kGridOriginDX,
        g_wh_ui_live_.layout.warehouse.y + warehouseui::kGridOriginDY,
        warehouseui::kGridPitch, static_cast<int>(warehouseui::kGridColumns),
        static_cast<int>(warehouseui::kGridRows), kWhUiSlotCount_);
    if (local < 0) return;
    const warehouseui::PageView view = warehouseui::MakePageView(
        g_wh_ui_live_.filtered.size(), g_wh_ui_live_.page);
    const std::size_t at = view.begin + static_cast<std::size_t>(local);
    if (at >= view.begin + view.count || at >= g_wh_ui_live_.filtered.size()) return;
    const std::size_t recordIndex = g_wh_ui_live_.filtered[at];
    auto& records = g_warehouse_.heroes[static_cast<std::size_t>(
        g_wh_ui_live_.hero->id)];
    if (recordIndex >= records.size() || records[recordIndex].artifactId < 0) return;
    const warehouse::ArtifactRecord& record = records[recordIndex];
    const char* artifactName = nullptr;
    const char* spellName = nullptr;
    __try {
        artifactName = H3Artifact(record.artifactId, record.subtype).GetName();
        if (record.artifactId == warehouseui::kScrollArtifactId
            && record.subtype >= 0
            && record.subtype <= warehouseui::kMaxScrollSubtype) {
            const H3Spell* spells = *reinterpret_cast<H3Spell* const*>(0x687FA8);
            if (spells) spellName = spells[record.subtype].name;
        }
    } __except (GuardCrashFilter_(GUARD_WH_UI_POST_COMMON, GetExceptionInformation())) {
        return;
    }
    char text[512];
    if (!warehouseui::FormatStatusHint(text, sizeof(text), artifactName, spellName))
        return;
    __try {
        if (!WarehouseUiOwns_(dlg, bar)) return;
        // 与原生序列逐槽对应，但走 H3API（不自己解析地址）：
        //   SetText → vSetText（原生 vtable[13]）
        //   Draw    → vDrawToWindow（原生 vtable[4]）
        //   Refresh → 经父对话框刷该 item 区域（对应原生 0x10FAA10 的效果）
        bar->SetText(text);
        bar->Draw();
        bar->Refresh();
    } __except (GuardCrashFilter_(GUARD_WH_UI_POST_COMMON, GetExceptionInformation())) {}
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
        // 仓库槽悬停 → 仓库状态条提示（round-14）；背包侧仍走原生 2000..2063
        // 分支更新原生状态条（ID 777），两条状态条各管各自面板。
        WarehouseUiUpdateSlotStatus_(dlg, msg);
        return false;
    }
    // 滚轮（round-9/9.1 用户裁定）：仓库展开且光标在页签行（含两端箭头，行边距
    // 18px）上时，滚动页签窗口。只在实际滚动时消费；行外/已到边界一律透传，不
    // 影响原生滚轮路径。H3 消息层把 WM_MOUSEWHEEL 增量放在 subtype（H3Auto
    // PanelInput 同源实证）：>0 上滚=窗口左移、<0 下滚。
    if (msg->command == eMsgCommand::MOUSE_WHEEL) {
        if (!g_wh_ui_live_.expanded) return false;
        const warehouseui::Rect row(g_wh_ui_live_.layout.warehouse.x
            + kWhUiTabRowInset_,
            g_wh_ui_live_.layout.warehouse.y + kWhUiTabY_,
            g_wh_ui_live_.layout.warehouse.width - kWhUiTabRowInset_ * 2,
            kWhUiTabHeight_);
        if (warehouseui::PointInRect(msg->GetX(), msg->GetY(), row)) {
            const int shift = warehouseui::TabWheelShift(static_cast<int>(msg->subtype));
            if (shift == 0) return false;
            const std::size_t before = g_wh_ui_tab_offset_;
            WarehouseUiTabCommand_(shift < 0 ? 30310 : 30311);
            if (g_wh_ui_tab_offset_ == before) return false;  // 已到边界：透传
            WarehouseUiRefresh_(dlg, true);
            LogInfo("仓库UI: 滚轮滚动页签窗口 delta=%d offset=%u",
                static_cast<int>(msg->subtype),
                static_cast<unsigned>(g_wh_ui_tab_offset_));
            return true;
        }
        // 分页行滚轮（round-12）：数字页码没有 < > 按钮，滚轮提供逐页翻页的加速通道。
        // 与页签行互斥（两行矩形不重叠），到边界或无页时透传，不影响原生滚轮路径。
        const warehouseui::Rect pagerRow(g_wh_ui_live_.layout.warehouse.x
            + warehouseui::kPagerLeftInset,
            g_wh_ui_live_.layout.warehouse.y + warehouseui::PagerRowY(
                g_wh_ui_layout_cfg_.pagerRowBottomGap),
            warehouseui::kPanelWidth - warehouseui::kPagerLeftInset * 2,
            warehouseui::kPagerRowHeight);
        if (warehouseui::PointInRect(msg->GetX(), msg->GetY(), pagerRow)) {
            const std::size_t pages = warehouseui::PageCount(
                g_wh_ui_live_.filtered.size());
            const std::size_t target = warehouseui::PagerWheelTarget(
                g_wh_ui_live_.page, pages, static_cast<int>(msg->subtype));
            if (pages == 0 || target == g_wh_ui_live_.page) return false;
            g_wh_ui_live_.page = target;
            WarehouseUiRefresh_(dlg, true);
            LogInfo("仓库UI: 滚轮翻页 page=%u pages=%u",
                static_cast<unsigned>(target + 1), static_cast<unsigned>(pages));
            return true;
        }
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
        // round-19（用户裁定）：DOWN 置位自有按下态，五个按钮（打开/关闭/取出/放入/
        // 整理）一致的“按下”视觉；按在按钮以外（背包槽、空白、页码）等于全部弹起。
        WarehouseUiSetButtonPressedByIdOrPoint_(dlg, msg);
        // round-8：展开态下背包侧左键按下全部由仓库接管（含 1000..1063 原生命中项）。
        // 透传会让原版按压/拖拽状态机与事务写回同时改同一份背包数组，且空格的
        // 按下最终会落到原生未知回退关闭分支；这里不再区分 ID 与控件归属。
        if (warehouseui::ConsumeExpandedLeftDown(g_wh_ui_live_.expanded, leftDown))
            return true;
        return warehouseui::ConsumeWarehouseDown(id);
    }
    // round-19：释放（CLICK）= 弹起。先把按钮复位成普通态再执行动作，五个按钮一致，
    // 与整理按钮原有的“按下又弹起”观感相同。
    WarehouseUiClearAllButtonsPressed_();
    // item状态机已在callback之前处理按下；只中和callback的未知ID关闭分支。
    // 按钮及页签动作只在释放CLICK执行一次。
    if (!g_wh_ui_live_.expanded && id != kWhUiOpenId_) return false;
    if (!warehouseui::ExecutesWarehouseAction(static_cast<int>(msg->subtype))) return true;
    // 背包侧单击即事务性放入（round-8 用户裁定）：有物品的槽直接移入，空槽只消费
    // 左键不动数据。ID 不可靠（空槽无原生命中项、覆盖层与原件同 ID 区间）时再用
    // 点坐标在 8x8 网格上定位，保证“点背包里的装备就进仓库”与 ID 无关。
    if (id >= 1000 && id < 1064) {
        WarehouseUiDepositSlot_(dlg, id - 1000);
        return true;
    }
    if (id >= 2000 && id < 2064) {
        WarehouseUiDepositSlot_(dlg, id - 2000);
        return true;
    }
    // round-8.1（实机回归）：几何兜底只兜“背包区域内但无已知控件”的点击（空格、
    // 底板）。打开按钮(30200)等仓库控件位于背包面板矩形内部，若先做几何判定会被
    // “区域内”吞掉——表现为“点仓库按钮没反应”。已知控件必须走各自 ID 分派。
    if (warehouseui::UsesGeometryFallback(id)) {
        const int pointSlot = WarehouseUiBackpackSlotAtPoint_(msg);
        const bool insideBackpack = pointSlot >= 0
            || warehouseui::PointInRect(msg->GetX(), msg->GetY(),
                g_wh_ui_live_.layout.backpack);
        if (insideBackpack) {
            WarehouseUiDepositSlot_(dlg, pointSlot);
            return true;
        }
    }
    if (id >= kWhUiSlotBase_ && id < kWhUiSlotBase_ + kWhUiSlotCount_) {
        const int local = id - kWhUiSlotBase_;
        const warehouseui::PageView view = warehouseui::MakePageView(
            g_wh_ui_live_.filtered.size(), g_wh_ui_live_.page);
        const std::size_t at = view.begin + static_cast<std::size_t>(local);
        warehouse::NativeBackpack native;
        WarehouseUiCopyNative_(g_wh_ui_live_.hero, native);
        int count = g_wh_ui_live_.hero->backpackCount;
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
    // 页码直跳（round-7/12）：30400..30409 对应当前页码窗口的第 1..10 个页号，点哪个
    // 就跳到那个绝对页号；两端 < > 箭头已按用户裁定删除，翻窗靠滑动窗口保证邻页可点。
    if (id >= kWhUiPagerBase_
        && id < kWhUiPagerBase_ + warehouseui::kPagerMaxButtons) {
        const std::size_t pages = warehouseui::PageCount(
            g_wh_ui_live_.filtered.size());
        const std::size_t slot = static_cast<std::size_t>(id - kWhUiPagerBase_);
        const std::size_t at = WarehouseUiPagerBegin_() + slot;
        if (slot < WarehouseUiPagerVisible_() && at < pages
            && at != g_wh_ui_live_.page) {
            g_wh_ui_live_.page = at;
            WarehouseUiRefresh_(dlg, true);
        }
        return true;
    }
    if (id == kWhUiOpenId_) {
        WarehouseUiSetExpanded_(dlg, true, true);
        return true;
    }
    // 整理按钮（round-8/12）：只在可见（常规页签 + 展开态）时响应；不可见状态下的
    // 同名点击一律忽略，避免快捷键式误触发。判定与显隐共用同一个纯函数。
    if (id == kWhUiOrganizeId_) {
        if (warehouseui::TidyVisible(g_wh_ui_live_.expanded,
                WarehouseUiActiveTabKind_()))
            WarehouseUiOrganizeStorage_(dlg);
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

// 状态条创建（round-14）：单独成函数——MSVC 不允许在“需要对象展开”的函数里使用
// __try（C2712），而 WarehouseUiAttach_ 里有 std::vector 等局部对象，所以把 SEH
// 隔离推进这个小函数：任何失败都只等于“没有状态条”，绝不拖累面板本身创建。
static H3DlgTextPcx* WarehouseUiCreateStatusBar_(int x, int y) noexcept
{
    H3DlgTextPcx* bar = nullptr;
    __try {
        bar = H3ObjectAllocator<H3DlgTextPcx>().allocate(1);
        if (bar) {
            THISCALL_12(H3DlgTextPcx*, kWhUiStatusCtorVa_, bar, x, y,
                warehouseui::kStatusWidth, warehouseui::kStatusHeight,
                "", "smalfont.fnt", "HD_TStat.pcx",
                kWhUiStatusColor_, kWhUiStatusId_, 5, 0);
        }
    } __except (GuardCrashFilter_(GUARD_WH_UI_POST_COMMON, GetExceptionInformation())) {
        bar = nullptr;
    }
    return bar;
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
    // 无需重启游戏）；页签显示顺序同轮读（round-11 TabOrder）。
    WarehouseUiLoadLayoutConfig_();
    WarehouseUiLoadTabOrder_();
    WarehouseUiPrepareNativeIcons_(dlg);
    if (!g_wh_ui_live_.nativeIconsReady) {
        LogError("仓库UI: 无法建立完整原生背包图标，保留原生模式");
        WarehouseUiSetNativeIconLayer_(dlg, false);
        WarehouseUiDetach_(dlg);
        return;
    }

    // 完整复用扩展窗口实际背景生成器，保留其 HD 32bit 画布/玩家色/底部状态凹槽。
    // 第三轮错误：把256x256源图的GetPcx当作405x549可写画布；已彻底删除该路径。
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
    // 页签行（round-9/9.1 用户裁定）：滚动箭头在这一行两端、距边 18px；两箭头
    // 之间“能显示多少页签就显示多少”（36 宽、间距 4，8 个占 316px，余量居中）；
    // 整行支持滚轮滚动窗口。
    const int tabInnerStart = kWhUiTabRowInset_ + 18;  // 边距 + 箭头宽
    const int tabInnerSpan = layout.warehouse.width - tabInnerStart * 2;
    static_assert(kWhUiTabRowInset_ == 18, "行边距与几何推导保持一致");
    const int tabRowX = layout.warehouse.x + tabInnerStart
        + warehouseui::CenteredTabsX(tabInnerSpan, kWhUiTabWidth_,
            kWhUiTabSlotCount_, warehouseui::kPagerButtonGap);
    for (int i = 0; i < kWhUiTabSlotCount_; ++i) {
        const WarehouseTabPair_ pair = WarehouseUiCreateTab_(i,
            tabRowX + i * (kWhUiTabWidth_ + warehouseui::kPagerButtonGap),
            layout.warehouse.y + kWhUiTabY_, 30300 + i);
        WarehouseUiAddPanelItem_(dlg, pair.label);
        WarehouseUiAddPanelItem_(dlg, pair.hit);
    }
    for (int i = 0; i < 2; ++i) {
        const WarehouseTabPair_ pair = WarehouseUiCreateArrow_(i,
            layout.warehouse.x + (i == 0 ? kWhUiTabRowInset_
                                         : layout.warehouse.width
                                             - kWhUiTabRowInset_ - 18),
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
    // 分页行（round-12 用户裁定）：去掉两端 < > 箭头，只留数字页码；页码按钮个数按
    // 可用宽度“能摆几个就几个”，点击直接跳到该页。最左固定给“整理”按钮留位。
    // 页码按钮按两位数定宽（kPagerNumberWidth），比整理按钮窄。
    const int footerY = layout.warehouse.y + warehouseui::PagerRowY(
        g_wh_ui_layout_cfg_.pagerRowBottomGap)
        + (warehouseui::kPagerRowHeight - warehouseui::kPagerButtonHeight) / 2;
    const int pagerX = layout.warehouse.x + warehouseui::CenteredPagerNumbersX(
        warehouseui::kPanelWidth, warehouseui::kPagerMaxButtons,
        warehouseui::kPagerNumberWidth, warehouseui::kPagerButtonGap);
    for (int i = 0; i < warehouseui::kPagerMaxButtons; ++i) {
        const int x = pagerX + i
            * (warehouseui::kPagerNumberWidth + warehouseui::kPagerButtonGap);
        const WarehouseTabPair_ pair = WarehouseUiCreatePager_(i, x, footerY,
            kWhUiPagerBase_ + i);
        WarehouseUiAddPanelItem_(dlg, pair.label);
        WarehouseUiAddPanelItem_(dlg, pair.hit);
        // 数字文本 id=0：H3DlgText 不参与命中，点击落在下层透明命中项上。
        H3DlgText* number = H3DlgText::Create(x, footerY,
            warehouseui::kPagerNumberWidth, warehouseui::kPagerButtonHeight, "",
            NH3Dlg::Text::SMALL, eTextColor::REGULAR, 0,
            eTextAlignment::MIDDLE_CENTER, 0);
        g_wh_ui_live_.pagerText[i] = number;
        WarehouseUiAddPanelItem_(dlg, number);
    }
    // 整理按钮（round-8/13 用户裁定）：分页条左侧预留的按钮位，仅“常规”页显示，
    // 点击对仓库记录做**存储层**排序（不是只重排展示）。round-13 起与主按钮完全
    // 同一管线（原生 H3DlgDefButton 吃输入并维护按压/悬停/禁用四态 + PCX 视觉）。
    {
        const WarehouseButtonPair_ pair = WarehouseUiCreateButton_(4,
            layout.warehouse.x + warehouseui::kPagerLeftInset, footerY,
            kWhUiOrganizeId_);
        WarehouseUiAddPanelItem_(dlg, pair.visual);
        WarehouseUiAddPanelItem_(dlg, pair.def);
    }
    // 底部状态条（round-14 用户裁定）：背包弹窗自带一条状态条（原生 ID 777，
    // (8,433) 389x18，HD_TStat.pcx+smalfont.fnt），仓库侧按“同构造、同几何规范、
    // 同一悬停文案源”补一条，悬停仓库道具时提示与背包完全一致（含文字颜色——
    // 颜色随 H3DlgTextPcx 构造参数走，不自己另调）。整段独立 __try 隔离：任何失败
    // 只等于“没有状态条”，绝不拖累面板本身的创建（2026-10-14 崩溃的教训）。
    {
        const int barX = layout.warehouse.x + warehouseui::kStatusInsetX;
        const int barY = layout.warehouse.y + warehouseui::kStatusTextY;
        H3DlgTextPcx* bar = WarehouseUiCreateStatusBar_(barX, barY);
        g_wh_ui_live_.statusBar = bar;
        if (bar) WarehouseUiAddPanelItem_(dlg, bar);
        LogInfo("仓库UI: 状态条 create=%d id=%d at=(%d,%d) %dx%d",
            bar ? 1 : 0, kWhUiStatusId_, barX, barY,
            warehouseui::kStatusWidth, warehouseui::kStatusHeight);
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
