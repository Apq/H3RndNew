// ========== UiPortWiring.inc.cpp ==========
// 界面接线（round-21 界面与功能分离，docs/界面与功能分离.md）。
// 三个契约指针（modules/UiPort.hpp）在静态初始化期——本文件位于全部实现
// 之后、Entry.inc.cpp 之前，单 TU 按定义顺序先构造实现对象再赋值——指向
// HD 原生实现；运行期（DllMain 起的任何钩子回调）恒非空，钩子层不判空。
// 顺序约束（改了顺序要同步 docs/界面与功能分离.md 的装配顺序表）：
//   UiPort.hpp → TrueRandom → BankMaximize → HdScenarioUi →
//   SettingsDialog → HdSettingsUi → WarehouseStore → WarehouseUiStageA →
//   WarehouseUiFlow → WarehouseUiButtons → WarehouseUiPanel →
//   WarehouseUiStageB（钩子层）→ 本文件 → Entry。
// 未来换界面：实现新的 IScenarioUi/ISettingsUi/IWarehouseUi 并改这里
// （单实现直连），或按 H3BattleStore 的 UiPortDispatch_ 模式加配置驱动
// 的运行期分发器（两套实现同 DLL 切换）。

#include "UiPort.hpp"

IScenarioUi* g_uiScenario = &g_hdScenarioUi;
ISettingsUi* g_uiSettings = &g_hdSettingsUi;
IWarehouseUi* g_uiWarehouse = &g_hdWarehouseUi;
