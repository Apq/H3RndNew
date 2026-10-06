// ========== 开局真随机 ==========
// 只在单人场景点「开始」到建档返回这一段，替换取随机数的函数。
// 地址依据：H3Note\开局真随机接入点逆向笔记.md。

static const UINT32 kScenarioDlgCtor_ = 0x579CE0; // thiscall(dlg, mode)
static const UINT32 kScenarioDlgProc_ = 0x587FD0; // thiscall(dlg, msg)
static const UINT32 kStartGame_       = 0x58BFB0; // fastcall(this)，开始/装载共用建档入口
static const UINT32 kGameRand_        = 0x50C7C0; // fastcall(lo, hi)
static const UINT32 kCrtRand_         = 0x61842C; // cdecl，随机图生成器直接调它

// id 130 是右上区域下方的「随机图」按钮 (414,105) 200x20，仅作参照。
// 「显示可选场景」在它正上方一行，id 128 同行 (414,81)。

// 文字区：显示在 id=131 的空白 hitbox 上方，最终单独重画以保证层级。
static const int kLabelGap_ = 6;
static const int kLabelW_    = 90;
static const int kCheckId_    = 0x7D01;
static const int kLabelId_    = 0x7D02;
static const int kLabelHitId_ = 0x7D03;
static const int kCheckW_     = 32;
static const int kCheckH_     = 24;

// 游戏界面按 GBK 画字。源码是 UTF-8，这里存转好的字节。
static const char kCheckHint_[] = "\xBF\xAA\xBE\xD6\xD5\xE6\xCB\xE6\xBB\xFA";

// 只在点开始到建档返回之间为 1。战斗、菜单、走动中的取数看到的是 0。
static volatile LONG g_in_new_game_ = 0;
// 界面上下文由构造和原消息处理前的字段快照更新；建档入口一次消费。
// 只有明确的新游戏上下文允许开局替换，未知/读取/保存均走原版。
static_assert(offsetof(H3SelectScenarioDialog, isCampaignMaybe) == 0x64,
    "SoD scenario load flag offset");
static_assert(offsetof(H3SelectScenarioDialog, isLoadingMaybe) == 0x65,
    "SoD scenario save flag offset");
#include "ScenarioContext.hpp"

static const char* ScenarioContextName_(ScenarioContext_ context)
{
    switch (context) {
    case kScenarioNewGame_: return "新游戏";
    case kScenarioLoad_: return "读档";
    case kScenarioSave_: return "保存";
    default: return "未知";
    }
}
// 本次开局窗口内真随机实际替换的次数，退出时落日志定位崩溃点。
static volatile LONG g_replaced_count_ = 0;
// 窗口外被 hook 拦到、按原版走的调用数：验证替换范围没有扩大到战斗/走动。
static volatile LONG g_outside_rand_calls_ = 0; // 0x50C7C0 Rand
static volatile LONG g_outside_crt_calls_ = 0;  // 0x61842C _rand
// 全局模式下「开局窗口外」被真随机替换的次数：全局生效的直接证据。
static volatile LONG g_full_replaced_calls_ = 0;

// 保留分段计数，进度输出统一由完整审计心跳负责。
static void CountFullReplacedRoll_()
{
    InterlockedIncrement(&g_full_replaced_calls_);
}

// 完整入口审计定义在 RandomAudit.inc.cpp；必须在安装 RNG Hook 前初始化。
// Compare bounded read-only snapshots; unchanged chains need no repeated dump.
struct RngPatchSnapshot_ {
    UINT32 patch;
    int type;
    UINT32 default_func;
    UINT32 original_func;
    char owner[128];
};
struct RngChainSnapshot_ {
    BYTE bytes[5];
    int count;
    UINT32 remaining;
    RngPatchSnapshot_ patches[16];
};
static RngChainSnapshot_ g_rng_chain_snapshot_[2] = {};
static bool g_rng_chain_seen_[2] = {};

