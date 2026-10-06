// ========== Entry.inc.cpp ==========
// 插件入口。新游戏建档随机源的 Hook 以后挂在 StartPlugin。

#pragma comment(lib, "version.lib")

static void LogSelfVersion_()
{
    wchar_t wpath[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, wpath, MAX_PATH);
    char utf8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8,
        (int)sizeof(utf8), nullptr, nullptr);
    char ver[64] = "?";
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(wpath, &handle);
    if (size) {
        BYTE* data = new BYTE[size];
        if (GetFileVersionInfoW(wpath, 0, size, data)) {
            struct LangCodePage { WORD lang, codepage; };
            LangCodePage* langs = nullptr; UINT lang_count = 0;
            if (VerQueryValueW(data, L"\\VarFileInfo\\Translation",
                    (LPVOID*)&langs, &lang_count)
                && lang_count > 0)
            {
                wchar_t key[80] = {};
                swprintf(key, 80, L"\\StringFileInfo\\%04X%04X\\ProductVersion",
                    langs[0].lang, langs[0].codepage);
                wchar_t* product = nullptr; UINT len = 0;
                if (VerQueryValueW(data, key, (LPVOID*)&product, &len) && product)
                    WideCharToMultiByte(CP_UTF8, 0, product, -1, ver,
                        (int)sizeof(ver), nullptr, nullptr);
            }
        }
        delete[] data;
    }
    LogInfo("真随机 v%s | DLL=%s", ver, utf8);
}

static void StartPlugin()
{
    InitRandomAudit_();
    LogSelfVersion_();
    LogInfo("真随机: plugin enabled. TrueRandom=%d Full=%d Hotkey=%d"
        "（0=伪随机 1=真随机；Full=全局；Hotkey=设置键扫描码）",
        g_true_random, g_true_random_full, g_settings_hotkey_scan);
    InstallTrueRandomHooks_();
    InstallSettingsHooks_();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    static bool initialized = false;
    if (reason == DLL_PROCESS_ATTACH && !initialized) {
        initialized = true;
        g_hModule = hModule;
        auto utf8_from_wide = [](const wchar_t* w, char* out, int out_size) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_size, nullptr, nullptr);
            if (out_size > 0) out[out_size - 1] = 0;
        };
        wchar_t* wpath = new wchar_t[kPathCap_ / 2]();
        auto set_dll_dir_file = [&](const wchar_t* name, char* out_utf8) {
            GetModuleFileNameW(hModule, wpath, kPathCap_ / 2);
            wchar_t* wslash = wcsrchr(wpath, L'\\');
            if (!wslash) wslash = wcsrchr(wpath, L'/');
            if (wslash) wcscpy(wslash + 1, name);
            else wcscpy(wpath, name);
            utf8_from_wide(wpath, out_utf8, kPathCap_);
        };
        set_dll_dir_file(L"H3RndNew.default.ini", g_ini_path);
        set_dll_dir_file(L"H3RndNew.user.ini", g_user_ini_path);
        g_disable_log = ReadDisableLogFromIniFiles();
        delete[] wpath;
        SetupDatedLogPathAndCleanup(hModule);

        // CrashGuard L1：无条件安装崩溃自记录（版本不对也要能记录崩溃）。
        // DisableLog 时 g_log_path_w 为空，防御日志随之关闭。
        GuardSetLogPathW(g_log_path_w);
        InstallCrashGuard();

        LogDebug("真随机 loading.");
        _P = GetPatcher();
        if (!_P) { LogError("GetPatcher failed."); return TRUE; }
        _PI = _P->CreateInstance("HD.Plugin.H3RndNew");
        if (!_PI) { LogError("CreateInstance failed."); return TRUE; }

        // CrashGuard L4 版本门卫：SoD 数据指纹不吻合（完整版/HotA/改版 exe）
        // 时不挂钩——RNG 入口偏移错配的代价比失去功能大得多。
        if (!GuardVerifySodBytes_()) {
            LogError("[Guard] 版本门卫不通过：已停用全部钩子（仅保留日志与崩溃自记录）。");
            return TRUE;
        }

        ReadConfig();
        StartPlugin();
    } else if (reason == DLL_PROCESS_DETACH) {
        // 收尾证据；缺失需结合进程状态/异常报告，不能单凭缺行归因崩溃。
        GuardShutdown();
    }
    return TRUE;
}
