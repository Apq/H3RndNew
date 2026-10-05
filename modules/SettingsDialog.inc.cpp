// ========== 热键设置窗 ==========
// 随时可开的全局设置窗（默认 F11）：全局真随机复选框 + 热键修改。
// 键盘入口 0x4EC1C0：WndProc(0x4F8290) 把 WM_KEYDOWN(0x100)/WM_KEYUP(0x101)
// 交给它，bool __fastcall(hwnd, msg, wparam, lparam)；扫描码 = lparam>>16 & 0xFF，
// 与 H3 内部键码一致（F11=87、F12=88，H3API.hpp eVKey 表）。原版 F1/F4 帮助
// 就是在该函数里直接响应并打开界面的，模式同源。依据：H3Note 调研笔记。

static const UINT32 kKeyMsgProc_ = 0x4EC1C0;

// 输入管理器（DAT_00699530）：原函数入口检查非空且 +0x34==1 才处理消息，
// 弹窗沿用同一守卫，避免启动/切换期抢跑。
static bool InputMgrReady_()
{
    const UINT32 mgr = *reinterpret_cast<const UINT32*>(0x699530);
    return mgr != 0
        && *reinterpret_cast<const INT32*>(mgr + 0x34) == 1;
}

// ---- 状态 ----
static volatile LONG s_settings_dlg_running_ = 0; // 设置窗模态运行中
static volatile LONG s_listen_new_hotkey_ = 0;    // 等待玩家按新热键

// ---- 文本工具 ----

// 源码 UTF-8，游戏字体按 GBK 画字（开局模块已验证），运行时转换。
static int Utf8ToGbk_(const char* utf8, char* out, int out_size)
{
    if (!utf8 || !out || out_size <= 0) return 0;
    wchar_t wide[192];
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 192);
    if (wn <= 0) { out[0] = 0; return 0; }
    const int n = WideCharToMultiByte(936, 0, wide, -1, out, out_size,
        nullptr, nullptr);
    if (n <= 0) { out[0] = 0; return 0; }
    return n;
}

// 扫描码 -> 键名（ASCII 直接可用；中文名先产 UTF-8 再由调用方转 GBK）。
static void ScanToName_(int scan, char* out, int out_size)
{
    if (scan >= 0x3B && scan <= 0x44)
        _snprintf(out, out_size - 1, "F%d", scan - 0x3B + 1);
    else if (scan == 0x57) _snprintf(out, out_size - 1, "F11");
    else if (scan == 0x58) _snprintf(out, out_size - 1, "F12");
    else if (scan >= 0x1E && scan <= 0x26)
        _snprintf(out, out_size - 1, "%c", 'A' + scan - 0x1E);
    else if (scan >= 0x02 && scan <= 0x0A)
        _snprintf(out, out_size - 1, "%c", '1' + scan - 0x02);
    else if (scan == 0x0B) _snprintf(out, out_size - 1, "0");
    else if (scan == 0x39) _snprintf(out, out_size - 1, "空格");
    else if (scan == 0x1C) _snprintf(out, out_size - 1, "回车");
    else if (scan == 0x0F) _snprintf(out, out_size - 1, "Tab");
    else if (scan == 0x0E) _snprintf(out, out_size - 1, "退格");
    else if (scan == 1)    _snprintf(out, out_size - 1, "ESC");
    else _snprintf(out, out_size - 1, "键%d", scan);
    out[out_size - 1] = 0;
}

// ---- 持久化（写玩家层 user.ini，与开局勾选同机制）----

static void PersistTrueRandomFull_()
{
    char value[8] = {};
    _snprintf(value, sizeof(value) - 1, "%d", g_true_random_full ? 1 : 0);
    if (!IniWriteKeyUtf8(g_user_ini_path, "General", "TrueRandomFull", value))
        LogError("真随机: 写入 user.ini(TrueRandomFull) 失败");
}