static void CaptureRngHookChain_(UINT32 address, RngChainSnapshot_& current)
{
    memset(&current, 0, sizeof(current));
    memcpy(current.bytes, reinterpret_cast<const void*>(address), sizeof(current.bytes));
    Patch* patch = _P->GetFirstPatchAt(address);
    while (patch && current.count < 16) {
        RngPatchSnapshot_& p = current.patches[current.count++];
        p.patch = reinterpret_cast<UINT32>(patch);
        p.type = patch->GetType();
        HiHook* hi = p.type == HIHOOK_ ? static_cast<HiHook*>(patch) : nullptr;
        p.default_func = hi ? hi->GetDefaultFunc() : 0;
        p.original_func = hi ? hi->GetOriginalFunc() : 0;
        const char* owner = patch->GetOwner();
        _snprintf(p.owner, sizeof(p.owner) - 1, "%s", owner ? owner : "?");
        patch = patch->GetAppliedAfter();
    }
    current.remaining = reinterpret_cast<UINT32>(patch);
}

static void LogRngHookChain_(const char* phase)
{
    if (g_disable_log || g_log_level > LOG_INFO) return;
    const UINT32 addresses[2] = { kGameRand_, kCrtRand_ };
    bool changed = false;
    for (int entry = 0; entry < 2; ++entry) {
        const UINT32 address = addresses[entry];
        RngChainSnapshot_ current = {};
        CaptureRngHookChain_(address, current);
        if (g_rng_chain_seen_[entry]
            && memcmp(&current, &g_rng_chain_snapshot_[entry], sizeof(current)) == 0) continue;
        changed = true;
        LogInfo("真随机: RNG链[%s][%s] 入口=0x%08X 字节=%02X %02X %02X %02X %02X",
            phase, g_rng_chain_seen_[entry] ? "变化" : "首次", address,
            current.bytes[0], current.bytes[1], current.bytes[2], current.bytes[3], current.bytes[4]);
        for (int i = 0; i < current.count; ++i) {
            const RngPatchSnapshot_& p = current.patches[i];
            LogInfo("真随机: RNG链[%s] 入口=0x%08X 序=%d 所有者=%s 类型=%d "
                "default=0x%08X original=0x%08X", phase, address, i, p.owner,
                p.type, p.default_func, p.original_func);
        }
        if (current.remaining)
            LogWarn("真随机: RNG链[%s] 入口=0x%08X 超过16项，快照截断", phase, address);
        g_rng_chain_snapshot_[entry] = current;
        g_rng_chain_seen_[entry] = true;
    }
    if (!changed) LogDebug("真随机: RNG链[%s] 两入口未变化（沿用前次快照）", phase);
}

// 窗口总数仍在结束时输出，不重复打印按调用次数触发的进度。
static void CountReplacedRoll_()
{
    InterlockedIncrement(&g_replaced_count_);
}

static void PersistTrueRandom_()
{
    char value[8] = {};
    _snprintf(value, sizeof(value) - 1, "%d", g_true_random ? 1 : 0);
    if (!IniWriteKeyUtf8(g_user_ini_path, "General", "TrueRandom", value))
        LogError("真随机: 写入 user.ini 失败");
}

// 取 0..0x7FFF。失败返回 -1；持续故障按倍增累计报告，审计仍逐次计数。
static volatile LONG g_true_rng_failures_ = 0;
static int NextTrue15_()
{
    UINT32 value = 0;
    const NTSTATUS status = BCryptGenRandom(nullptr,
        reinterpret_cast<PUCHAR>(&value), sizeof(value),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        const ULONG count = static_cast<ULONG>(InterlockedIncrement(&g_true_rng_failures_));
        if (count && (count & (count - 1u)) == 0)
            LogError("真随机: BCryptGenRandom 失败 status=0x%08X 累计=%lu（倍增汇总）",
                static_cast<unsigned>(status), count);
        return -1;
    }
    return static_cast<int>(value & 0x7FFFu);
}

// HD「其它选项」用 ChkBlue.def 的静态图，帧 0 空、帧 1 勾。
static void ApplyCheckFrame_(H3DlgDef* box)
{
    if (!box) return;
    box->SetFrame(g_true_random ? 1 : 0);
    box->Draw();
    box->ParentRedraw();
}

