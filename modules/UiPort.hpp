// ========== UiPort.hpp ==========
// 界面契约（docs/界面与功能分离.md，round-21）。参考 H3BattleStore 的
// BattleUiPort.hpp 分层：钩子层（各 On* 钩子与 Entry）只经本接口驱动界面；
// 界面实现不反调钩子层，只调用功能层（模型/持久化/事务）。
// 判定口径：换一套界面后仍然成立的是功能（模型、持久化、事务、审计）；
// 换界面就没了的是界面（像素、控件、手势、命中、窗口时序）。
// 约束：仅游戏主线程调用；不得在 __except 过滤器内调用；实现内禁止栈上
// 大缓冲（路径缓冲守 kPathCap_ 堆规则）。事件参数只用指针/标量，契约方法
// 可能在 SEH 保护体内被调用。
// 包含顺序：本头只用 H3 类型的指针。生产 TU 先 include H3API.hpp 再包含
// 本头（重复声明与既有定义合一，合法）；测试 TU 不含 H3API 时仅有本头的
// 前向声明也能编译（测试里的 Null 实现即证明契约自足）。不得在本头内
// using 任何命名空间。

#ifndef H3RNDNEW_UIPORT_HPP_
#define H3RNDNEW_UIPORT_HPP_

namespace h3 {
struct H3SelectScenarioDialog;
struct H3BaseDlg;
struct H3Hero;
struct H3Msg;
}

// —— 选图对话框界面：开局真随机复选框 + 宝屋最大化 复选框/滑块/百分比 ——
// 实现方 owning 自己的控件 id、布局常量与命中判定；点击后经功能层的
// 持久化函数写模型（g_true_random / g_bank_maximize / g_bank_ratio）。
class IScenarioUi {
public:
    virtual ~IScenarioUi() {}

    // 选图对话框构造完成（原构造函数已执行完）：清界面残留状态并记录
    // 布局清单日志。campaign/load 模式的可见性由实现自理。
    virtual void DialogCreated(h3::H3SelectScenarioDialog* dlg, int mode) = 0;

    // 原消息处理前：吞并自家控件（复选框/标签/滑块拖动）的命中。
    // 返回 true = 消费该消息（钩子层直接返回 1，不再进原函数）。
    virtual bool DialogMessage(h3::H3SelectScenarioDialog* dlg, h3::H3Msg* msg) = 0;

    // 原消息处理完之后：关窗清理（result==2）、首消息摆控件、可见性刷新。
    virtual void DialogMessageDone(h3::H3SelectScenarioDialog* dlg, int result) = 0;
};

// —— F11 设置窗界面：日志等级下拉 + 全局真随机开关 + 热键改键 ——
// 键盘入口钩子（0x4EC1C0）把 WM_KEYDOWN 翻译成本事件；实现方持有
// 「窗在跑/等新键」状态并完成开窗/捕获/取消。返回 true = 消费不进游戏。
class ISettingsUi {
public:
    virtual ~ISettingsUi() {}

    virtual bool KeyMsg(bool keyDown, int scan, bool autoRepeat) = 0;
};

// —— 仓库面板界面：挂在 HD 完整背包 modal 上的仓库外壳 ——
// 两只 modal LoHook（0x039E66 attach / 0x039E80 detach）把上下文翻译成
// 本接口；游戏消息回调（dlg+0x70 替换点）整体委托给 Callback。
// Callback 的实现必须自带 SEH 铠甲，且保证原生 callback 在 __try 之外
// 恰好一次（CrashGuard L2 责任边界，随实现走）。
class IWarehouseUi {
public:
    virtual ~IWarehouseUi() {}

    virtual void Attach(h3::H3BaseDlg* dlg, h3::H3Hero* hero,
        int backpackX, int backpackY) = 0;
    virtual void Detach(h3::H3BaseDlg* dlg) = 0;
    virtual int Callback(void* self, h3::H3Msg* msg) = 0;
};

// 生产 TU 的唯一界面入口（UiPortWiring.inc.cpp 定义并指向 HD 实现）。
// 钩子层与 Entry 不得再触碰任何界面实现内部符号；测试 TU 不链接它们时
// 只要不引用这三个指针即可（引用则由 hook_abi_test 对 H3RndNew.cpp 的
// 整体包含提供定义）。
extern IScenarioUi* g_uiScenario;
extern ISettingsUi* g_uiSettings;
extern IWarehouseUi* g_uiWarehouse;

#endif // H3RNDNEW_UIPORT_HPP_
