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
// round-19（用户裁定）：一键取出/一键放入/关闭X/背包仓库这四个按钮的“按下→弹起”
// 视觉必须与「整理」按钮一致。原实现完全依赖原生按下位（item +0x16 & 1）触发重绘，
// 实测只有整理按钮看得到按下再弹起、其余四个看不到（用户原话：“点击整理后,它有按下
// 又弹起的感觉,但其它的没有这个感觉”）。因此按下态改由本模块自己维护：
// DOWN 置位、CLICK/光标离开按钮/隐藏时清位，四态统一走 ButtonVisualState。
inline constexpr int kWhUiOpenButtonId = 30200;
inline constexpr int kWhUiCloseButtonId = 30060;
inline constexpr int kWhUiTakeAllButtonId = 30201;
inline constexpr int kWhUiPutAllButtonId = 30202;
inline constexpr int kWhUiTidyButtonId = 30052;
// 按钮数组下标（与 WarehouseUiButtons.inc.cpp 的 g_wh_ui_buttons_ 逐项对应）：
// 0=打开 1=关闭 2=取出 3=放入 4=整理；非按钮控件返回 -1。
inline constexpr int ButtonIndexFromId(int id) noexcept {
    return id == kWhUiOpenButtonId ? 0
        : id == kWhUiCloseButtonId ? 1
        : id == kWhUiTakeAllButtonId ? 2
        : id == kWhUiPutAllButtonId ? 3
        : id == kWhUiTidyButtonId ? 4 : -1;
}
// 四态优先级：禁用 > 按下 > 悬停 > 普通（与原实现同序，只改了按下位的来源）。
// round-20 起生产路径不再传 disabled（一键取出/放入不做“能不能取放”判断），但仍保留
// 该参数与优先级：它是纯函数，也是这一组状态的唯一定义处。
inline constexpr int ButtonVisualState(bool disabled, bool pressed,
                                      bool hovered) noexcept {
    return disabled ? 3 : pressed ? 1 : hovered ? 2 : 0;
}
// round-20（用户裁定）：五个按钮都不需要“焦点/选中”语义，每次刷新统一清掉这两位。
// 原生 item 状态字（+0x16，H3CstDlgControls.hpp 命名 vs 实测语义）：
//   0x01 PRESSED、0x02 ACTIVE、0x04 VISIBLE、0x08 SHADED、0x10 SELECTED、
//   0x20 禁用（H3DlgItem::IsEnabled() 返回 !(state & 0x20)，置位即禁用，枚举名
//   ENABLED 是命名笔误）、0x40 FOCUSED。
// 其中 SELECTED(0x10) 会让原生 H3DlgDefButton::vDrawToWindow(0x00456620) 改用
// [item+0x40] 帧来画，是唯一能表现“选中/焦点”的位；原生 vGetFocus/vLoseFocus
// 是空 ret（0x0051BD60），因此焦点位只能由别处置上——一律清除最稳。
inline constexpr unsigned kWhUiButtonFocusBits = 0x10 | 0x40;
inline constexpr bool IsWarehouseControlId(int id) noexcept {
    // 30052 整理按钮（仅常规页签可见）；30060..30202 覆盖关闭/打开/一键取出/一键
    // 放入与 30100..30163 仓库槽；30300..30311 页签；30400..30411 分页页码按钮
    // （round-12：两端 30050/30051 箭头已删除；页码按两位数定宽、18px 边距内摆满，
    // 当前 10 个，区间留少量余量）。30000..30004 是背景/边框，不吞。
    // 五个按钮 ID 一律经 ButtonIndexFromId 判定，避免两处硬编码走偏。
    return ButtonIndexFromId(id) >= 0
        || (id >= 30060 && id <= 30202) || (id >= 30300 && id <= 30311)
        || (id >= 30400 && id <= 30411);
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
// round-8.1（实机回归）：几何兜底只允许兜“背包区域内但没有已知控件”的点击。
// 打开按钮(30200)位于背包面板矩形内部，若先做几何判定会被“区域内”吞掉，
// 表现为“点仓库按钮没反应”。已知仓库控件必须优先走各自的 ID 分派。
inline constexpr bool UsesGeometryFallback(int id) noexcept {
    return !IsWarehouseControlId(id);
}
template <typename NativeCallback>
inline int CallNativeOnce(NativeCallback&& nativeCallback) {
    return nativeCallback(); // No finalize-close phase after a successful move.
}
} // namespace warehouseui
#endif