static void PersistHotkey_()
{
    char value[8] = {};
    _snprintf(value, sizeof(value) - 1, "%d", g_settings_hotkey_scan);
    if (!IniWriteKeyUtf8(g_user_ini_path, "General", "SettingsHotkeyScan",
            value))
        LogError("真随机: 写入 user.ini(SettingsHotkeyScan) 失败");
}

// ---- 设置窗 ----

static const int kSdTitleId_     = 0x7E10;
static const int kSdCheckId_     = 0x7E11; // ChkBlue.def 勾选框
static const int kSdCheckHitId_  = 0x7E12; // 标签透明点击区
static const int kSdKeyHintId_   = 0x7E13; // 热键行「热键：」文字
static const int kSdKeyHitId_    = 0x7E14; // 键名透明点击区
static const int kSdKeyNameId_   = 0x7E15; // 键名文字
static const int kSdOkId_        = 0x7E16;
static const int kSdKeyFrameId_  = 0x7E17; // 键名可见框（Box66x32.pcx）
static const int kSdW_ = 340;
static const int kSdH_ = 190;

struct SettingsDlg_ final : public H3Dlg
{
    H3DlgDef*  check_box_ = nullptr;
    H3DlgPcx*  key_frame_ = nullptr;
    H3DlgText* key_name_  = nullptr;

    SettingsDlg_() : H3Dlg(kSdW_, kSdH_) {}

    BOOL OnCreate() override
    {
        char gbk[160];
        H3DlgText* title = H3DlgText::Create(0, 12, kSdW_, 24,
            Utf8ToGbk_("真随机 设置", gbk, sizeof(gbk)) ? gbk : "",
            "smalfont.fnt", 1, kSdTitleId_, 5, 0);
        if (title) AddItem(title);

        // 全局真随机：ChkBlue 勾选框 + 可点标签（与开局勾选同款素材）。
        static const char kFullHint_[] =
            "开启后游戏全程随机数改用系统级随机；"
            "关闭时仅新游戏建档期间替换";
        char hint_gbk[160];
        const bool hint_ok = Utf8ToGbk_(kFullHint_, hint_gbk,
            sizeof(hint_gbk)) != 0;
        check_box_ = H3DlgDef::Create(60, 52, 32, 24, kSdCheckId_,
            "ChkBlue.def", g_true_random_full ? 1 : 0, 0, FALSE, FALSE);
        if (check_box_) {
            if (hint_ok) check_box_->SetHint(hint_gbk);
            AddItem(check_box_);
        } else {
            LogError("真随机: 设置窗创建勾选框失败");
        }
        H3DlgText* full_label = H3DlgText::Create(100, 52, 200, 24,
            Utf8ToGbk_("全局真随机（全程）", gbk, sizeof(gbk)) ? gbk : "",
            "smalfont.fnt", 1, kSdCheckHitId_, 4, 0);
        if (full_label) {
            if (hint_ok) full_label->SetHint(hint_gbk);
            AddItem(full_label);
        }

        // 热键行：说明文字 + 带可见边框的键名框（Box66x32.pcx）。
        H3DlgText* key_label = H3DlgText::Create(60, 96, 52, 24,
            Utf8ToGbk_("热键：", gbk, sizeof(gbk)) ? gbk : "",
            "smalfont.fnt", 5, kSdKeyHintId_, 4, 0);
        if (key_label) {
            char key_hint_gbk[160];
            const bool key_hint_ok = Utf8ToGbk_(
                "点击框内后按新键（ESC 取消）；F12 已被 SoD_SP 设置占用",
                key_hint_gbk, sizeof(key_hint_gbk)) != 0;
            if (key_hint_ok) key_label->SetHint(key_hint_gbk);
            AddItem(key_label);
        }
        key_frame_ = H3DlgPcx::Create(120, 92, kSdKeyFrameId_,
            NH3Dlg::Assets::BOX_66_32_PCX);
        if (key_frame_) AddItem(key_frame_);
        H3DlgTransparentItem* key_hit = H3DlgTransparentItem::Create(
            120, 92, 66, 32, kSdKeyHitId_);
        if (key_hit) {
            char key_hint_gbk[160];
            const bool key_hint_ok = Utf8ToGbk_(
                "点击框内后按新键（ESC 取消）；F12 已被 SoD_SP 设置占用",
                key_hint_gbk, sizeof(key_hint_gbk)) != 0;
            if (key_hint_ok) key_hit->SetHint(key_hint_gbk);
            AddItem(key_hit);
        }
        key_name_ = H3DlgText::Create(120, 96, 66, 24, "",
            "smalfont.fnt", 5, kSdKeyNameId_, 5, 0);
        if (key_name_) AddItem(key_name_);
        RefreshKeyName_();

        // 确定按钮：closeDialog=TRUE，点击自动关窗；Enter 等效。
        H3DlgDefButton* ok = H3DlgDefButton::Create(138, 140, kSdOkId_,
            "iokay.def", 0, 1, TRUE, NH3VKey::H3VK_ENTER);
        if (ok) AddItem(ok);
        return TRUE;
    }

