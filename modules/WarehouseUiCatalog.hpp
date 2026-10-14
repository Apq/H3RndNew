// ========== WarehouseUiCatalog.hpp ==========
// 仓库UI 分类页签（纯逻辑，离线可回归；不依赖游戏内存）。
//
// 页签顺序（用户 2026-10-13 指定）：常规、卷轴、装备、以及按穿戴位置分类的页签。
// 说明：宝物穿戴位置取自 H3 原生宝物表（H3ArtifactSetup::position / H3Artifact::GetSlot），
// 运行时由 WarehouseUiStageB 的适配器读取，本头文件只做“位置→页签”的纯映射，
// 便于在无游戏环境下回归分类与滚动窗口。
//
// “常规”按用户“一键操作需要一个总入口”的用法定义为**全部道具**（不做筛选），
// 卷轴/装备/位置页签均为它的子集；判定函数 TabMatches 明确体现这一点。

#ifndef H3RNDNEW_WAREHOUSE_UI_CATALOG_HPP
#define H3RNDNEW_WAREHOUSE_UI_CATALOG_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace warehouseui {

enum class TabKind : std::uint8_t {
    Regular = 0,   // 常规：全部道具（不筛选）
    Scroll,        // 卷轴
    Equipment,     // 装备：所有可穿戴位置
    Head,          // 头部
    Shoulders,     // 肩部
    Neck,          // 项链
    Weapon,        // 右手（武器）
    Shield,        // 左手（盾牌）
    Torso,         // 胸甲
    Ring,          // 戒指（左右手）
    Feet,          // 靴子
    Misc,          // 杂物（MISC1..MISC5）
    Machine,       // 原生战争机器槽，不进入背包页签
    Spellbook      // 原生魔法书槽，不进入背包页签
};

// 机械与魔法书不会进入背包，也就不会进入仓库；自 2026-10-13 起不再列页签。
inline constexpr std::size_t kTabCount = 12;

// 页签显示顺序（round-11 用户裁定）：默认“常规、卷轴，杂物→头部倒序，装备”，
// 运行期可用 [WarehouseUI] TabOrder=逗号分隔页签名（UTF-8 中文）覆盖，须为 12 个
// 互不相同的合法页签名，否则回退本默认。素材 tab00..tab11 按 TabKind 枚举值出图
// （固定规范序，见 make_whui_assets.py 的 TABS），与显示顺序解耦。
inline constexpr TabKind kDefaultTabOrder[kTabCount] = {
    TabKind::Regular, TabKind::Scroll, TabKind::Misc, TabKind::Feet,
    TabKind::Ring, TabKind::Torso, TabKind::Shield, TabKind::Weapon,
    TabKind::Neck, TabKind::Shoulders, TabKind::Head, TabKind::Equipment};
inline constexpr const char* kDefaultTabOrderText =
    "常规,卷轴,杂物,靴子,戒指,胸甲,盾牌,武器,项链,肩部,头部,装备";

// 页签名（UTF-8）↔ TabKind：名字固定用于 ini 配置、日志与断言。
inline const char* TabKindName(TabKind kind) noexcept {
    switch (kind) {
    case TabKind::Regular: return "常规";
    case TabKind::Scroll: return "卷轴";
    case TabKind::Equipment: return "装备";
    case TabKind::Head: return "头部";
    case TabKind::Shoulders: return "肩部";
    case TabKind::Neck: return "项链";
    case TabKind::Weapon: return "武器";
    case TabKind::Shield: return "盾牌";
    case TabKind::Torso: return "胸甲";
    case TabKind::Ring: return "戒指";
    case TabKind::Feet: return "靴子";
    case TabKind::Misc: return "杂物";
    default: return "未知";
    }
}

// 解析逗号分隔的页签顺序文本。要求恰好 kTabCount 个互不相同且合法的页签名；
// 未知名、重复、缺失、多余项都判无效返回 false（调用方回退默认），分隔符两侧
// 容忍空白。
inline bool ParseTabOrder(const char* text, TabKind* out) noexcept {
    if (!text || !out) return false;
    bool used[kTabCount] = {};
    std::size_t count = 0;
    const char* p = text;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') ++p;
        const char* start = p;
        while (*p != '\0' && *p != ',') ++p;
        const char* end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
        int matched = -1;
        const std::size_t len = static_cast<std::size_t>(end - start);
        for (int k = 0; k < static_cast<int>(kTabCount); ++k) {
            const char* name = TabKindName(static_cast<TabKind>(k));
            if (strlen(name) == len && memcmp(name, start, len) == 0) {
                matched = k;
                break;
            }
        }
        if (matched < 0 || used[matched]) return false;
        used[matched] = true;
        if (count >= kTabCount) return false;
        out[count++] = static_cast<TabKind>(matched);
        if (*p == ',') ++p;
    }
    return count == kTabCount;
}

// 与 WarehouseUiFlow.hpp / H3 原版一致的常量（避免跨头文件耦合，这里再声明一次）。
inline constexpr int kTabScrollArtifactId = 1;   // eArtifacts::SPELL_SCROLL
inline constexpr int kSlotNone = -1;
inline constexpr int kSlotSpellbook = 17;
inline constexpr int kSlotMax = 18;

inline constexpr int kRegularTab = 0;