static bool PointInside_(H3DlgItem* item, int x, int y)
{
    if (!item) return false;
    return x >= item->GetAbsoluteX()
        && x < item->GetAbsoluteX() + item->GetWidth()
        && y >= item->GetAbsoluteY()
        && y < item->GetAbsoluteY() + item->GetHeight();
}

// Debug 只保留布局锚点、模式按钮和自有控件；Trace 才展开完整清单。
// 两个阶段仍可对照 HD 重排；提示按 GBK 十六进制记录。
static void LogInventory_(H3SelectScenarioDialog* dlg, const char* phase)
{
    if (!dlg || g_disable_log || g_log_level > LOG_DEBUG) return;
    H3DlgItem** const items = dlg->GetList().begin();
    const int count = static_cast<int>(dlg->GetList().Count());
    const bool complete = g_log_level == LOG_TRACE;
    LogDebug("真随机: [%s] 控件共 %d 项，输出=%s", phase, count,
        complete ? "完整(trace)" : "关键(debug)");
    for (int i = 0; i < count; ++i) {
        H3DlgItem* item = items[i];
        if (!item) continue;
        const int id = item->GetID();
        const bool key = (id >= 128 && id <= 131) || id == 186 || id == 335
            || id == kCheckId_ || id == kLabelId_ || id == kLabelHitId_;
        if (!complete && !key) continue;
        const UINT32 vt = *reinterpret_cast<UINT32*>(item);
        const char* asset = "?";
        if (vt == H3DlgDef::VTABLE && item->Cast<H3DlgDef>()->GetDef())
            asset = item->Cast<H3DlgDef>()->GetDef()->GetName();
        else if (vt == H3DlgDefButton::VTABLE && item->Cast<H3DlgDefButton>()->GetDef())
            asset = item->Cast<H3DlgDefButton>()->GetDef()->GetName();
        char hintHex[64] = "(无)";
        const UINT32 hintPtr = *reinterpret_cast<UINT32*>(
            reinterpret_cast<BYTE*>(item) + 0x20); // H3DlgItem::hint
        if (hintPtr) {
            hintHex[0] = '\0';
            const BYTE* hint = reinterpret_cast<const BYTE*>(hintPtr);
            for (int j = 0; j < 16 && hint[j]; ++j)
                _snprintf(hintHex + j * 3, sizeof(hintHex) - j * 3,
                    "%02X ", hint[j]);
        }
        auto log = complete ? &LogTrace : &LogDebug;
        log("真随机: [%s] id=%d (%d,%d) %dx%d vt=0x%08X %s 提示=%s",
            phase, item->GetID(), item->GetX(), item->GetY(),
            item->GetWidth(), item->GetHeight(), vt, asset, hintHex);
    }
}


// 已经记过清单的对话框。指针变了就是新开的一次选图界面。
static H3SelectScenarioDialog* s_seen_dlg_ = nullptr;
// H3DlgText::Draw 只改绘图缓冲，不保证当前窗口立即刷新；保存指针，
// 在该对话框每次消息处理结束后再画一次并 Refresh，避免后续重绘抹掉文字。
static H3DlgText* s_label_ = nullptr;

static void RefreshTrueRandomLabel_(H3SelectScenarioDialog* dlg)
{
    if (!dlg || !s_label_ || s_label_->GetID() != kLabelId_) return;
    s_label_->ShowActivate();
    // Draw 写入当前画布，Refresh 再把文字区域刷到窗口；顺序不能反。
    s_label_->Draw();
    s_label_->Refresh();
}

// 对话框收到第一条消息时才放：构造后 HD 还会重排，那时读到的
// 控件位置才是最终位置。id 128 与「显示可选场景」按钮同行，
// 在 (414,81) 200x20；勾选框放在它右边界外 4 像素、行内居中。
static const int kShowMapsRowId_ = 128;

