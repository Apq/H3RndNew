// H3MegaDesc式视觉层：原生DEF保留命中/按压，Draw槽改为绘制自有PCX。
// 素材进程内缓存；dialog/item裸指针只在attach..detach所属窗口生命周期内使用。
#include "WarehouseUiCatalog.hpp"
#include "WarehouseUiPcx.hpp"
#include <new>
#include <vector>

namespace {
static constexpr int kWhUiBtnWidth_ = 66, kWhUiBtnHeight_ = 32;
static constexpr int kWhUiTabWidth_ = 44, kWhUiTabHeight_ = 22;
static constexpr int kWhUiTabSlotCount_ = 4;
static constexpr int kWhUiArrowWidth_ = 18, kWhUiArrowHeight_ = 22;
static constexpr int kWhUiCloseWidth_ = 24, kWhUiCloseHeight_ = 24;
static constexpr int WarehouseUiButtonWidth_(int kind) noexcept {
    return kind == 1 ? kWhUiCloseWidth_ : kWhUiBtnWidth_;
}
static constexpr int WarehouseUiButtonHeight_(int kind) noexcept {
    return kind == 1 ? kWhUiCloseHeight_ : kWhUiBtnHeight_;
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
};
struct WarehouseTabPair_ {
    H3DlgPcx* label = nullptr;
    H3DlgTransparentItem* hit = nullptr;
};
struct WarehouseButtonPair_ {
    H3DlgPcx* visual = nullptr;
    H3DlgDefButton* def = nullptr;
};
static WarehouseButton_ g_wh_ui_buttons_[4];
static WarehouseTabPair_ g_wh_ui_tabs_[4], g_wh_ui_arrows_[2];
static H3LoadedPcx* g_wh_ui_button_images_[4][4] = {};
static H3LoadedPcx* g_wh_ui_tab_images_[warehouseui::kTabCount][2] = {};
static H3LoadedPcx* g_wh_ui_arrow_images_[2][2] = {};
// 只保存颜色字节，不静态构造会调用游戏地址的palette对象。
static unsigned char g_wh_ui_palette_[sizeof(H3RGB888) * 256] = {};
static bool g_wh_ui_images_attempted_ = false, g_wh_ui_images_ready_ = false;
static std::size_t g_wh_ui_tab_offset_ = 0;
static int g_wh_ui_tab_active_ = 0;
struct WarehouseVisualResource_ {
    H3DlgPcx* item = nullptr;
    H3LoadedPcx* original = nullptr;
};
static WarehouseVisualResource_ g_wh_ui_visual_resources_[20];
static std::size_t g_wh_ui_visual_count_ = 0;

// SetPcx不调整引用：保存创建时原指针，detach恢复后让原生析构释放原引用。
static H3DlgPcx* WarehouseUiCreateVisual_(int x, int y, int w, int h, int id,
                                         H3LoadedPcx* image) noexcept {
    if (g_wh_ui_visual_count_ >= 20) return nullptr;
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
        g_wh_ui_tab_images_[t][s] = WarehouseUiLoadOne_(file, 44, 22, key);
        all = all && g_wh_ui_tab_images_[t][s] != nullptr;
    }
    for (int a = 0; a < 2; ++a) for (int s = 0; s < 2; ++s) {
        _snprintf_s(file, sizeof(file), _TRUNCATE, "arrow_%s%d.pcx", a ? "next" : "prev", s);
        _snprintf_s(key, sizeof(key), _TRUNCATE, "wha%d%d", a, s);
        g_wh_ui_arrow_images_[a][s] = WarehouseUiLoadOne_(file, 18, 22, key);
        all = all && g_wh_ui_arrow_images_[a][s] != nullptr;
    }
    g_wh_ui_images_ready_ = all;
    LogInfo("仓库UI: 44张四态/Tab素材 ready=%d", g_wh_ui_images_ready_ ? 1 : 0);
}
static bool WarehouseUiImagesReady_() noexcept { return g_wh_ui_images_ready_; }