// H3ArtifactSetup::position 的运行时编码（2026-10-14 第十轮实机定案，分段而非整体偏移）：
//   raw 0    = 无装备位；
//   raw 1..6 = 头/肩/颈/右手/左手/胸甲（比 0-based 规范整体 +1）；
//   raw 7    = 戒指族（单值；规范里 6/7 双戒指位在表中只出现一个）；
//   raw 8+   = 与规范 0-based 完全一致：8=靴子、9..12=杂物1..4、13..16=战争机器、
//              17=魔法书、18=杂物5。
// 证据链：第八轮修前症状（头部空、肩部显示头、戒指页混胸甲）锁定 raw 1..6 偏移；
// 第十轮修后症状（戒指页=戒指+靴子、靴子页全是杂物、杂物页空）锁定 raw 7=戒指、
// raw 8=靴子、raw 9=杂物族——高段本就与规范一致，均匀 -1 反而把它们错位一格。
inline int NormalizeArtifactTableSlot(int rawSlot) noexcept {
    if (rawSlot <= 0) return kSlotNone;
    if (rawSlot <= 6) return rawSlot - 1;
    if (rawSlot == 7) return 6;   // 戒指族归到规范的右戒指位（SlotTab 6/7 同为 Ring）
    return rawSlot;               // 8..18 已是规范 0-based
}

// 穿戴位置 → 位置页签；超出原版范围（模组）仍归入“装备”。
inline TabKind SlotTab(int slot) noexcept {
    switch (slot) {
    case 0: return TabKind::Head;
    case 1: return TabKind::Shoulders;
    case 2: return TabKind::Neck;
    case 3: return TabKind::Weapon;
    case 4: return TabKind::Shield;
    case 5: return TabKind::Torso;
    case 6: case 7: return TabKind::Ring;
    case 8: return TabKind::Feet;
    // 原生 NSlots::eArtifactSlots：MISC1..MISC4 = 9..12、MISC5 = 18；
    // BALLISTA/AMMO_CART/FIRST_AID_TENT/CATAPULT = 13..16；SPELLBOOK = 17。
    case 9: case 10: case 11: case 12: case 18: return TabKind::Misc;
    case 13: case 14: case 15: case 16: return TabKind::Machine;
    case 17: return TabKind::Spellbook;
    default: return TabKind::Equipment;
    }
}

// 单个宝物的**具体**页签：卷轴优先于位置；无位置为常规；其余按位置。
inline TabKind BaseTab(int artifactId, int slot) noexcept {
    if (artifactId == kTabScrollArtifactId) return TabKind::Scroll;
    if (slot <= kSlotNone) return TabKind::Regular;
    return SlotTab(slot);
}

// 当前页签是否显示该宝物。artifactId<0（空格）永不匹配。
inline bool IsBackpackSlot(int slot) noexcept {
    return slot <= 12 || slot >= 18;
}

inline bool TabMatches(TabKind tab, int artifactId, int slot) noexcept {
    if (artifactId < 0 || !IsBackpackSlot(slot)) return false;
    if (tab == TabKind::Regular) return true;   // 常规 = 全部可进入背包的道具
    const TabKind base = BaseTab(artifactId, slot);
    if (tab == TabKind::Equipment)
        return base != TabKind::Regular && base != TabKind::Scroll;
    return base == tab;
}

inline constexpr bool IsPositionTab(TabKind tab) noexcept {
    return tab != TabKind::Regular && tab != TabKind::Scroll
        && tab != TabKind::Equipment && tab != TabKind::Machine
        && tab != TabKind::Spellbook;
}

// ---- 页签行左右滚动窗口 ----

struct TabWindow {
    std::size_t begin = 0;    // 窗口起点（页签索引）
    std::size_t visible = 0;  // 窗口内可显示的页签个数
    bool canPrev = false;
    bool canNext = false;
};

inline std::size_t TabOffsetClamp(std::size_t offset, std::size_t total,
                                  std::size_t slots) noexcept {
    if (slots == 0 || total <= slots) return 0;
    const std::size_t maxOffset = total - slots;
    return offset > maxOffset ? maxOffset : offset;
}

inline TabWindow MakeTabWindow(std::size_t offset, std::size_t total,
                               std::size_t slots) noexcept {
    TabWindow window;
    if (slots == 0) return window;
    window.begin = TabOffsetClamp(offset, total, slots);
    window.visible = total < slots ? total : slots;
    window.canPrev = window.begin > 0;
    window.canNext = window.begin + window.visible < total;
    return window;
}

inline std::size_t TabOffsetShift(std::size_t offset, int delta,
                                  std::size_t total, std::size_t slots) noexcept {
    offset = TabOffsetClamp(offset, total, slots);
    if (delta == 0) return offset;
    std::size_t next = offset;
    if (delta < 0) {
        const std::size_t step = static_cast<std::size_t>(-static_cast<std::int64_t>(delta));
        next = step > offset ? 0 : offset - step;
    } else {
        const std::size_t maxOffset = total > slots && slots ? total - slots : 0;
        const std::size_t step = static_cast<std::size_t>(delta);
        next = step > maxOffset - offset ? maxOffset : offset + step;
    }
    return TabOffsetClamp(next, total, slots);
}

// 整理按钮的显隐（round-12 修正）：按“当前页签种类”而不是顺序下标判断。TabOrder 可
// 配置，常规不一定是第 0 个显示槽；用下标判断会让整理按钮跑到别的页签上。
inline constexpr bool TidyVisible(bool expanded, TabKind activeTab) noexcept {
    return expanded && activeTab == TabKind::Regular;
}

// 展示排序开关（round-13 用户裁定）：分类页在筛选结果内保持“相同道具连续”
// （round-8 的展示层排序）；常规页展示**纯存储顺序**——放入逐个追加到末尾、
// 取出后剩余自动前移，不做任何展示层重排（存储顺序只有“整理”能改变）。
inline constexpr bool TabSortsDisplay(TabKind tab) noexcept {
    return tab != TabKind::Regular;
}

} // namespace warehouseui

#endif // H3RNDNEW_WAREHOUSE_UI_CATALOG_HPP
