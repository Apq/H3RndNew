// ========== 配置与日志 ==========
// 两层 ini：H3RndNew.default.ini 出厂默认，H3RndNew.user.ini 玩家覆盖。
// 日志按日期写在 DLL 同目录，保留最近 30 个。

static char* g_ini_path = new char[kPathCap_];        // H3RndNew.default.ini
static char* g_user_ini_path = new char[kPathCap_](); // H3RndNew.user.ini（可不存在）
static char* g_log_path = new char[kPathCap_];
static wchar_t* g_log_path_w = new wchar_t[kPathCap_ / 2];

// 创建新游戏建档时的随机源：0=伪随机（原版），1=真随机。
static int g_true_random = 0;

// 分层读取：先默认层，再叠加玩家层（键存在且非空才覆盖）。两层都未命中 → fallback。
static bool IniReadUtf8Layered(const char* section, const char* key,
    const char* fallback, char* out, int out_size)
{
    const bool hit_default =
        IniReadUtf8(g_ini_path, section, key, fallback, out, out_size);
    char user_v[512] = {};
    if (IniReadUtf8(g_user_ini_path, section, key, "", user_v,
            (int)sizeof(user_v))
        && user_v[0]) {
        const int n = (int)strlen(user_v) < out_size - 1
            ? (int)strlen(user_v) : out_size - 1;
        memcpy(out, user_v, n);
        out[n] = 0;
        return true;
    }
    return hit_default;
}

static int IniReadIntUtf8Layered(const char* section, const char* key,
    int fallback)
{
    char buf[32] = {};
    IniReadUtf8Layered(section, key, "", buf, (int)sizeof(buf));
    return buf[0] ? atoi(buf) : fallback;
}

static HMODULE g_hModule = nullptr;
static bool g_disable_log = false;

static const int MAX_LOG_FILES_TO_KEEP = 30;
static const int MAX_LOG_FILES_TO_SCAN = 1024;

struct LogFileEntryW {
    wchar_t path[MAX_PATH * 2];
    FILETIME last_write;
};

static int __cdecl CompareLogFileEntryW(const void* a, const void* b)
{
    const LogFileEntryW* la = (const LogFileEntryW*)a;
    const LogFileEntryW* lb = (const LogFileEntryW*)b;
    int cmp = CompareFileTime(&la->last_write, &lb->last_write);
    if (cmp != 0) return cmp;
    return _wcsicmp(la->path, lb->path);
}

static bool ReadDisableLogFromIniFiles()
{
    return IniReadIntUtf8Layered("Logging", "DisableLog", 0) != 0;
}

