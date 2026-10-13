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
    // 30050/30051 分页箭头；30060..30202 覆盖关闭/打开/一键取出/一键放入与
    // 30100..30163 仓库槽；30300..30311 页签；30400..30403 分页页码按钮。
    // 30000..30004 是背景/边框，不吞。
    return id == 30050 || id == 30051
        || (id >= 30060 && id <= 30202) || (id >= 30300 && id <= 30311)
        || (id >= 30400 && id <= 30403);
}
inline constexpr bool ConsumeWarehouseDown(int id) noexcept {
    return IsWarehouseControlId(id) && !IsNativeBackpackGestureId(id);
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
