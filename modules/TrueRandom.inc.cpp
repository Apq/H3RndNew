// ========== 开局真随机 ==========
// 只在单人场景点「开始」到建档返回这一段，替换取随机数的函数。
// 地址依据：H3Note\开局真随机接入点逆向笔记.md。

static const UINT32 kScenarioDlgCtor_ = 0x579CE0; // thiscall(dlg, mode)
static const UINT32 kScenarioDlgProc_ = 0x587FD0; // thiscall(dlg, msg)
static const UINT32 kStartGame_       = 0x58BFB0; // fastcall(this)，点开始
static const UINT32 kGameRand_        = 0x50C7C0; // fastcall(lo, hi)
static const UINT32 kCrtRand_         = 0x61842C; // cdecl，随机图生成器直接调它

static const int kShowMapsX_  = 0x39;
static const int kShowMapsW_  = 0x151;
static const int kShowMapsY_  = 0x217;
static const int kCheckId_    = 0x7D01;
static const int kLabelId_    = 0x7D02;
static const int kCheckW_     = 32;
static const int kCheckH_     = 24;
static const int kLabelRight_ = 0x30C; // 不越过右侧开始/返回按钮列

static const char kCheckText_[] = "开局真随机";

// 只在点开始到建档返回之间为 1。战斗、菜单、走动中的取数看到的是 0。
static volatile LONG g_in_new_game_ = 0;

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

static void ApplyCheckFrame_(H3DlgDef* box)
{
    if (!box) return;
    box->SetFrame(g_true_random ? 1 : 0);
    box->Draw();
    box->ParentRedraw();
}

static void AddTrueRandomCheckbox_(H3SelectScenarioDialog* dlg)
{
    if (!dlg) return;
    // +0x64 战役，+0x65 读档。教程不进这个构造。
    if (dlg->isCampaignMaybe || dlg->isLoadingMaybe) return;
    if (dlg->GetH3DlgItem(static_cast<UINT16>(kCheckId_))) return;

    const int x = kShowMapsX_ + kShowMapsW_ + 8;
    H3DlgDef* box = H3DlgDef::Create(x, kShowMapsY_, kCheckW_, kCheckH_,
        kCheckId_, NH3Dlg::Assets::ON_OFF_CHECKBOX,
        g_true_random ? 1 : 0, 0, FALSE, FALSE);
    if (!box) {
        LogError("真随机: 创建勾选框失败");
        return;
    }
    dlg->AddItem(box);

    const int text_x = x + kCheckW_ + 4;
    const int text_w = kLabelRight_ - text_x;
    if (text_w > 8) {
        H3DlgText* label = H3DlgText::Create(text_x, kShowMapsY_,
            text_w, kCheckH_, kCheckText_, NH3Dlg::Text::SMALL,
            static_cast<INT32>(eTextColor::REGULAR), kLabelId_,
            static_cast<INT32>(eTextAlignment::MIDDLE_LEFT), 0);
        if (label) dlg->AddItem(label);
    }
    LogInfo("真随机: 选图界面已加复选框 TrueRandom=%d", g_true_random);
}

// EXTENDED_ 把 this 和原参数都放进栈，替换函数统一 __stdcall。
static int __stdcall OnScenarioProc_(HiHook* hook,
    H3SelectScenarioDialog* dlg, H3Msg* msg)
{
    if (msg && msg->itemId == kCheckId_
        && msg->command == eMsgCommand::ITEM_COMMAND
        && msg->subtype == eMsgSubtype::LBUTTON_CLICK)
    {
        g_true_random = g_true_random ? 0 : 1;
        PersistTrueRandom_();
        ApplyCheckFrame_(dlg ? dlg->GetDef(
            static_cast<UINT16>(kCheckId_)) : nullptr);
        LogInfo("真随机: 勾选改为 %d", g_true_random);
        return 1;
    }
    return THISCALL_2(int, hook->GetDefaultFunc(), dlg, msg);
}

static void __stdcall OnScenarioCtor_(HiHook* hook,
    H3SelectScenarioDialog* dlg, int mode)
{
    THISCALL_2(void, hook->GetDefaultFunc(), dlg, mode);
    if (mode == 0) AddTrueRandomCheckbox_(dlg);
}

// 游戏 Rand(lo, hi)。两端无区间时原函数不取数，这里也不取。
static int __stdcall OnGameRand_(HiHook* hook, int lo, int hi)
{
    if (g_true_random && g_in_new_game_ && hi > lo) {
        const int span = hi - lo + 1;
        const int roll = NextTrue15_();
        if (roll >= 0) return roll % span + lo;
    }
    return FASTCALL_2(int, hook->GetDefaultFunc(), lo, hi);
}

// 随机图生成器直接调的 CRT _rand，返回值同样是 0..0x7FFF。
static int __stdcall OnCrtRand_(HiHook* hook)
{
    if (g_true_random && g_in_new_game_) {
        const int roll = NextTrue15_();
        if (roll >= 0) return roll;
    }
    return CDECL_0(int, hook->GetDefaultFunc());
}

static int __stdcall OnStartGame_(HiHook* hook, int self)
{
    const LONG depth = InterlockedIncrement(&g_in_new_game_);
    if (g_true_random && depth == 1)
        LogInfo("真随机: 开局取数已换成系统随机");
    const int result = FASTCALL_1(int, hook->GetDefaultFunc(), self);
    InterlockedDecrement(&g_in_new_game_);
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
    LogInfo("真随机: Hook 已安装");
}
