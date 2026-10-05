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
    LogSelfVersion_();
    LogInfo("真随机: plugin enabled. TrueRandom=%d（0=伪随机 1=真随机）",
        g_true_random);
    InstallTrueRandomHooks_();
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
        LogInfo("真随机 loading.");
        _P = GetPatcher();
        if (!_P) { LogError("GetPatcher failed."); return TRUE; }
        _PI = _P->CreateInstance("HD.Plugin.H3RndNew");
        if (!_PI) { LogError("CreateInstance failed."); return TRUE; }
        ReadConfig();
        StartPlugin();
    }
    return TRUE;
}