static void WarehouseUiRefreshButton_(int index) noexcept {
    WarehouseButton_& b = g_wh_ui_buttons_[index];
    if (!WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.def)
        || !WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.visual)) return;
    const UINT16 flags = *reinterpret_cast<UINT16*>(reinterpret_cast<BYTE*>(b.def) + 0x16);
    const int state = (flags & 0x20) ? 3 : (flags & 1) ? 1 : b.hovered ? 2 : 0;
    b.visual->SetPcx(g_wh_ui_button_images_[index][state]);
}
// 专门的视觉Draw替换：不画原来的iOkay图标，不改变其余14个vtable槽。
static void __fastcall WarehouseUiButtonDraw_(void* self, void*) {
    __try {
        for (int i = 0; i < 4; ++i) {
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
    if (index < 0 || index >= 4 || !WarehouseUiImagesReady_()) return pair;
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
    b.originalVtable = *reinterpret_cast<void***>(b.def);
    for (int i = 0; i < 15; ++i) b.novtbl[i] = b.originalVtable[i];
    b.novtbl[4] = reinterpret_cast<void*>(&WarehouseUiButtonDraw_);
    *reinterpret_cast<void***>(b.def) = b.novtbl;
    return pair;
}
static void WarehouseUiSetButtonDisabled_(int index, bool disabled) noexcept {
    WarehouseButton_& b = g_wh_ui_buttons_[index];
    if (!WarehouseUiVisualOwns_(g_wh_ui_visual_dialog_, b.def)) return;
    UINT16& flags = *reinterpret_cast<UINT16*>(reinterpret_cast<BYTE*>(b.def) + 0x16);
    flags = disabled ? static_cast<UINT16>(flags | 0x20) : static_cast<UINT16>(flags & ~0x20);
    WarehouseUiRefreshButton_(index);
}
static void WarehouseUiUpdateHover_(H3BaseDlg* dlg, H3Msg* msg) noexcept {
    if (!dlg || !msg || dlg != g_wh_ui_visual_dialog_) return;
    for (int i = 0; i < 4; ++i) {
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
        WarehouseUiRefreshButton_(i);
        b.visual->Refresh();
    }
}
static void WarehouseUiRestoreButtons_(H3BaseDlg* dlg) noexcept {
    for (int i = 0; i < 4; ++i) {
        WarehouseButton_& b = g_wh_ui_buttons_[i];
        if (WarehouseUiVisualOwns_(dlg, b.def) && b.originalVtable
            && *reinterpret_cast<void***>(b.def) == b.novtbl)
            *reinterpret_cast<void***>(b.def) = b.originalVtable;
        b.def = nullptr; b.visual = nullptr; b.originalVtable = nullptr; b.hovered = false;
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
    // native -2重建保留当前分类/偏移，不保留任何对象引用。
}
static WarehouseTabPair_ WarehouseUiCreateTab_(int slot, int x, int y, int id) noexcept {
    WarehouseTabPair_ pair;
    if (!WarehouseUiImagesReady_() || slot < 0 || slot >= 4) return pair;
    pair.label = WarehouseUiCreateVisual_(x, y, 44, 22, id,
        g_wh_ui_tab_images_[0][0]);
    pair.hit = H3DlgTransparentItem::Create(x, y, 44, 22, id);
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
static void WarehouseUiRefreshTabs_(H3BaseDlg* dlg, bool redraw) noexcept {
    const auto window = warehouseui::MakeTabWindow(g_wh_ui_tab_offset_, warehouseui::kTabCount, 4);
    g_wh_ui_tab_offset_ = window.begin;
    for (int i = 0; i < 4; ++i) {
        const std::size_t at = window.begin + i;
        if (WarehouseUiVisualOwns_(dlg, g_wh_ui_tabs_[i].label) && at < warehouseui::kTabCount)
            g_wh_ui_tabs_[i].label->SetPcx(g_wh_ui_tab_images_[at][at == g_wh_ui_tab_active_ ? 1 : 0]);
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
        g_wh_ui_tab_offset_ = warehouseui::TabOffsetShift(g_wh_ui_tab_offset_, id == 30310 ? -1 : 1,
            warehouseui::kTabCount, 4);
        return 0;
    }
    if (id >= 30300 && id < 30304) {
        const std::size_t at = g_wh_ui_tab_offset_ + id - 30300;
        if (at >= warehouseui::kTabCount || at == g_wh_ui_tab_active_) return 0;
        g_wh_ui_tab_active_ = static_cast<int>(at);
        return 1;
    }
    return -1;
}
static int WarehouseUiActiveTab_() noexcept { return g_wh_ui_tab_active_; }
static warehouseui::TabKind WarehouseUiActiveTabKind_() noexcept {
    return warehouseui::kTabOrder[g_wh_ui_tab_active_];
}
static int WarehouseUiArtifactSlot_(int artifactId) noexcept {
    if (artifactId < 0 || artifactId > 143) return warehouseui::kSlotNone;
    __try {
        const H3Artifact artifact(artifactId, -1);
        return static_cast<int>(artifact.GetSlot());
    } __except (GuardCrashFilter_(GUARD_WH_UI_BUTTON_DRAW, GetExceptionInformation())) {
        return warehouseui::kSlotNone;
    }
}
} // namespace
