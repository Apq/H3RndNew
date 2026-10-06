// ========== 开局真随机 ==========
// 只在单人场景点「开始」到建档返回这一段，替换取随机数的函数。
// 地址依据：H3Note\开局真随机接入点逆向笔记.md。

static const UINT32 kScenarioDlgCtor_ = 0x579CE0; // thiscall(dlg, mode)
static const UINT32 kScenarioDlgProc_ = 0x587FD0; // thiscall(dlg, msg)
static const UINT32 kStartGame_       = 0x58BFB0; // fastcall(this)，点开始
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
// 本次开局窗口内真随机实际替换的次数，退出时落日志定位崩溃点。
static volatile LONG g_replaced_count_ = 0;
// 窗口外被 hook 拦到、按原版走的调用数：验证替换范围没有扩大到战斗/走动。
static volatile LONG g_outside_rand_calls_ = 0; // 0x50C7C0 Rand
static volatile LONG g_outside_crt_calls_ = 0;  // 0x61842C _rand
// 全局模式下「开局窗口外」被真随机替换的次数：全局生效的直接证据。
static volatile LONG g_full_replaced_calls_ = 0;

// 全局模式下窗口外替换累计到一定量级留一条日志，证明全程替换在跑。
static void CountFullReplacedRoll_()
{
    const LONG n = InterlockedIncrement(&g_full_replaced_calls_);
    if ((n & 0x3FFFF) == 0) // 每 26 万次一条，避免刷屏
        LogInfo("真随机: 窗口外替换分段累计 %d 次",
            static_cast<int>(n));
}

// 完整入口审计定义在 RandomAudit.inc.cpp；必须在安装 RNG Hook 前初始化。

// 保留旧替换进度；完整入口心跳由 EndRandomAudit_ 独立驱动，原版路径也有汇总。
static void CountReplacedRoll_()
{
    const LONG n = InterlockedIncrement(&g_replaced_count_);
    if ((n & 0xFFFF) == 0) {
        LogInfo("真随机: 已替换 %d 次", static_cast<int>(n));
    }
}

static void PersistTrueRandom_()
{
    char value[8] = {};
    _snprintf(value, sizeof(value) - 1, "%d", g_true_random ? 1 : 0);
    if (!IniWriteKeyUtf8(g_user_ini_path, "General", "TrueRandom", value))
        LogError("真随机: 写入 user.ini 失败");
}