static void PlaceTrueRandomCheckbox_(H3SelectScenarioDialog* dlg,
    const char* phase)
{
    if (!dlg || dlg->isCampaignMaybe || dlg->isLoadingMaybe) return;
    if (dlg->GetH3DlgItem(static_cast<UINT16>(kCheckId_))) return;
    H3DlgItem* row = dlg->GetH3DlgItem(static_cast<UINT16>(kShowMapsRowId_));
    if (!row) {
        LogError("真随机: [%s] 找不到显示可选场景所在行控件", phase);
        return;
    }
    const int x = row->GetX() + row->GetWidth() + 4;
    const int y = row->GetY() + (row->GetHeight() - kCheckH_) / 2;
    // 与 HD「其它选项」相同：ChkBlue.def 静态图，点击在 0x0C 消息里翻转。
    H3DlgDef* box = H3DlgDef::Create(x, y, kCheckW_, kCheckH_,
        kCheckId_, "ChkBlue.def",
        g_true_random ? 1 : 0, 0, FALSE, FALSE);
    if (!box) {
        LogError("真随机: 创建勾选框失败");
        return;
    }
    box->SetHint(kCheckHint_);
    dlg->AddItem(box);
    box->Draw();
    // 透明点击区先加（更高层），文字在上面绘制，避免被点击区遮挡。
    H3DlgTransparentItem* hit = H3DlgTransparentItem::Create(
        x + kCheckW_ + kLabelGap_, y, kLabelW_, kCheckH_, kLabelHitId_);
    if (!hit) {
        LogError("真随机: 创建文字点击区失败");
        return;
    }
    hit->SetHint(kCheckHint_);
    dlg->AddItem(hit);

    H3DlgText* label = H3DlgText::Create(x + kCheckW_ + kLabelGap_, y,
        kLabelW_, kCheckH_, kCheckHint_, "smalfont.fnt",
        1, kLabelId_, 4, 0);
    if (!label) {
        LogError("真随机: 创建文字失败");
        return;
    }
    label->SetHint(kCheckHint_);
    dlg->AddItem(label);
    s_label_ = label;
    // AddItem 会加载控件，但原生对话框后续重绘可能覆盖它；最后单独绘制文字。
    dlg->Redraw();
    RefreshTrueRandomLabel_(dlg);
    box->ParentRedraw();
    RefreshTrueRandomLabel_(dlg);
    // 一次开框只留一条汇总；细节排查用 [Logging] MinLevel=debug 配合清单。
    LogInfo("真随机: [%s] 已加复选框 TrueRandom=%d（显示行 (%d,%d) %dx%d，"
        "勾选框 (%d,%d)，文字 visible=%d active=%d）",
        phase, g_true_random, row->GetX(), row->GetY(),
        row->GetWidth(), row->GetHeight(), x, y,
        label->IsVisible(), label->IsActive());
}


// 本模块的钩子 id（静态初始化期注册；文件在 CrashGuard.hpp 之后包含）。
static const int GUARD_SCENARIO_CTOR = GuardRegisterHook_("ScenarioDlgCtor");
static const int GUARD_SCENARIO_PROC = GuardRegisterHook_("ScenarioDlgProc");
static const int GUARD_START_GAME   = GuardRegisterHook_("StartGame");
static const int GUARD_GAME_RAND    = GuardRegisterHook_("GameRand");
static const int GUARD_CRT_RAND     = GuardRegisterHook_("CrtRand");

