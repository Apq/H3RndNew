// H3MegaDesc式视觉层：原生DEF保留命中/按压，Draw槽改为绘制自有PCX。
// 素材进程内缓存；dialog/item裸指针只在attach..detach所属窗口生命周期内使用。
#include "WarehouseUiCatalog.hpp"
#include "WarehouseUiCallback.hpp"
#include "WarehouseUiFlow.hpp"
#include "WarehouseUiPcx.hpp"
#include <new>
#include <vector>

namespace {
static constexpr int kWhUiBtnWidth_ = 66, kWhUiBtnHeight_ = 32;
// round-9：页签 44→36（标题均为两字），高不变。
static constexpr int kWhUiTabWidth_ = 36, kWhUiTabHeight_ = 22;
// round-9.1（用户裁定）：两端箭头距边 18px，中间“能放多少放多少”页签。
// 内区 = 405 - 18*2(边距) - 18*2(箭头) = 333px；36 宽页签 + 4 间距 = 40 步距
// → FittingTabCount(333,36,4) = 8 个（316px，余 17px 居中），不再留大片空白。
static constexpr int kWhUiTabSlotCount_ = 8;
static constexpr int kWhUiTabRowInset_ = 18;
static constexpr int kWhUiArrowWidth_ = 18, kWhUiArrowHeight_ = 22;
static constexpr int kWhUiCloseWidth_ = 24, kWhUiCloseHeight_ = 24;
static constexpr int WarehouseUiButtonWidth_(int kind) noexcept {
    return kind == 1 ? kWhUiCloseWidth_
        : kind == 4 ? warehouseui::kOrganizeButtonWidth : kWhUiBtnWidth_;
}
static constexpr int WarehouseUiButtonHeight_(int kind) noexcept {
    return kind == 1 ? kWhUiCloseHeight_
        : kind == 4 ? warehouseui::kPagerButtonHeight : kWhUiBtnHeight_;
}
// 共用一个绘制guard，使全插件注册总数不超过CrashGuard的16项容量。
static const int GUARD_WH_UI_BUTTON_DRAW = GuardRegisterHook_("WarehouseUiVisual");
static H3BaseDlg* g_wh_ui_visual_dialog_ = nullptr;
struct WarehouseButton_ {
    H3DlgDefButton* def = nullptr;
    H3DlgPcx* visual = nullptr;
    void** originalVtable = nullptr;
    void* novtbl[15] = {};
    bool hovered = false;
    // round-19：自有按下态。原实现只看原生按下位（item +0x16 & 1），实测只有整理
    // 按钮能看到“按下→弹起”，一键/X/仓库看不到，故改由本模块在 DOWN 置位、
    // CLICK/光标离开/隐藏时清位，并显式重绘（见 WarehouseUiSetButtonPressed_）。
    bool pressed = false;
};
struct WarehouseTabPair_ {
    H3DlgPcx* label = nullptr;
    H3DlgTransparentItem* hit = nullptr;
};
struct WarehouseButtonPair_ {
    H3DlgPcx* visual = nullptr;
    H3DlgDefButton* def = nullptr;
};
// round-13（用户裁定）：整理按钮并入主按钮模型，成为第 5 个按钮——原生
// H3DlgDefButton 吃输入并维护按压/禁用状态位（flags +0x16），PCX 视觉件按
// 四态换底图，与“一键取出/放入”完全同一管线，不再单独发明悬停/按压逻辑。
// 索引：0=打开 1=关闭 2=取出 3=放入 4=整理。
static constexpr int kWhUiButtonCount_ = 5;
static WarehouseButton_ g_wh_ui_buttons_[kWhUiButtonCount_];
static WarehouseTabPair_ g_wh_ui_tabs_[kWhUiTabSlotCount_], g_wh_ui_arrows_[2];
// 分页栏（round-12）：只留数字页码按钮，按“能摆几个就摆几个”创建（上限 10 个），
// 两端 < > 箭头已按用户裁定删除。
static WarehouseTabPair_ g_wh_ui_pagers_[warehouseui::kPagerMaxButtons];
static std::size_t g_wh_ui_pager_window_begin_ = 0;
static std::size_t g_wh_ui_pager_visible_ = 0;
static H3LoadedPcx* g_wh_ui_button_images_[kWhUiButtonCount_][4] = {};
static H3LoadedPcx* g_wh_ui_tab_images_[warehouseui::kTabCount][2] = {};
static H3LoadedPcx* g_wh_ui_arrow_images_[2][2] = {};
static H3LoadedPcx* g_wh_ui_pager_images_[2] = {};
// 只保存颜色字节，不静态构造会调用游戏地址的palette对象。
static unsigned char g_wh_ui_palette_[sizeof(H3RGB888) * 256] = {};
static bool g_wh_ui_images_attempted_ = false, g_wh_ui_images_ready_ = false;
static std::size_t g_wh_ui_tab_offset_ = 0;
static int g_wh_ui_tab_active_ = 0;
// 页签显示顺序（round-11）：显示槽 i 对应 g_wh_ui_tab_order_[i]；attach 时经
// [WarehouseUI] TabOrder 解析覆盖，非法回退 kDefaultTabOrder。
static warehouseui::TabKind g_wh_ui_tab_order_[warehouseui::kTabCount];
struct WarehouseVisualResource_ {
    H3DlgPcx* item = nullptr;
    H3LoadedPcx* original = nullptr;
};
// 视觉控件登记表：这是“替换 PCX 原指针的恢复账本”（detach 要把每张 PCX 换回创建时的
// BkPack 指针），不是素材缓存、也不表示仓库容量/页数上限，所以容量必须覆盖实际创建数。
// round-13 实际：5 按钮视觉件(含整理) + 8 页签 + 2 页签箭头 + 10 页码 = 25，取 32 留
// 余量。（round-8..11 只有 20：整理是第 21 次 CreateVisual，被上限拒绝后 label=null，
//  表现为“常规页看不到整理按钮”，但透明命中区仍会被创建并加入面板项。）
static constexpr std::size_t kWhUiVisualResourceCapacity_ = 32;
static WarehouseVisualResource_ g_wh_ui_visual_resources_[kWhUiVisualResourceCapacity_];
static std::size_t g_wh_ui_visual_count_ = 0;

// SetPcx不调整引用：保存创建时原指针，detach恢复后让原生析构释放原引用。
static H3DlgPcx* WarehouseUiCreateVisual_(int x, int y, int w, int h, int id,
                                         H3LoadedPcx* image) noexcept {
    if (!image || g_wh_ui_visual_count_ >= kWhUiVisualResourceCapacity_) return nullptr;
    H3DlgPcx* item = H3DlgPcx::Create(x, y, w, h, id, "BkPack.pcx");
    if (!item) return nullptr;
    g_wh_ui_visual_resources_[g_wh_ui_visual_count_++] = { item, item->GetPcx() };
    item->SetPcx(image);
    // H3MegaDesc ReplacePcx8ItemImage：不让PCX画填充矩形遮住透明标题。
    UINT16& type = *reinterpret_cast<UINT16*>(reinterpret_cast<BYTE*>(item) + 0x14);
    type = static_cast<UINT16>(type & ~0x10);
    return item;
}

static bool WarehouseUiVisualOwns_(H3BaseDlg* dlg, const H3DlgItem* item) noexcept {
    if (!dlg || !item) return false;
    for (H3DlgItem* owned : dlg->GetList()) if (owned == item) return true;
    return false;
}

static H3LoadedPcx* WarehouseUiQuantize_(const std::vector<unsigned char>& rgb,
                                        int width, int height, const char* name) {
    H3LoadedPcx* pcx = H3LoadedPcx::Create(name, width, height);
    if (!pcx || !pcx->buffer || pcx->scanlineSize < width) return nullptr;
    std::memcpy(pcx->palette888.color, g_wh_ui_palette_, sizeof(g_wh_ui_palette_));
    pcx->palette565.InitiateFromPalette888(pcx->palette888);
    for (int y = 0; y < height; ++y) {
        unsigned char* dst = pcx->buffer + static_cast<std::size_t>(y) * pcx->scanlineSize;
        for (int x = 0; x < width; ++x) {
            const unsigned char* p = rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            if (p[0] == 255 && p[1] == 0 && p[2] == 255) { dst[x] = 0; continue; }
            int best = 1, distance = 0x7fffffff;
            for (int i = 1; i < 256; ++i) {
                const H3RGB888& c = pcx->palette888.color[i];
                const int r = static_cast<int>(p[0]) - c.r;
                const int g = static_cast<int>(p[1]) - c.g;
                const int b = static_cast<int>(p[2]) - c.b;
                const int d = r*r + g*g + b*b;
                if (d < distance) { best = i; distance = d; if (!d) break; }
            }
            dst[x] = static_cast<unsigned char>(best);
        }
    }
    pcx->IncreaseReferences(); // 进程缓存持有引用，不随modal析构释放。
    return pcx;
}

static H3LoadedPcx* WarehouseUiLoadOne_(const char* filename, int width, int height,
                                        const char* resourceName) noexcept {
    wchar_t* path = new (std::nothrow) wchar_t[kPathCap_ / sizeof(wchar_t)];
    if (!path) return nullptr;
    const DWORD cap = static_cast<DWORD>(kPathCap_ / sizeof(wchar_t));
    const DWORD copied = GetModuleFileNameW(g_hModule, path, cap);
    if (!copied || copied >= cap) { delete[] path; return nullptr; }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) { delete[] path; return nullptr; }
    slash[1] = 0;
    const std::size_t used = wcslen(path);
    if (swprintf_s(path + used, cap - used, L"whui\\%hs", filename) <= 0) {
        delete[] path; return nullptr;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    delete[] path;
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size = {};
    H3LoadedPcx* result = nullptr;
    try {
        if (GetFileSizeEx(file, &size) && size.QuadPart >= 128 && size.QuadPart <= 2097152) {
            std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
            std::vector<unsigned char> rgb;
            DWORD read = 0;
            if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
                && read == bytes.size()
                && warehouseui::DecodePcxRgb(bytes.data(), bytes.size(), width, height, rgb))
                result = WarehouseUiQuantize_(rgb, width, height, resourceName);
        }
    } catch (...) {}
    CloseHandle(file);
    if (!result) LogError("仓库UI: PCX加载失败 %s", filename);
    return result;
}