// 取 0..0x7FFF。失败返回 -1，调用方退回原版伪随机。
static int NextTrue15_()
{
    UINT32 value = 0;
    const NTSTATUS status = BCryptGenRandom(nullptr,
        reinterpret_cast<PUCHAR>(&value), sizeof(value),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        LogError("真随机: BCryptGenRandom 失败 status=0x%08X",
            static_cast<unsigned>(status));
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

// 记下当前对话框全部控件的 id、位置、尺寸、资源和提示字节。
// 提示按 GBK 十六进制打印（「显示」是 CF D4 CA BE），用来认出
// 玩家看到的那颗「显示可选场景」按钮到底是哪个控件。
static void LogInventory_(H3SelectScenarioDialog* dlg, const char* phase)
{
    if (!dlg) return;
    H3DlgItem** const items = dlg->GetList().begin();
    const int count = static_cast<int>(dlg->GetList().Count());
    // 排查期工具：600+ 行/次，降到 debug 级，默认 MinLevel=info 不落盘。
    LogDebug("真随机: [%s] 控件清单共 %d 项", phase, count);
    for (int i = 0; i < count; ++i) {
        H3DlgItem* item = items[i];
        if (!item) continue;
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
        LogDebug("真随机: [%s] id=%d (%d,%d) %dx%d vt=0x%08X %s 提示=%s",
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
static void __stdcall OnScenarioCtor_(HiHook* hook,
    H3SelectScenarioDialog* dlg, int mode)
{
    THISCALL_2(void, hook->GetDefaultFunc(), dlg, mode);
    __try {
        s_seen_dlg_ = nullptr;
        s_label_ = nullptr;
        // 构造刚完，HD 的重排还没跑，这里只留一份清单做对照。
        if (mode == 0) LogInventory_(dlg, "构造");
    } __except (GuardCrashFilter_(GUARD_SCENARIO_CTOR, GetExceptionInformation())) {}
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
            if (hi <= lo) {
                result = FASTCALL_2(int, hook->GetDefaultFunc(), lo, hi);
                outcome = kAuditNoDraw_;
                return result;
            }
            if (replace) {
                const int roll = NextTrue15_();
                if (roll >= 0) {
                    CountReplacedRoll_();
                    if (!g_in_new_game_) CountFullReplacedRoll_();
                    outcome = kAuditTrue_;
                    result = roll % (hi - lo + 1) + lo;
                    return result;
                }
            } else if (!g_in_new_game_) {
                InterlockedIncrement(&g_outside_rand_calls_);
            }
        }
        __except (GuardCrashFilter_(GUARD_GAME_RAND, GetExceptionInformation())) {
            // 插件逻辑异常：outcome 保持 kAuditAborted_，走下方原版回退。
        }
        result = FASTCALL_2(int, hook->GetDefaultFunc(), lo, hi);
        outcome = replace ? kAuditFallback_ : kAuditOriginal_;
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
    BOOL is_load_game = FALSE;
    __try {
        // 读档路径自证（2026-10-06 实测修正）：0x58BFB0 同时服务「新游戏开始」
        // 与「读取存档进入」，由 this+0x37f 区分（0=新游戏，非0=读档，读档分支
        // FUN_00587c70 加载存档）。读档不应受开局真随机影响：存档里的世界是
        // 既成的，重掷随机会破坏读档一致性。读档只走审计，不开替换窗口。
        const BOOL load_flag = *(char*)(self + 0x37f) != 0;
        is_load_game = load_flag;
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
        LogRandomAudit_(is_load_game ? "读档开始前" : "开始前");
        if (is_load_game) {
            // 读档：不开开局真随机窗口；全局档不受影响（不依赖开局窗口）。
            LogInfo("真随机: 读档路径（this+0x37f!=0），开局窗口跳过，开局档不介入");
        } else {
        // 若上次异常退出没归零，这里强制重置并留痕，防止泄漏殃及战斗取数。
        const LONG leaked = InterlockedExchange(&g_in_new_game_, 1);
        if (leaked != 0)
            LogError("真随机: 上次开局窗口未归零(%d)，已强制重置", leaked);
        InterlockedExchange(&g_replaced_count_, 0);
        if (g_true_random_full)
            LogInfo("真随机: 全局模式开启，开局窗口内外均走系统随机");
        else if (g_true_random)
            LogInfo("真随机: 开局取数已换成系统随机");
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
        if (is_load_game) {
            LogInfo("真随机: 读档进入完成，开局档全程未介入");
            LogRandomAudit_("读档结束");
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
    _PI->WriteHiHook(kStartGame_, SPLICE_, EXTENDED_, FASTCALL_,
        reinterpret_cast<void*>(&OnStartGame_));
    _PI->WriteHiHook(kGameRand_, SPLICE_, EXTENDED_, FASTCALL_,
        reinterpret_cast<void*>(&OnGameRand_));
    _PI->WriteHiHook(kCrtRand_, SPLICE_, EXTENDED_, CDECL_,
        reinterpret_cast<void*>(&OnCrtRand_));
    LogInfo("真随机: Hook 已安装；RNG审计 v2，统计 Rand/_rand 每次入口及结果，"
        "全局期间单列，不覆盖绕过入口的其它模块私有 RNG；原版路径不保证 HD Hook 链的内部算法");
    LogRandomAudit_("审计启动");
}
