// ========== 热键设置窗：设置模型 + 持久化 + 键盘钩子层 ==========
// round-21 界面与功能分离：窗口类/下拉框/改键交互/开窗时序已迁至
// HdSettingsUi.inc.cpp（ISettingsUi 的 HD 原生实现）；本文件保留共享助手
// （Utf8ToGbk_/ScanToName_）、模型持久化（PersistTrueRandomFull_/
// PersistHotkey_）与键盘入口钩子（0x4EC1C0，只做翻译与铠甲）。
// 随时可开的全局设置窗（默认 F11）：日志等级 + 全局真随机 + 热键修改。
// 键盘入口 0x4EC1C0：WndProc(0x4F8290) 把 WM_KEYDOWN(0x100)/WM_KEYUP(0x101)
// 交给它，bool __fastcall(hwnd, msg, wparam, lparam)；扫描码 = lparam>>16 & 0xFF，
// 与 H3 内部键码一致（F11=87、F12=88，H3API.hpp eVKey 表）。原版 F1/F4 帮助
// 就是在该函数里直接响应并打开界面的，模式同源。依据：H3Note 调研笔记。

static const UINT32 kKeyMsgProc_ = 0x4EC1C0;

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

// ---- 键盘入口 hook ----

// 本模块的钩子 id（文件在 CrashGuard.hpp 之后包含）。
static const int GUARD_KEY_MSG = GuardRegisterHook_("KeyMsgProc");

// 铠甲（CrashGuard L2）：热键判定段 __try，异常安全默认 =
// 放行原函数；原函数调用在 __try 之外（游戏自身崩溃不吞）。
// 钩子层只做翻译：从 lparam 提取扫描码/自动重复位，界面语义
// （开窗、侦听改键、运行中吞键）全部经 g_uiSettings 契约交给实现。
static bool __stdcall OnKeyMsg_(HiHook* hook, void* hwnd, UINT msg,
    UINT32 wp, UINT32 lp)
{
    bool handled = false;
    __try {
        (void)wp;
        const bool key_down = (msg == 0x100);
        const int scan = (int)((lp >> 16) & 0xFF);
        handled = g_uiSettings->KeyMsg(key_down, scan,
            (lp & 0x40000000) != 0);
    } __except (GuardCrashFilter_(GUARD_KEY_MSG, GetExceptionInformation())) {}

    if (handled) return 0;
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