static void WarehouseUiCacheImages_(H3DlgPcx* source) noexcept {
    if (g_wh_ui_images_attempted_ || !source || !source->GetPcx()) return;
    g_wh_ui_images_attempted_ = true;
    std::memcpy(g_wh_ui_palette_, source->GetPcx()->palette888.color, sizeof(g_wh_ui_palette_));
    const char* kinds[] = { "open", "close", "take", "put" };
    char file[32], key[16];
    bool all = true;
    for (int b = 0; b < 4; ++b) for (int s = 0; s < 4; ++s) {
        _snprintf_s(file, sizeof(file), _TRUNCATE, "btn_%s%d.pcx", kinds[b], s);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "whb%d%d", b, s);
        const int width = WarehouseUiButtonWidth_(b);
        const int height = WarehouseUiButtonHeight_(b);
        g_wh_ui_button_images_[b][s] = WarehouseUiLoadOne_(file, width, height, key);
        all = all && g_wh_ui_button_images_[b][s] != nullptr;
    }
    for (int t = 0; t < warehouseui::kTabCount; ++t) for (int s = 0; s < 2; ++s) {
        _snprintf_s(file, sizeof(file), _TRUNCATE, "tab%02d%c.pcx", t, s ? 's' : 'n');
        _snprintf_s(key, sizeof(key), _TRUNCATE, "wht%d%d", t, s);
        g_wh_ui_tab_images_[t][s] = WarehouseUiLoadOne_(file, kWhUiTabWidth_,
            kWhUiTabHeight_, key);
        all = all && g_wh_ui_tab_images_[t][s] != nullptr;
    }
    for (int a = 0; a < 2; ++a) for (int s = 0; s < 2; ++s) {
        _snprintf_s(file, sizeof(file), _TRUNCATE, "arrow_%s%d.pcx", a ? "next" : "prev", s);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "wha%d%d", a, s);
        g_wh_ui_arrow_images_[a][s] = WarehouseUiLoadOne_(file, 18, 22, key);
        all = all && g_wh_ui_arrow_images_[a][s] != nullptr;
    }
    // 分页页码按钮底图：28x22 两态（按“最多两位数”定宽，无文字，数字由 H3DlgText 叠加）。
    for (int s = 0; s < 2; ++s) {
        _snprintf_s(file, sizeof(file), _TRUNCATE, "pager_%c.pcx", s ? 's' : 'n');
        _snprintf_s(key, sizeof(key), _TRUNCATE, "whp%d", s);
        g_wh_ui_pager_images_[s] = WarehouseUiLoadOne_(file,
            warehouseui::kPagerNumberWidth, warehouseui::kPagerButtonHeight, key);
        all = all && g_wh_ui_pager_images_[s] != nullptr;
    }
    // 整理按钮四态底图（round-13）：tidy0..3.pcx 与主按钮同一管线（36x22）。
    for (int s = 0; s < 4; ++s) {
        _snprintf_s(file, sizeof(file), _TRUNCATE, "tidy%d.pcx", s);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "whz%d", s);
        g_wh_ui_button_images_[4][s] = WarehouseUiLoadOne_(file,
            WarehouseUiButtonWidth_(4), WarehouseUiButtonHeight_(4), key);
        all = all && g_wh_ui_button_images_[4][s] != nullptr;
    }
    g_wh_ui_images_ready_ = all;
    LogInfo("仓库UI: 50张四态/Tab/分页/整理素材 ready=%d", g_wh_ui_images_ready_ ? 1 : 0);
}
static bool WarehouseUiImagesReady_() noexcept { return g_wh_ui_images_ready_; }