// EXTENDED_ 把 this 和原参数都放进栈，替换函数统一 __stdcall。
// 铠甲（CrashGuard L2）：前置拦截段与后置控件段各自 __try；原函数调用在
// __try 之外（游戏自身崩溃不吞），异常安全默认 = 放行原函数。
static int __stdcall OnScenarioProc_(HiHook* hook,
    H3SelectScenarioDialog* dlg, H3Msg* msg)
{
    __try {
        // 必须在原消息处理前观察：回车也可能在原函数内部直接建档。
        if (dlg) {
            const ScenarioContext_ context = ClassifyScenarioContext_(
                dlg->isCampaignMaybe != 0, dlg->isLoadingMaybe != 0);
            const LONG previous = ObserveScenarioContext_(context);
            if (previous != context)
                LogInfo("真随机: 场景消息前 dlg=0x%08X +64=%d +65=%d +66=%d "
                    "cmd=%d subtype=%d id=%d → %s",
                    reinterpret_cast<UINT32>(dlg), dlg->isCampaignMaybe,
                    dlg->isLoadingMaybe, dlg->_f_66,
                    msg ? msg->command : -1, msg ? msg->subtype : -1,
                    msg ? msg->itemId : -1, ScenarioContextName_(context));
        }
        if (msg && dlg
            && msg->command == eMsgCommand::ITEM_COMMAND
            && msg->subtype == eMsgSubtype::LBUTTON_DOWN)
        {
            H3DlgItem* clicked = dlg->GetH3DlgItem(
                static_cast<UINT16>(msg->itemId));
            const bool ours = clicked
                && (clicked->GetID() == kCheckId_
                    || clicked->GetID() == kLabelHitId_);
            const bool over_check = PointInside_(dlg->GetH3DlgItem(
                static_cast<UINT16>(kCheckId_)), msg->GetX(), msg->GetY());
            const bool over_label = PointInside_(dlg->GetH3DlgItem(
                static_cast<UINT16>(kLabelHitId_)), msg->GetX(), msg->GetY());
            if (ours || over_check || over_label) {
                g_true_random = g_true_random ? 0 : 1;
                PersistTrueRandom_();
                ApplyCheckFrame_(dlg->GetDef(static_cast<UINT16>(kCheckId_)));
                LogInfo("真随机: 勾选改为 %d，消息 id=%d",
                    g_true_random, msg->itemId);
                return 1;
            }
        }
    } __except (GuardCrashFilter_(GUARD_SCENARIO_PROC, GetExceptionInformation())) {}

    const int result = THISCALL_2(int, hook->GetDefaultFunc(), dlg, msg);

    __try {
        RefreshTrueRandomLabel_(dlg);
        // 原函数处理完这条消息后再动控件列表，避免在它的遍历中途插入。
        if (dlg && dlg != s_seen_dlg_) {
            s_seen_dlg_ = dlg;
            LogInventory_(dlg, "首消息");
            PlaceTrueRandomCheckbox_(dlg, "首消息");
        }
    } __except (GuardCrashFilter_(GUARD_SCENARIO_PROC, GetExceptionInformation())) {}
    return result;
}

// 铠甲：原函数（构造）必须先执行；后置清单段 __try。
static H3SelectScenarioDialog* __stdcall OnScenarioCtor_(HiHook* hook,
    H3SelectScenarioDialog* dlg, int mode)
{
    H3SelectScenarioDialog* const result = THISCALL_2(H3SelectScenarioDialog*,
        hook->GetDefaultFunc(), dlg, mode);
    __try {
        s_seen_dlg_ = nullptr;
        s_label_ = nullptr;
        const ScenarioContext_ context = dlg ? ClassifyScenarioContext_(
            dlg->isCampaignMaybe != 0, dlg->isLoadingMaybe != 0) : kScenarioUnknown_;
        ObserveScenarioContext_(context);
        LogInfo("真随机: 场景构造 dlg=0x%08X mode=%d +64=%d +65=%d +66=%d → %s",
            reinterpret_cast<UINT32>(dlg), mode,
            dlg ? dlg->isCampaignMaybe : -1, dlg ? dlg->isLoadingMaybe : -1,
            dlg ? dlg->_f_66 : -1, ScenarioContextName_(context));
        // 构造刚完，HD 的重排还没跑，这里只留一份清单做对照。
        if (mode == 0) LogInventory_(dlg, "构造");
    } __except (GuardCrashFilter_(GUARD_SCENARIO_CTOR, GetExceptionInformation())) {}
    return result;
}

// 是否处于替换态：全局模式全程替换（含开局窗口内）；否则仅开局窗口内
// 且开局勾选打开。探针计数只在「既不替换、又在窗口外」时进行，全局模式
// 下窗口外调用是被替换的对象，不再计入探针。
static bool InReplaceMode_(bool full)
{
    return full || (g_true_random != 0 && g_in_new_game_ != 0);
}

