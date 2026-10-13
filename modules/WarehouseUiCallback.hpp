// Shared, pure presentation/gesture policy for the in-place warehouse modal.
#ifndef H3RNDNEW_WAREHOUSE_UI_CALLBACK_HPP_
#define H3RNDNEW_WAREHOUSE_UI_CALLBACK_HPP_
namespace warehouseui {
struct IconView {
    bool visible;
    int kind; // 0: artifact.def; 1: def.SpScrArt
    int frame;
};
inline constexpr IconView MakeIconView(int artifactId, int subtype, bool scrollDef) noexcept {
    return artifactId < 0 ? IconView{false, 0, 0}
        : artifactId == 1 && scrollDef ? IconView{true, 1, subtype}
        : IconView{true, 0, artifactId};
}
inline constexpr bool IsNativeBackpackGestureId(int id) noexcept {
    return (id >= 1000 && id < 1064) || (id >= 2000 && id < 2064);
}
inline constexpr bool IsWarehouseControlId(int id) noexcept {
    // 30050/30051 分页箭头；30052 整理按钮（仅常规页可见）；30060..30202 覆盖
    // 关闭/打开/一键取出/一键放入与 30100..30163 仓库槽；30300..30311 页签；
    // 30400..30403 分页页码按钮。30000..30004 是背景/边框，不吞。
    return id == 30050 || id == 30051 || id == 30052
        || (id >= 30060 && id <= 30202) || (id >= 30300 && id <= 30311)
        || (id >= 30400 && id <= 30403);
}
inline constexpr bool ConsumeWarehouseDown(int id) noexcept {
    return IsWarehouseControlId(id) && !IsNativeBackpackGestureId(id);
}
// round-8（2026-10-14 用户实测）：展开态下背包侧左键按下也必须由仓库接管。
// 原实现把 1000..1063 的按下透传原生，实测“点空格会关掉整个界面”——原生命中项
// 只在有物品的格子存在，空格的左键最终落进原生 0x039987 未知回退（写 dlg+0x78=-1）。
inline constexpr bool ConsumeExpandedLeftDown(bool expanded, bool leftDown) noexcept {
    return expanded && leftDown;
}
// 背包 8x8 网格：点坐标 → 槽位（空槽没有原生命中项时的唯一依据）。
// 原点为背包面板左上角 + 原生网格原点，步距 46、单元格 47x47（末列/末行各多 1px）。
inline constexpr int GridSlotFromPoint(int x, int y, int originX, int originY,
                                      int pitch, int columns, int rows,
                                      int count) noexcept {
    if (pitch <= 0 || columns <= 0 || rows <= 0) return -1;
    const int dx = x - originX;
    const int dy = y - originY;
    if (dx < 0 || dy < 0) return -1;
    const int col = dx / pitch;
    const int row = dy / pitch;
    if (col >= columns || row >= rows) return -1;
    const int slot = row * columns + col;
    return slot < count ? slot : -1;
}
inline constexpr bool ExecutesWarehouseAction(int subtype) noexcept {
    return subtype == 0xD;
}
template <typename NativeCallback>
inline int CallNativeOnce(NativeCallback&& nativeCallback) {
    return nativeCallback(); // No finalize-close phase after a successful move.
}
} // namespace warehouseui
#endif
