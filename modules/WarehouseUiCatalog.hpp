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

inline constexpr TabKind kTabOrder[kTabCount] = {
    TabKind::Regular, TabKind::Scroll, TabKind::Equipment, TabKind::Head,
    TabKind::Shoulders, TabKind::Neck, TabKind::Weapon, TabKind::Shield,
    TabKind::Torso, TabKind::Ring, TabKind::Feet, TabKind::Misc};

// 与 WarehouseUiFlow.hpp / H3 原版一致的常量（避免跨头文件耦合，这里再声明一次）。
inline constexpr int kTabScrollArtifactId = 1;   // eArtifacts::SPELL_SCROLL
inline constexpr int kSlotNone = -1;
inline constexpr int kSlotSpellbook = 17;
inline constexpr int kSlotMax = 18;

inline constexpr int kRegularTab = 0;

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

} // namespace warehouseui

#endif // H3RNDNEW_WAREHOUSE_UI_CATALOG_HPP