static void CleanupOldLogFilesW(const wchar_t* log_dir, const wchar_t* log_base, const wchar_t* current_log_path)
{
    if (!log_dir || !log_dir[0] || !log_base || !log_base[0]) return;
    wchar_t pattern[MAX_PATH * 2];
    _snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%s\\%s_*.log", log_dir, log_base);

    LogFileEntryW* entries = (LogFileEntryW*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_LOG_FILES_TO_SCAN * sizeof(LogFileEntryW));
    if (!entries) return;
    int count = 0;
    bool current_found = false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { HeapFree(GetProcessHeap(), 0, entries); return; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (count >= MAX_LOG_FILES_TO_SCAN) break;
        _snwprintf_s(entries[count].path, _countof(entries[count].path), _TRUNCATE, L"%s\\%s", log_dir, fd.cFileName);
        entries[count].last_write = fd.ftLastWriteTime;
        if (current_log_path && _wcsicmp(entries[count].path, current_log_path) == 0) current_found = true;
        ++count;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    int keep_existing = current_found ? MAX_LOG_FILES_TO_KEEP : (MAX_LOG_FILES_TO_KEEP - 1);
    if (keep_existing < 0) keep_existing = 0;
    if (count <= keep_existing) { HeapFree(GetProcessHeap(), 0, entries); return; }

    qsort(entries, count, sizeof(entries[0]), CompareLogFileEntryW);
    int delete_count = count - keep_existing;
    for (int i = 0; i < delete_count; ++i) DeleteFileW(entries[i].path);
    HeapFree(GetProcessHeap(), 0, entries);
}

static void SetupDatedLogPathAndCleanup(HMODULE hModule)
{
    if (g_disable_log) {
        g_log_path[0] = 0;
        g_log_path_w[0] = 0;
        return;
    }

    const int cap = kPathCap_ / 2;
    wchar_t* module_path = new wchar_t[cap]();
    wchar_t* dir = new wchar_t[cap]();
    wchar_t* base = new wchar_t[cap]();
    GetModuleFileNameW(hModule, module_path, cap);

    const wchar_t* slash1 = wcsrchr(module_path, L'\\');
    const wchar_t* slash2 = wcsrchr(module_path, L'/');
    const wchar_t* slash = slash1 > slash2 ? slash1 : slash2;
    const wchar_t* name = slash ? slash + 1 : module_path;
    if (slash) {
        int len = (int)(slash - module_path);
        if (len >= cap) len = cap - 1;
        memcpy(dir, module_path, len * sizeof(wchar_t));
        dir[len] = 0;
    } else {
        wcscpy_s(dir, cap, L".");
    }
    wcsncpy_s(base, cap, name, _TRUNCATE);
    wchar_t* dot = wcsrchr(base, L'.');
    if (dot) *dot = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(g_log_path_w, kPathCap_ / 2, _TRUNCATE,
        L"%s\\%s_%04u%02u%02u_%02u%02u%02u.log",
        dir, base, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    WideCharToMultiByte(CP_UTF8, 0, g_log_path_w, -1, g_log_path, kPathCap_, nullptr, nullptr);
    CleanupOldLogFilesW(dir, base, g_log_path_w);
    delete[] module_path; delete[] dir; delete[] base;

    HANDLE hf = CreateFileW(g_log_path_w, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER fpos; fpos.QuadPart = 0;
        if (SetFilePointerEx(hf, fpos, &fpos, FILE_END) && fpos.QuadPart == 0) {
            DWORD wr; WriteFile(hf, "\xEF\xBB\xBF", 3, &wr, nullptr);
        }
        SYSTEMTIME st2; GetLocalTime(&st2);
        char line[128];
        int n = _snprintf(line, sizeof(line)-1, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] 日志初始化完成。\r\n",
            st2.wYear, st2.wMonth, st2.wDay, st2.wHour, st2.wMinute, st2.wSecond, st2.wMilliseconds);
        if (n > 0) { DWORD wr; WriteFile(hf, line, (DWORD)n, &wr, nullptr); }
        CloseHandle(hf);
    }
}

// 日志级别（常见五级）。MinLevel 以下不落盘；DisableLog 仍最高优先。
enum LogLevel {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO  = 2,
    LOG_WARN  = 3,
    LOG_ERROR = 4,
};
static int g_log_level = LOG_INFO;

static const char* LogLevelName_(int level)
{
    switch (level) {
    case LOG_TRACE: return "trace";
    case LOG_DEBUG: return "debug";
    case LOG_INFO:  return "info";
    case LOG_WARN:  return "warn";
    default:        return "error";
    }
}

static int ParseLogLevel_(const char* name)
{
    if (!name || !name[0]) return LOG_INFO;
    if (_stricmp(name, "trace") == 0) return LOG_TRACE;
    if (_stricmp(name, "debug") == 0) return LOG_DEBUG;
    if (_stricmp(name, "info") == 0) return LOG_INFO;
    if (_stricmp(name, "warn") == 0 || _stricmp(name, "warning") == 0) return LOG_WARN;
    if (_stricmp(name, "error") == 0) return LOG_ERROR;
    return LOG_INFO;
}

static void LogTrace(const char* fmt, ...);
static void LogDebug(const char* fmt, ...);
static void LogInfo(const char* fmt, ...);
static void LogWarn(const char* fmt, ...);
static void LogError(const char* fmt, ...);

static int ClampInt(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static void ReadConfig()
{
    g_true_random = ClampInt(
        IniReadIntUtf8Layered("General", "TrueRandom", 0), 0, 1);

    char lv[16] = {};
    IniReadUtf8Layered("Logging", "MinLevel", "info", lv, sizeof(lv));
    g_log_level = ParseLogLevel_(lv);
    LogInfo("配置加载：TrueRandom=%d MinLevel=%s",
        g_true_random, LogLevelName_(g_log_level));
}

static void AppendUtf8LogLine(const char* text)
{
    if (g_disable_log) return;
    if (!g_log_path_w[0]) return;
    HANDLE h = CreateFileW(g_log_path_w, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER pos;
    pos.QuadPart = 0;
    if (SetFilePointerEx(h, pos, &pos, FILE_END) && pos.QuadPart == 0) {
        DWORD written = 0;
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        WriteFile(h, bom, 3, &written, nullptr);
    }
    DWORD written = 0;
    WriteFile(h, text, (DWORD)strlen(text), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

static void WriteLogLv(int level, const char* fmt, ...)
{
    if (g_disable_log) return;
    if (level < g_log_level) return;
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int off = _snprintf(line, sizeof(line) - 1, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        LogLevelName_(level));
    if (off < 0) off = 0;
    if (off >= (int)sizeof(line)) off = (int)sizeof(line) - 1;

    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line + off, sizeof(line) - off - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    AppendUtf8LogLine(line);
}

#define H3RNDNEW_LOG_BODY_(level)                                        \
    do {                                                                 \
        if (g_disable_log || (level) < g_log_level) break;               \
        char line[1024];                                                 \
        va_list ap; va_start(ap, fmt);                                   \
        _vsnprintf(line, sizeof(line) - 1, fmt, ap);                     \
        va_end(ap);                                                      \
        line[sizeof(line) - 1] = 0;                                      \
        WriteLogLv(level, "%s", line);                                   \
    } while (0)

static void LogTrace(const char* fmt, ...) { H3RNDNEW_LOG_BODY_(LOG_TRACE); }
static void LogDebug(const char* fmt, ...) { H3RNDNEW_LOG_BODY_(LOG_DEBUG); }
static void LogInfo(const char* fmt, ...)  { H3RNDNEW_LOG_BODY_(LOG_INFO); }
static void LogWarn(const char* fmt, ...)  { H3RNDNEW_LOG_BODY_(LOG_WARN); }
static void LogError(const char* fmt, ...) { H3RNDNEW_LOG_BODY_(LOG_ERROR); }