    BOOL OnLeftClick(INT itemId, H3Msg& msg) override
    {
        (void)msg;
        if (itemId == kSdCheckId_ || itemId == kSdCheckHitId_) {
            g_true_random_full = g_true_random_full ? 0 : 1;
            PersistTrueRandomFull_();
            if (check_box_) {
                check_box_->SetFrame(g_true_random_full ? 1 : 0);
                check_box_->Draw();
                check_box_->ParentRedraw();
            }
            LogInfo("真随机: 全局模式切换为 %d（%s）", g_true_random_full,
                g_true_random_full ? "全程系统级随机" : "仅开局窗口");
            return TRUE;
        }
        if (itemId == kSdKeyHitId_ || itemId == kSdKeyNameId_) {
            InterlockedExchange(&s_listen_new_hotkey_, 1);
            RefreshKeyName_();
            LogInfo("真随机: 等待输入新热键");
            return TRUE;
        }
        return FALSE; // OK 按钮由 closeDialog 默认逻辑关窗
    }

    // 先隐藏动态文字，再重画键名框背景；否则 SetText 后 Draw 会把
    // 新字符串直接叠到旧字符串上。提示文字由对话框框架绘制，不走此路径。
    void ClearKeyName_()
    {
        if (key_name_)
            key_name_->Hide();
        if (key_frame_) {
            key_frame_->Show();
            key_frame_->Draw();
            key_frame_->Refresh();
        }
    }

    // 键名/提示文字重画。动态文字必须采用「隐藏旧字 → 恢复框底 → 画新字」
    // 三步顺序；否则「F11」与「请按新键…」会发生像素叠加。
    void RefreshKeyName_()
    {
        if (!key_name_) return;
        char utf8[64];
        if (s_listen_new_hotkey_)
            _snprintf(utf8, sizeof(utf8) - 1, "请按新键…");
        else
            ScanToName_(g_settings_hotkey_scan, utf8, sizeof(utf8));
        char gbk[64];
        if (!Utf8ToGbk_(utf8, gbk, sizeof(gbk))) return;
        ClearKeyName_();
        key_name_->SetText(gbk);
        key_name_->ShowActivate();
        key_name_->Draw();
        key_name_->Refresh();
    }

    // 关闭前先清掉通过 Draw+Refresh 直写的动态键名像素。
    void OnOK() override { ClearKeyName_(); }
    void OnCancel() override { ClearKeyName_(); }
    void OnClose(INT itemId) override
    {
        (void)itemId;
        ClearKeyName_();
    }

    // 键盘钩子捕获新热键/取消后，从同一线程的窗口外调这里刷新显示。
    void OnHotkeyChanged_()
    {
        RefreshKeyName_();
    }
};

static SettingsDlg_* s_active_dlg_ = nullptr;

// ---- 弹窗（SEH 与 C++ 对象展开不能同函数，拆两层）----