// round-20（用户裁定）：五个按钮都不需要“焦点/选中”语义——SELECTED(0x10) 会让原生
// H3DlgDefButton::vDrawToWindow 改用 [item+0x40] 帧、FOCUSED(0x40) 是焦点位。原生
// vGetFocus/vLoseFocus 是空 ret，焦点位只可能被别处（对话框/HD 包装）置上，所以这里
// 每次刷新都清掉这两位，按钮永远不会“看起来有焦点”。
static void WarehouseUiClearButtonFocusBits_(WarehouseButton_& b) noexcept {
    if (!WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.def)) return;
    UINT16& flags = *reinterpret_cast<UINT16*>(reinterpret_cast<BYTE*>(b.def) + 0x16);
    flags = static_cast<UINT16>(flags
        & ~static_cast<UINT16>(warehouseui::kWhUiButtonFocusBits));
}

static void WarehouseUiRefreshButton_(int index) noexcept {
    WarehouseButton_& b = g_wh_ui_buttons_[index];
    if (!WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.def)
        || !WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.visual)) return;
    const UINT16 flags = *reinterpret_cast<UINT16*>(reinterpret_cast<BYTE*>(b.def) + 0x16);
    // round-20 第二次修正（用户裁定原话：“我不需要焦点效果,需要鼠标按下时的压住效果
    // 和松开时的弹起效果(不带焦点效果)”）：五按钮只有 普通/按下 两态。
    // * 不再用发亮的“悬停态”（四态素材里的 state2）：按住按钮时鼠标必然在按钮上，
    //   按下位一旦没置上，显示的就是那一格——用户说的“焦点效果还在”正是它；
    // * 按下位 = 自维护 pressed **或** 原生 PRESSED(+0x16 & 1) 任一为真。原生位是游戏
    //   自己在鼠标按下时置上的，不依赖我们对坐标/消息的判定，是压住感的兜底来源
    //   （第 19 轮实机可见的压住感就来自它，本不该删）。
    WarehouseUiClearButtonFocusBits_(b);
    const int state = warehouseui::ButtonVisualStateForPress(b.pressed,
        (flags & 1) != 0);
    b.visual->SetPcx(g_wh_ui_button_images_[index][state]);
}
// round-19（用户裁定）：五个按钮的“按下→弹起”必须一致可见。自己维护 pressed 后
// 显式刷屏，序列与仓库状态条相同（Draw 走 vtable[4]，Refresh 经父对话框刷该 item
// 矩形，对应原生 0x10FAA10 的效果）——不再依赖原生按下位是否触发重绘。
static void WarehouseUiSetButtonPressed_(int index, bool pressed) noexcept {
    if (index < 0 || index >= kWhUiButtonCount_) return;
    WarehouseButton_& b = g_wh_ui_buttons_[index];
    if (b.pressed == pressed) return;
    b.pressed = pressed;
    if (!WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.def)
        || !WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.visual)) return;
    WarehouseUiRefreshButton_(index);
    __try {
        b.visual->Draw();
        b.visual->Refresh();
    } __except (GuardCrashFilter_(GUARD_WH_UI_BUTTON_DRAW, GetExceptionInformation())) {}
}
// 按下置位：先按 itemId 命中；原生按下消息的 itemId 不保证是我们的按钮 ID（空槽/
// 视觉件/命中回退都可能给别的值），此时退化为用光标点在该按钮矩形内定位（与
// WarehouseUiUpdateHover_ 同一套判定）。两者都没命中 = 全部弹起。
static void WarehouseUiSetButtonPressedByIdOrPoint_(H3BaseDlg* dlg, H3Msg* msg) noexcept {
    int hit = msg ? warehouseui::ButtonIndexFromId(msg->itemId) : -1;
    if (hit < 0 && msg) {
        for (int i = 0; i < kWhUiButtonCount_; ++i) {
            WarehouseButton_& b = g_wh_ui_buttons_[i];
            if (!WarehouseUiVisualOwns_(dlg, b.def) || !b.def->IsVisible()
                || !b.def->IsEnabled()) continue;
            const int x = b.def->GetAbsoluteX(), y = b.def->GetAbsoluteY();
            if (msg->GetX() >= x && msg->GetX() < x + WarehouseUiButtonWidth_(i)
                && msg->GetY() >= y && msg->GetY() < y + WarehouseUiButtonHeight_(i)) {
                hit = i;
                break;
            }
        }
    }
    for (int i = 0; i < kWhUiButtonCount_; ++i)
        WarehouseUiSetButtonPressed_(i, i == hit);
}
static void WarehouseUiClearAllButtonsPressed_() noexcept {
    for (int i = 0; i < kWhUiButtonCount_; ++i) WarehouseUiSetButtonPressed_(i, false);
}
// round-20：展开/收起会整体显隐面板项，切换前的按下/悬停标记此后不一定再收到
// MOUSE_OVER 去纠正，残留下来就表现为“按钮看起来有焦点”。切换时统一清干净。
static void WarehouseUiResetButtonInput_() noexcept {
    for (int i = 0; i < kWhUiButtonCount_; ++i) {
        WarehouseButton_& b = g_wh_ui_buttons_[i];
        b.hovered = false;
        WarehouseUiSetButtonPressed_(i, false);
        WarehouseUiClearButtonFocusBits_(b);
    }
}
// 专门的视觉Draw替换：不画原来的iOkay图标，不改变其余14个vtable槽。
static void __fastcall WarehouseUiButtonDraw_(void* self, void*) {
    __try {
        for (int i = 0; i < kWhUiButtonCount_; ++i) {
            WarehouseButton_& b = g_wh_ui_buttons_[i];
            if (b.def == self && WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.def)
                && WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.visual)) {
                WarehouseUiRefreshButton_(i);
                b.visual->Draw();
                break;
            }
        }
    } __except (GuardCrashFilter_(GUARD_WH_UI_BUTTON_DRAW, GetExceptionInformation())) {}
}