// 游戏 Rand(lo, hi)。入口捕获开关；每条返回/SEH 异常路径均由 finally 完成审计。
// 铠甲（CrashGuard L2）：替换判定/真随机取数段 __try，异常回退原版取数
// （安全默认）；原函数调用仅被 __finally 审计覆盖，异常本身继续传播
// （游戏自身崩溃不吞）。
static int __stdcall OnGameRand_(HiHook* hook, int lo, int hi)
{
    const bool full = g_true_random_full != 0;
    const bool replace = InReplaceMode_(full);
    RandomAuditOutcome_ outcome = kAuditAborted_;
    BeginRandomAudit_(0, full);
    int result = 0;
    __try {
        __try {
            if (hi > lo && replace) {
                const int roll = NextTrue15_();
                if (roll >= 0) {
                    CountReplacedRoll_();
                    if (!g_in_new_game_) CountFullReplacedRoll_();
                    outcome = kAuditTrue_;
                    result = roll % (hi - lo + 1) + lo;
                    return result;
                }
            } else if (hi > lo && !g_in_new_game_) {
                InterlockedIncrement(&g_outside_rand_calls_);
            }
        }
        __except (GuardCrashFilter_(GUARD_GAME_RAND, GetExceptionInformation())) {
            // 插件逻辑异常：outcome 保持 kAuditAborted_，走下方原版回退。
        }
        result = FASTCALL_2(int, hook->GetDefaultFunc(), lo, hi);
        outcome = hi <= lo ? kAuditNoDraw_
            : (replace ? kAuditFallback_ : kAuditOriginal_);
        return result;
    }
    __finally {
        EndRandomAudit_(0, full, outcome);
    }
}

// CRT _rand，返回 0..0x7FFF。与 Rand 入口分开计数，不将嵌套调用当两次独立取数。
// 铠甲结构同 OnGameRand_。
static int __stdcall OnCrtRand_(HiHook* hook)
{
    const bool full = g_true_random_full != 0;
    const bool replace = InReplaceMode_(full);
    RandomAuditOutcome_ outcome = kAuditAborted_;
    BeginRandomAudit_(1, full);
    int result = 0;
    __try {
        __try {
            if (replace) {
                const int roll = NextTrue15_();
                if (roll >= 0) {
                    CountReplacedRoll_();
                    if (!g_in_new_game_) CountFullReplacedRoll_();
                    outcome = kAuditTrue_;
                    result = roll;
                    return result;
                }
            } else if (!g_in_new_game_) {
                InterlockedIncrement(&g_outside_crt_calls_);
            }
        }
        __except (GuardCrashFilter_(GUARD_CRT_RAND, GetExceptionInformation())) {
            // 插件逻辑异常：outcome 保持 kAuditAborted_，走下方原版回退。
        }
        result = CDECL_0(int, hook->GetDefaultFunc());
        outcome = replace ? kAuditFallback_ : kAuditOriginal_;
        return result;
    }
    __finally {
        EndRandomAudit_(1, full, outcome);
    }
}