static void RunSettingsDialog_()
{
    SettingsDlg_ dlg;
    s_active_dlg_ = &dlg;
    dlg.Start(); // 模态，返回即关窗
    // 键名文字在侦听期间通过 Draw+Refresh 直接更新过屏幕；
    // 关闭后主动让窗口矩形交底层重绘，避免静止场景留下动态文字残影。
    if (H3WindowManager* wm = H3WindowManager::Get())
        wm->H3Redraw(dlg.GetX(), dlg.GetY(), dlg.GetWidth(), dlg.GetHeight());
    InterlockedExchange(&s_listen_new_hotkey_, 0);
    s_active_dlg_ = nullptr;
}

static void OpenSettingsDialog_()
{
    if (InterlockedCompareExchange(&s_settings_dlg_running_, 1, 0) != 0)
        return;
    LogInfo("真随机: 打开设置窗（全局=%d 热键码=%d）",
        g_true_random_full, g_settings_hotkey_scan);
    __try {
        RunSettingsDialog_();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogError("真随机: 设置窗异常 0x%08X", GetExceptionCode());
    }
    InterlockedExchange(&s_listen_new_hotkey_, 0);
    InterlockedExchange(&s_settings_dlg_running_, 0);
}

// ---- 键盘入口 hook ----

static bool IsModifierScan_(int scan)
{
    return scan == 29 || scan == 42 || scan == 54 // L-Ctrl, L/R-Shift
        || scan == 56 || scan == 58                // L-Alt, R-Alt(扩展)
        || scan == 0x5B || scan == 0x5C;           // L/R-Win
}

static bool __stdcall OnKeyMsg_(HiHook* hook, void* hwnd, UINT msg,
    UINT32 wp, UINT32 lp)
{
    (void)wp;
    const bool key_down = (msg == 0x100);
    const int scan = (int)((lp >> 16) & 0xFF);

    if (s_settings_dlg_running_) {
        if (key_down && s_listen_new_hotkey_) {
            if (scan == 1) { // ESC 取消修改
                InterlockedExchange(&s_listen_new_hotkey_, 0);
                if (s_active_dlg_) s_active_dlg_->OnHotkeyChanged_();
                LogInfo("真随机: 热键修改取消");
            } else if (!IsModifierScan_(scan)) {
                char name[32];
                ScanToName_(scan, name, sizeof(name));
                g_settings_hotkey_scan = scan;
                PersistHotkey_();
                InterlockedExchange(&s_listen_new_hotkey_, 0);
                if (s_active_dlg_) s_active_dlg_->OnHotkeyChanged_();
                LogInfo("真随机: 设置热键改为 %s（扫描码 %d）", name, scan);
            }
            return 0; // 侦听期间按键全部吞掉（修饰键继续侦听）
        }
        // 设置窗开着时，热键本身吞掉避免递归弹窗；其余交给窗/原函数。
        if (key_down && scan == g_settings_hotkey_scan)
            return 0;
        return FASTCALL_4(bool, hook->GetDefaultFunc(), hwnd, msg, wp, lp);
    }

    if (key_down && !(lp & 0x40000000)   // 忽略按住自动重复
        && scan == g_settings_hotkey_scan && InputMgrReady_()) {
        OpenSettingsDialog_();
        return 0; // 该键已消费，不进游戏
    }
    return FASTCALL_4(bool, hook->GetDefaultFunc(), hwnd, msg, wp, lp);
}

static void InstallSettingsHooks_()
{
    if (!_PI) return;
    _PI->WriteHiHook(kKeyMsgProc_, SPLICE_, EXTENDED_, FASTCALL_,
        reinterpret_cast<void*>(&OnKeyMsg_));
    char name[32];
    ScanToName_(g_settings_hotkey_scan, name, sizeof(name));
    LogInfo("真随机: 设置热键 Hook 已安装（0x4EC1C0，当前 %s/%d）",
        name, g_settings_hotkey_scan);
}