static WarehouseButtonPair_ WarehouseUiCreateButton_(int index, int x, int y, int id) noexcept {
    WarehouseButtonPair_ pair;
    if (index < 0 || index >= kWhUiButtonCount_ || !WarehouseUiImagesReady_()) return pair;
    WarehouseButton_& b = g_wh_ui_buttons_[index];
    // 调用方在detach已恢复旧表并忘记对象；严禁复用跨modal的旧指针。
    const int width = WarehouseUiButtonWidth_(index);
    const int height = WarehouseUiButtonHeight_(index);
    pair.visual = WarehouseUiCreateVisual_(x, y, width, height, id,
        g_wh_ui_button_images_[index][0]);
    if (!pair.visual) return pair;
    pair.def = H3DlgDefButton::Create(x, y, width, height, id, "iOkay.def", 0, 1, FALSE, 0);
    if (!pair.def) return pair;
    b.visual = pair.visual;
    b.def = pair.def;
    b.hovered = false;
    b.pressed = false;
    b.originalVtable = *reinterpret_cast<void***>(b.def);
    for (int i = 0; i < 15; ++i) b.novtbl[i] = b.originalVtable[i];
    b.novtbl[4] = reinterpret_cast<void*>(&WarehouseUiButtonDraw_);
    *reinterpret_cast<void***>(b.def) = b.novtbl;
    return pair;
}
// round-20（用户裁定）：一键取出/放入不再判断“能不能取放”，按钮不再写任何禁用位，
// 因此原来的 WarehouseUiSetButtonDisabled_ 整个删除（五按钮状态只剩普通/按下/悬停）。
// 事务侧的安全判定（手持物品、仓库锁）仍由 StageB 的动作分支自己把守，与本视觉无关。
// 按钮对显隐（round-13）：视觉件与原生命中按钮同步显示/隐藏；隐藏用
// HideDeactivate（不显示也不参与 0x5FF9A0 命中）。整理按钮按页签种类切换，
// 其余按钮仍随展开态整体处理。
static void WarehouseUiSetButtonVisible_(int index, bool visible) noexcept {
    if (index < 0 || index >= kWhUiButtonCount_) return;
    WarehouseButton_& b = g_wh_ui_buttons_[index];
    const bool owned = WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.visual)
        && WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.def);
    if (owned) {
        if (visible) {
            b.visual->ShowActivate();
            b.def->ShowActivate();
            WarehouseUiRefreshButton_(index);
        } else {
            b.visual->HideDeactivate();
            b.def->HideDeactivate();
        }
    }
    b.hovered = false;
    b.pressed = false;
}
// 悬停跟踪（round-13 起整理也是按钮之一）：MOUSE_OVER 里改 b.hovered。round-20
// 第二次修正后悬停**不再决定底图**（五按钮只有 普通/按下 两态），它只用来判“光标
// 是否还压在这个按钮上”，按住后移出按钮即弹起；不再有单独的悬停外观逻辑。
static void WarehouseUiUpdateHover_(H3BaseDlg* dlg, H3Msg* msg) noexcept {
    if (!dlg || !msg || dlg != g_wh_ui_visual_dialog_) return;
    for (int i = 0; i < kWhUiButtonCount_; ++i) {
        WarehouseButton_& b = g_wh_ui_buttons_[i];
        if (!WarehouseUiVisualOwns_(dlg, b.def) || !WarehouseUiVisualOwns_(dlg, b.visual)) continue;
        const int x = b.def->GetAbsoluteX(), y = b.def->GetAbsoluteY();
        const int width = WarehouseUiButtonWidth_(i);
        const int height = WarehouseUiButtonHeight_(i);
        const bool hover = b.def->IsVisible() && b.def->IsEnabled()
            && msg->GetX() >= x && msg->GetX() < x + width
            && msg->GetY() >= y && msg->GetY() < y + height;
        if (hover == b.hovered) continue;
        b.hovered = hover;
        // 按住后把光标移出按钮 = 弹起（原生同语义）。只有按下标记真变了才刷屏：
        // 普通悬停变化不再触发任何重绘，也避免白刷一帧。
        if (!hover) WarehouseUiSetButtonPressed_(i, false);
    }
}
static void WarehouseUiRestoreButtons_(H3BaseDlg* dlg) noexcept {
    for (int i = 0; i < kWhUiButtonCount_; ++i) {
        WarehouseButton_& b = g_wh_ui_buttons_[i];
        if (WarehouseUiVisualOwns_(dlg, b.def) && b.originalVtable
            && *reinterpret_cast<void***>(b.def) == b.novtbl)
            *reinterpret_cast<void***>(b.def) = b.originalVtable;
        b.def = nullptr; b.visual = nullptr; b.originalVtable = nullptr;
        b.hovered = false; b.pressed = false;
        // novtbl不清零：已失去归属的对象不能写，也不能破坏其可能仍使用的表。
    }
    for (std::size_t i = 0; i < g_wh_ui_visual_count_; ++i) {
        auto& resource = g_wh_ui_visual_resources_[i];
        if (WarehouseUiVisualOwns_(dlg, resource.item))
            resource.item->SetPcx(resource.original);
        resource = {};
    }
    g_wh_ui_visual_count_ = 0;
    g_wh_ui_visual_dialog_ = nullptr;
}
static void WarehouseUiResetTabs_() noexcept {
    for (auto& tab : g_wh_ui_tabs_) tab = {};
    for (auto& arrow : g_wh_ui_arrows_) arrow = {};
    for (auto& pager : g_wh_ui_pagers_) pager = {};
    g_wh_ui_pager_window_begin_ = 0;
    g_wh_ui_pager_visible_ = 0;
    for (std::size_t i = 0; i < warehouseui::kTabCount; ++i)
        g_wh_ui_tab_order_[i] = warehouseui::kDefaultTabOrder[i];
    // native -2重建保留当前分类/偏移，不保留任何对象引用；顺序由下次 attach 重读。
}
static WarehouseTabPair_ WarehouseUiCreateTab_(int slot, int x, int y, int id) noexcept {
    WarehouseTabPair_ pair;
    if (!WarehouseUiImagesReady_() || slot < 0 || slot >= kWhUiTabSlotCount_)
        return pair;
    // round-9（用户裁定）：页签标题都是两个字，44 收窄到 36（与页码/整理按钮同宽）。
    pair.label = WarehouseUiCreateVisual_(x, y, kWhUiTabWidth_, kWhUiTabHeight_, id,
        g_wh_ui_tab_images_[0][0]);
    pair.hit = H3DlgTransparentItem::Create(x, y, kWhUiTabWidth_, kWhUiTabHeight_, id);
    g_wh_ui_tabs_[slot] = pair;
    return pair;
}
static WarehouseTabPair_ WarehouseUiCreateArrow_(int arrow, int x, int y, int id) noexcept {
    WarehouseTabPair_ pair;
    if (!WarehouseUiImagesReady_() || arrow < 0 || arrow >= 2) return pair;
    pair.label = WarehouseUiCreateVisual_(x, y, 18, 22, id,
        g_wh_ui_arrow_images_[arrow][0]);
    pair.hit = H3DlgTransparentItem::Create(x, y, 18, 22, id);
    g_wh_ui_arrows_[arrow] = pair;
    return pair;
}
// 分页页码按钮：36x22 底图 + 透明命中；数字文本由 StageB 叠加。
static WarehouseTabPair_ WarehouseUiCreatePager_(int slot, int x, int y,
                                                 int id) noexcept {
    WarehouseTabPair_ pair;
    if (!WarehouseUiImagesReady_() || slot < 0
        || slot >= warehouseui::kPagerMaxButtons)
        return pair;
    pair.label = WarehouseUiCreateVisual_(x, y, warehouseui::kPagerNumberWidth,
        warehouseui::kPagerButtonHeight, id, g_wh_ui_pager_images_[0]);
    pair.hit = H3DlgTransparentItem::Create(x, y, warehouseui::kPagerNumberWidth,
        warehouseui::kPagerButtonHeight, id);
    g_wh_ui_pagers_[slot] = pair;
    return pair;
}
static std::size_t WarehouseUiPagerBegin_() noexcept {
    return g_wh_ui_pager_window_begin_;
}
static std::size_t WarehouseUiPagerVisible_() noexcept {
    return g_wh_ui_pager_visible_;
}
// 整理按钮所有权（round-13 并入主按钮模型后）：视觉件与原生命中按钮都在
// 本 dialog 名下才算完整，供刷新翻转日志核对“按钮存在但没显示”类问题。
static bool WarehouseUiTidyOwned_(H3BaseDlg* dlg) noexcept {
    return WarehouseUiVisualOwns_(dlg, g_wh_ui_buttons_[4].visual)
        && WarehouseUiVisualOwns_(dlg, g_wh_ui_buttons_[4].def);
}
// 刷新分页栏（round-12）：只显示数字页码，数量按可用宽度取（仓库容量不设上限）；
// 页码窗口含当前页，不足则整页隐藏，避免点击对应空白区却误判页码。
static void WarehouseUiRefreshPager_(H3BaseDlg* dlg, std::size_t page,
                                     std::size_t pages, bool expanded) noexcept {
    const std::size_t visible = warehouseui::PagerVisibleCount(pages,
        warehouseui::FittingPagerButtons(warehouseui::kPanelWidth,
            warehouseui::kPagerNumberWidth, warehouseui::kPagerButtonGap));
    g_wh_ui_pager_visible_ = visible;
    g_wh_ui_pager_window_begin_ = warehouseui::PagerWindowBegin(page, visible, pages);
    for (int i = 0; i < warehouseui::kPagerMaxButtons; ++i) {
        const std::size_t at = g_wh_ui_pager_window_begin_
            + static_cast<std::size_t>(i);
        const bool shown = at < pages
            && static_cast<std::size_t>(i) < visible;
        WarehouseTabPair_& pager = g_wh_ui_pagers_[i];
        if (WarehouseUiVisualOwns_(dlg, pager.label)) {
            pager.label->SetPcx(
                g_wh_ui_pager_images_[shown && at == page ? 1 : 0]);
            if (shown && expanded) pager.label->ShowActivate();
            else pager.label->HideDeactivate();
        }
        if (pager.hit) {
            if (shown && expanded) pager.hit->ShowActivate();
            else pager.hit->HideDeactivate();
        }
    }
}
static void WarehouseUiRefreshTabs_(H3BaseDlg* dlg, bool redraw) noexcept {
    const auto window = warehouseui::MakeTabWindow(g_wh_ui_tab_offset_,
        warehouseui::kTabCount, static_cast<std::size_t>(kWhUiTabSlotCount_));
    g_wh_ui_tab_offset_ = window.begin;
    for (int i = 0; i < kWhUiTabSlotCount_; ++i) {
        const std::size_t at = window.begin + i;
        // round-11：显示槽 at 对应顺序数组里的 TabKind；素材固定按枚举规范序出图
        // （tab{枚举值}.pcx），与显示顺序解耦，配置换序不用换素材。
        if (WarehouseUiVisualOwns_(dlg, g_wh_ui_tabs_[i].label)
            && at < warehouseui::kTabCount) {
            const int asset = static_cast<int>(g_wh_ui_tab_order_[at]);
            g_wh_ui_tabs_[i].label->SetPcx(g_wh_ui_tab_images_[asset]
                [at == static_cast<std::size_t>(g_wh_ui_tab_active_) ? 1 : 0]);
        }
    }
    for (int i = 0; i < 2; ++i) {
        const bool enabled = i ? window.canNext : window.canPrev;
        if (WarehouseUiVisualOwns_(dlg, g_wh_ui_arrows_[i].label))
            g_wh_ui_arrows_[i].label->SetPcx(g_wh_ui_arrow_images_[i][enabled ? 0 : 1]);
    }
    // 不自行ShowActivate：展开/收起仅由StageB owned panelItems控制。
    if (redraw) dlg->Redraw();
}
static int WarehouseUiTabCommand_(int id) noexcept {
    if (id == 30310 || id == 30311) {
        g_wh_ui_tab_offset_ = warehouseui::TabOffsetShift(g_wh_ui_tab_offset_,
            id == 30310 ? -1 : 1, warehouseui::kTabCount,
            static_cast<std::size_t>(kWhUiTabSlotCount_));
        return 0;
    }
    if (id >= 30300 && id < 30300 + kWhUiTabSlotCount_) {
        const std::size_t at = g_wh_ui_tab_offset_ + id - 30300;
        if (at >= warehouseui::kTabCount || at == g_wh_ui_tab_active_) return 0;
        g_wh_ui_tab_active_ = static_cast<int>(at);
        return 1;
    }
    return -1;
}
static int WarehouseUiActiveTab_() noexcept { return g_wh_ui_tab_active_; }
static warehouseui::TabKind WarehouseUiActiveTabKind_() noexcept {
    return g_wh_ui_tab_order_[g_wh_ui_tab_active_];
}
// 页签显示顺序（round-11）：读 [WarehouseUI] TabOrder（逗号分隔中文页签名），
// 两层 ini 叠加，默认 kDefaultTabOrderText。要求 12 个互不相同的合法页签名，
// 任何缺失/重复/未知名都整体回退默认并留日志（原始读数 + 生效顺序都打出来）。
static void WarehouseUiLoadTabOrder_() noexcept {
    __try {
        char text[192] = {};
        IniReadUtf8Layered("WarehouseUI", "TabOrder",
            warehouseui::kDefaultTabOrderText, text, (int)sizeof(text));
        warehouseui::TabKind parsed[warehouseui::kTabCount];
        if (warehouseui::ParseTabOrder(text, parsed)) {
            for (std::size_t i = 0; i < warehouseui::kTabCount; ++i)
                g_wh_ui_tab_order_[i] = parsed[i];
        } else {
            for (std::size_t i = 0; i < warehouseui::kTabCount; ++i)
                g_wh_ui_tab_order_[i] = warehouseui::kDefaultTabOrder[i];
            LogInfo("仓库UI: 页签顺序配置无效，回退默认 raw=%s", text);
        }
        char effective[192] = {};
        int used = 0;
        for (std::size_t i = 0; i < warehouseui::kTabCount && used >= 0
            && (std::size_t)used + 1 < sizeof(effective); ++i)
            used += _snprintf_s(effective + used,
                (rsize_t)(sizeof(effective) - used), _TRUNCATE, "%s%s",
                i ? "," : "",
                warehouseui::TabKindName(g_wh_ui_tab_order_[i]));
        LogInfo("仓库UI: 页签顺序 raw=%s 生效=%s", text, effective);
    } __except (GuardCrashFilter_(GUARD_WH_UI_BUTTON_DRAW,
        GetExceptionInformation())) {
        for (std::size_t i = 0; i < warehouseui::kTabCount; ++i)
            g_wh_ui_tab_order_[i] = warehouseui::kDefaultTabOrder[i];
    }
}
static int WarehouseUiArtifactSlot_(int artifactId) noexcept {
    if (artifactId < 0 || artifactId > 143) return warehouseui::kSlotNone;
    __try {
        const H3Artifact artifact(artifactId, -1);
        return warehouseui::NormalizeArtifactTableSlot(
            static_cast<int>(artifact.GetSlot()));
    } __except (GuardCrashFilter_(GUARD_WH_UI_BUTTON_DRAW, GetExceptionInformation())) {
        return warehouseui::kSlotNone;
    }
}
} // namespace