// 铠甲：前置窗口状态段与后置汇总段各自 __try；原函数调用保持
// "__try/__finally 归零"结构，异常继续传播（游戏自身崩溃不吞）。
static int __stdcall OnStartGame_(HiHook* hook, int self)
{
    BOOL allow_window = FALSE;
    ScenarioContext_ context = kScenarioUnknown_;
    __try {
        context = ConsumeScenarioContext_();
        allow_window = AllowsNewGameWindow_(context);
        InterlockedExchange(&g_in_new_game_, 0);
        InterlockedExchange(&g_replaced_count_, 0);
        // +37f 只留诊断，不再把未确认语义的字段作为读档判据。
        LogInfo("真随机: 建档入口 self=0x%08X +37f=%d 上下文=%s 开局窗口=%d",
            self, static_cast<int>(*reinterpret_cast<BYTE*>(self + 0x37f)),
            ScenarioContextName_(context), allow_window);
        // 报告上一窗口结束至今窗口外的取数：非全局模式下都应只计数、不替换；
        // 全局模式下窗口外调用已被替换，探针为零是预期，不说明走原版。
        const LONG outside_rand = InterlockedExchange(&g_outside_rand_calls_, 0);
        const LONG outside_crt = InterlockedExchange(&g_outside_crt_calls_, 0);
        if (g_true_random_full)
            LogInfo("真随机: 全局模式运行中，窗口外替换累计 %d 次；"
                "窗口外探针原版 Rand=%d _rand=%d",
                static_cast<int>(InterlockedExchange(&g_full_replaced_calls_, 0)),
                static_cast<int>(outside_rand),
                static_cast<int>(outside_crt));
        else
            LogInfo("真随机: 上一窗口外探针 Rand=%d _rand=%d（均为原版）",
                static_cast<int>(outside_rand), static_cast<int>(outside_crt));
        LogRngHookChain_("建档前");
        LogRandomAudit_(context == kScenarioLoad_ ? "读档开始前" : "开始前");
        if (!allow_window) {
            LogInfo("真随机: %s路径，开局窗口跳过（全局档仍按其开关运行）",
                ScenarioContextName_(context));
        } else {
            if (g_true_random_full)
                LogInfo("真随机: 全局模式开启，开局窗口内外均走系统随机");
            else if (g_true_random) {
                LogInfo("真随机: 开局取数已换成系统随机");
                // Debug 源采样不参与游戏取数，不能证明地形生成路径。
                if (!g_disable_log && g_log_level <= LOG_DEBUG) {
                    const int sample0 = NextTrue15_();
                    const int sample1 = NextTrue15_();
                    const int sample2 = NextTrue15_();
                    const int sample3 = NextTrue15_();
                    LogDebug("真随机: 系统随机15位采样 %04X %04X %04X %04X",
                        sample0, sample1, sample2, sample3);
                }
            }
            InterlockedExchange(&g_in_new_game_, 1);
        }
    } __except (GuardCrashFilter_(GUARD_START_GAME, GetExceptionInformation())) {}

    int result = 0;
    __try {
        result = FASTCALL_1(int, hook->GetDefaultFunc(), self);
    }
    __finally {
        // SEH 异常穿透时也要归零，不能只靠正常返回路径。
        InterlockedExchange(&g_in_new_game_, 0);
    }
    __try {
        LogRngHookChain_("建档后");
        if (!allow_window) {
            LogInfo("真随机: %s进入完成，开局窗口未启用", ScenarioContextName_(context));
            LogRandomAudit_(context == kScenarioLoad_ ? "读档结束" : "窗口跳过结束");
        } else {
            LogInfo("真随机: 开局取数窗口结束，共替换 %d 次",
                static_cast<int>(g_replaced_count_));
            LogRandomAudit_("开局结束");
        }
    } __except (GuardCrashFilter_(GUARD_START_GAME, GetExceptionInformation())) {}
    return result;
}

static void InstallTrueRandomHooks_()
{
    if (!_PI) return;
    _PI->WriteHiHook(kScenarioDlgCtor_, SPLICE_, EXTENDED_, THISCALL_,
        reinterpret_cast<void*>(&OnScenarioCtor_));
    _PI->WriteHiHook(kScenarioDlgProc_, SPLICE_, EXTENDED_, THISCALL_,
        reinterpret_cast<void*>(&OnScenarioProc_));
    // EXTENDED_ FASTCALL_ marshals ECX and EDX; this entry only has ECX.
    _PI->WriteHiHook(kStartGame_, SPLICE_, EXTENDED_, THISCALL_,
        reinterpret_cast<void*>(&OnStartGame_));
    _PI->WriteHiHook(kGameRand_, SPLICE_, EXTENDED_, FASTCALL_,
        reinterpret_cast<void*>(&OnGameRand_));
    // This patcher CDECL_ bridge restores EAX after the callback. With zero
    // arguments STDCALL_ has identical stack cleanup and preserves the result.
    _PI->WriteHiHook(kCrtRand_, SPLICE_, EXTENDED_, STDCALL_,
        reinterpret_cast<void*>(&OnCrtRand_));
    LogInfo("真随机: Hook 已安装；RNG审计 v2，统计 Rand/_rand 每次入口及结果，"
        "全局期间单列，不覆盖绕过入口的其它模块私有 RNG；原版路径不保证 HD Hook 链的内部算法");
    LogRandomAudit_("审计启动");
}
