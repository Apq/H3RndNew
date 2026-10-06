// CrashGuard.hpp - 崩溃防御（通用自包含版，无项目依赖）
// ------------------------------------------------------------------------
// 设计与完整流程见技能 h3-plugin-crash-guard / H3Auto 设计文档 §18。
// 分层：
//   L1 崩溃自记录：VEH 首次机会环（8 条，噪音过滤）+ 未处理异常报告
//      （异常码中文名 / AV 读写/DEP / 出错模块+偏移 / 历史 / 栈回溯 /
//      各钩子累计异常），链回前一过滤器，不改默认崩溃行为。
//   L2 钩子铠甲：钩子入口 __try/__except 吞异常并落盘，走安全默认。
//   L3 异常聚合：同一钩子首异常记详情、之后每 100 次记一行计数；
//      钩子永远保持可用（不做熔断——那会让功能静默失效）。
//   L4 版本门卫：挂钩前校验 SoD 数据指纹（力场表 0x63CF18/0x63CF2C，
//      字节级取证见 H3Note\BattleCrashFix逆向笔记.md），不吻合不挂钩。
//
// 用法（单翻译单元；在日志路径全局变量定义之后 #include 本文件）：
//   1) 初始化取得日志路径后：GuardSetLogPathW(项目日志路径或 nullptr=关);
//   2) InstallCrashGuard();            // VEH + UEF，无条件安装
//   3) 挂钩前：if (!GuardVerifySodBytes_()) { 记录并跳过全部钩子注册; }
//   4) 每个钩子入口用下面的铠甲模式；
//   5) DLL_PROCESS_DETACH：GuardShutdown();  // 写收尾行（判读生死标记）
//
// 铠甲模式：
//   功能型插件（异常后放行原版，绝不吞游戏自身的崩溃）：
//     LoHook:  __try { ...自有逻辑... }
//              __except (GuardCrashFilter_(GUARD_XX, GetExceptionInformation())) {}
//              return EXEC_DEFAULT;
//     HiHook:  原函数调用放 __try 之外（开头直调或结尾兜底一次），自有
//              逻辑段单独 __try；异常返回值按钩子语义选安全值。
//     回调:    __try { ... } __except (同上) {} return CallNextHookEx(...);
//   防崩型插件（原函数路径本身有 bug，如 BattleCrashFix）：
//     安全默认 = 维持跳过原函数（return 0）；原函数调用可留 __try 内，
//     异常吞掉即等于跳过，正是修复语义。
//
// 崩溃上下文安全约束：处理器内只用静态/栈缓冲 + 内核文件 API
// （CreateFileW/WriteFile），不碰堆、不拿内核锁；自旋锁（放弃阈值）；
// 全程 __try 自包裹 + 重入标志；栈溢出跳过栈回溯。捕获不到的：
// TerminateProcess 强杀、__fastfail、栈溢出到无栈可用（转 Windows
// 事件查看器 1000 / %LOCALAPPDATA%\CrashDumps）。
// ========================================================================
#pragma once

#include "CrashGuardCore.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <windows.h>

using namespace H3AutoGuard;

// ---- 钩子注册表（DllMain 单线程注册，无并发）----
enum { kMaxGuardHooks = 16 };

struct GuardHookInfo_ {
    const char*    name;
    volatile LONG  faults;
};
static GuardHookInfo_ s_guard_hooks[kMaxGuardHooks];
static int            s_guard_hook_count = 0;

// 返回钩子 id（供 GuardCrashFilter_ 聚合）。超过上限时并入 0 号。
static int GuardRegisterHook_(const char* name)
{
    if (s_guard_hook_count <= 0) {
        s_guard_hooks[0].name = name;
        s_guard_hooks[0].faults = 0;
        s_guard_hook_count = 1;
        return 0;
    }
    for (int i = 0; i < s_guard_hook_count; ++i)
        if (s_guard_hooks[i].name == name) return i;
    if (s_guard_hook_count >= kMaxGuardHooks) return 0;
    s_guard_hooks[s_guard_hook_count].name = name;
    s_guard_hooks[s_guard_hook_count].faults = 0;
    return s_guard_hook_count++;
}

static const char* GuardHookName_(int id)
{
    if (id < 0 || id >= s_guard_hook_count || !s_guard_hooks[id].name)
        return "?";
    return s_guard_hooks[id].name;
}

// ---- 状态（全部静态，无堆）----
static Ring               s_ring;
static volatile LONG      s_ring_lock = 0;   // 自旋锁（崩溃上下文禁内核锁）
static volatile LONG      s_veh_seen = 0;    // 过滤后的首次机会异常总数
static volatile LONG      s_uef_busy = 0;
static void*              s_veh_handle = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_uef = nullptr;
static wchar_t            s_guard_log_path[MAX_PATH * 2];

// ---- 落盘器（崩溃安全：CreateFileW append，逐行即开即关）----
// 日志路径由项目喂入（通常与业务日志同文件）；空路径 = 玩家关闭日志。
static void GuardSetLogPathW(const wchar_t* path)
{
    s_guard_log_path[0] = 0;
    if (path && path[0])
        lstrcpynW(s_guard_log_path, path, (int)(sizeof(s_guard_log_path) / 2));
}

static void GuardWriteLine_(const char* utf8_line)
{
    if (!s_guard_log_path[0]) return;
    HANDLE f = CreateFileW(s_guard_log_path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[48];
    const int hl = _snprintf_s(head, sizeof(head), _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [Guard] ",
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
        (unsigned)st.wMilliseconds);
    DWORD wr = 0;
    if (hl > 0) WriteFile(f, head, (DWORD)hl, &wr, nullptr);
    const DWORD ll = (DWORD)strlen(utf8_line);
    if (ll) WriteFile(f, utf8_line, ll, &wr, nullptr);
    WriteFile(f, "\r\n", 2, &wr, nullptr);
    CloseHandle(f);
}

static void GuardLog_(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    GuardWriteLine_(buf);
}

// ---- 模块解析（崩溃安全：仅 kernel32 查询 + 静态缓冲）----
static bool GuardResolveModule_(unsigned long long addr, char* name_utf8,
    int cap, unsigned long long* base, unsigned long long* offset)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(addr)), &m)
        || !m)
        return false;
    wchar_t wpath[MAX_PATH];
    wpath[0] = 0;
    GetModuleFileNameW(m, wpath, MAX_PATH);
    const wchar_t* base_name = wpath;
    const wchar_t* s1 = wcsrchr(wpath, L'\\');
    const wchar_t* s2 = wcsrchr(wpath, L'/');
    if (s1 && s1 + 1 > base_name) base_name = s1 + 1;
    if (s2 && s2 + 1 > base_name) base_name = s2 + 1;
    if (cap > 0) name_utf8[0] = 0;
    WideCharToMultiByte(CP_UTF8, 0, base_name, -1, name_utf8, cap,
        nullptr, nullptr);
    if (cap > 0) name_utf8[cap - 1] = 0;
    *base = static_cast<unsigned long long>(
        reinterpret_cast<uintptr_t>(m));
    *offset = addr - *base;
    return true;
}

// 把"code+addr(+AV 细节)+模块"落盘成一行。崩溃上下文可直接调用。
static void GuardLogOne_(const char* tag, unsigned long code,
    unsigned long long addr, const EXCEPTION_RECORD* er)
{
    char mod[96];
    unsigned long long base = 0, off = 0;
    const bool has_mod = GuardResolveModule_(addr, mod, (int)sizeof(mod),
        &base, &off);
    const char* name = ExceptionCodeName(code);
    if (code == 0xC0000005 && er && er->NumberParameters >= 2) {
        GuardLog_("%s 异常 0x%08lX(%s: %s 0x%08llX) addr=0x%08llX%s%s+0x%llX",
            tag, code, name ? name : "未分类",
            AVOperationName(static_cast<unsigned long>(er->ExceptionInformation[0])),
            static_cast<unsigned long long>(er->ExceptionInformation[1]),
            addr, has_mod ? " " : "", has_mod ? mod : "模块未知",
            has_mod ? off : 0ull);
    } else {
        GuardLog_("%s 异常 0x%08lX(%s) addr=0x%08llX%s%s+0x%llX",
            tag, code, name ? name : "未分类", addr,
            has_mod ? " " : "", has_mod ? mod : "模块未知",
            has_mod ? off : 0ull);
    }
}

// ---- 钩子铠甲过滤器（__except 里调用；返回 EXECUTE_HANDLER 吞掉）----
// 首异常记详情、之后每 100 次记计数；钩子保持可用。
static int GuardCrashFilter_(int hook_id, EXCEPTION_POINTERS* ep)
{
    __try {
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_EXECUTE_HANDLER;
        const unsigned long code = ep->ExceptionRecord->ExceptionCode;
        const LONG n = InterlockedIncrement(&s_guard_hooks[hook_id].faults);
        if (n == 1) {
            GuardLogOne_(GuardHookName_(hook_id), code,
                reinterpret_cast<unsigned long long>(
                    ep->ExceptionRecord->ExceptionAddress),
                ep->ExceptionRecord);
        } else if (n % kFaultLogEvery == 0) {
            GuardLog_("钩子 %s 累计异常 %ld 次（每次已吞掉并走安全默认，钩子保持可用）",
                GuardHookName_(hook_id), n);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- VEH：首次机会异常进环（仅记录，绝不改分发结果）----
static LONG WINAPI GuardVeh_(PEXCEPTION_POINTERS ep)
{
    __try {
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        const unsigned long code = ep->ExceptionRecord->ExceptionCode;
        if (IsNoiseExceptionCode(code)) return EXCEPTION_CONTINUE_SEARCH;
        InterlockedIncrement(&s_veh_seen);
        int spins = 0;
        while (InterlockedCompareExchange(&s_ring_lock, 1, 0) != 0) {
            if (++spins > 4096) return EXCEPTION_CONTINUE_SEARCH; // 放弃记录
            YieldProcessor();
        }
        RingRecord r = {};
        r.tick_ms = GetTickCount();
        r.tid = GetCurrentThreadId();
        r.code = code;
        r.addr = reinterpret_cast<unsigned long long>(
            ep->ExceptionRecord->ExceptionAddress);
        r.info0 = ep->ExceptionRecord->NumberParameters >= 1
            ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
        r.info1 = ep->ExceptionRecord->NumberParameters >= 2
            ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        s_ring.Push(r);
        InterlockedExchange(&s_ring_lock, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- UEF：未处理异常全量报告（进程终止前最后的落盘机会）----
static void GuardLogFrame_(unsigned long long addr)
{
    char mod[96];
    unsigned long long base = 0, off = 0;
    if (GuardResolveModule_(addr, mod, (int)sizeof(mod), &base, &off))
        GuardLog_("  栈 %s+0x%llX", mod, off);
}

static void GuardWriteFatalReport_(PEXCEPTION_POINTERS ep)
{
    GuardLog_("====== 未处理异常：进程即将终止（崩溃自记录；随后交回系统处理，行为不变）======");
    if (!ep || !ep->ExceptionRecord) {
        GuardLog_("无异常记录指针（ep=NULL），报告到此为止");
        return;
    }
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const unsigned long code = er->ExceptionCode;
    GuardLogOne_("致命", code,
        reinterpret_cast<unsigned long long>(er->ExceptionAddress), er);
    GuardLog_("线程 %lu | 本次之前首次机会异常 %ld 条（缓存最近 %d 条）",
        GetCurrentThreadId(), s_veh_seen, s_ring.Count());

    RingRecord r = {};
    for (int i = 0; s_ring.Get(i, &r); ++i) {
        const char* name = ExceptionCodeName(r.code);
        char mod[96];
        unsigned long long base = 0, off = 0;
        const bool has_mod = GuardResolveModule_(r.addr, mod,
            (int)sizeof(mod), &base, &off);
        if (r.code == 0xC0000005)
            GuardLog_("  历史[%d] +%lums 0x%08lX(访问冲突: %s 0x%08llX) %s+0x%llX",
                i, r.tick_ms, r.code,
                AVOperationName(static_cast<unsigned long>(r.info0)), r.info1,
                has_mod ? mod : "模块未知", has_mod ? off : 0ull);
        else
            GuardLog_("  历史[%d] +%lums 0x%08lX(%s) %s+0x%llX",
                i, r.tick_ms, r.code, name ? name : "未分类",
                has_mod ? mod : "模块未知", has_mod ? off : 0ull);
    }

    if (code == 0xC00000FD) {
        GuardLog_("栈溢出：跳过调用栈回溯（避免再次耗尽栈）");
    } else {
        void* frames[12] = {};
        const WORD n = CaptureStackBackTrace(2, 12, frames, nullptr);
        if (n == 0)
            GuardLog_("调用栈回溯失败（FPO/栈损坏，尽力而为）");
        else
            for (WORD i = 0; i < n; ++i)
                GuardLogFrame_(reinterpret_cast<unsigned long long>(frames[i]));
    }

    // 各钩子累计异常汇总（归因参考：崩前哪些路径已多次吞异常）。
    for (int id = 0; id < s_guard_hook_count; ++id) {
        if (s_guard_hooks[id].faults > 0)
            GuardLog_("钩子 %s 本次会话累计异常 %ld 次",
                GuardHookName_(id), s_guard_hooks[id].faults);
    }
    GuardLog_("====== 报告结束 ======");
}

static LONG WINAPI GuardUef_(PEXCEPTION_POINTERS ep)
{
    // 重入（报告过程自身再崩）时不再写，直接放行，避免递归刷日志。
    if (InterlockedCompareExchange(&s_uef_busy, 1, 0) == 0) {
        __try {
            GuardWriteFatalReport_(ep);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        InterlockedExchange(&s_uef_busy, 0);
    }
    // 链回前一个过滤器（可能是 HD Mod 的崩溃窗）；没有则交回 WER。
    // 返回值透传，绝不改变默认崩溃行为。
    if (s_prev_uef) return s_prev_uef(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- 版本门卫：SoD 数据指纹（挂钩前调用）----
// 校验值来源：本机 SoD exe 文件偏移 0x23CF18/0x23CF2C 实测
// （0x63CF1A=WORD 2、0x63CF1C={0,-16}、0x63CF2E=WORD 3、
//  0x63CF30={0,-16,-34}、表内 +0x10 指针 -> "C15spE1.def"/"C15spE10.def"）。
static bool GuardVerifySodBytes_()
{
    bool ok = false;
    __try {
        const WORD n2 = *reinterpret_cast<const WORD*>(0x63CF18 + 0x06);
        const signed char c2a = *reinterpret_cast<const signed char*>(0x63CF18 + 0x08);
        const signed char c2b = *reinterpret_cast<const signed char*>(0x63CF18 + 0x09);
        const WORD n3 = *reinterpret_cast<const WORD*>(0x63CF2C + 0x06);
        const signed char c3a = *reinterpret_cast<const signed char*>(0x63CF2C + 0x08);
        const signed char c3b = *reinterpret_cast<const signed char*>(0x63CF2C + 0x09);
        const signed char c3c = *reinterpret_cast<const signed char*>(0x63CF2C + 0x0A);
        const char* def2 = reinterpret_cast<const char*>(
            *reinterpret_cast<const uintptr_t*>(0x63CF18 + 0x10));
        const char* def3 = reinterpret_cast<const char*>(
            *reinterpret_cast<const uintptr_t*>(0x63CF2C + 0x10));
        ok = ForceFieldTableLooksLikeSod(n2, c2a, c2b, n3, c3a, c3b, c3c,
            def2, def3);
        if (!ok) {
            GuardLog_("SoD 数据指纹不匹配：0x63CF18 count=%d 格={%d,%d} / 0x63CF2C count=%d 格={%d,%d,%d} def=\"%s\"/\"%s\" —— 疑似非 SoD 或改版 exe",
                (int)n2, (int)c2a, (int)c2b, (int)n3,
                (int)c3a, (int)c3b, (int)c3c,
                def2 ? def2 : "(空)", def3 ? def3 : "(空)");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GuardLog_("版本校验读取异常 code=0x%08lX，保守判定失败",
            GetExceptionCode());
        ok = false;
    }
    return ok;
}

// ---- 安装/收尾 ----
// 无条件调用（版本不对/不挂钩也要能记录崩溃）。需先 GuardSetLogPathW。
static void InstallCrashGuard()
{
    s_veh_handle = AddVectoredExceptionHandler(1, GuardVeh_);
    if (s_veh_handle)
        GuardLog_("崩溃自记录已启用：VEH 首次机会环 + 未处理异常报告 + 钩子铠甲（异常聚合，钩子保持可用）");
    else
        GuardLog_("VEH 安装失败，仅保留未处理异常报告与钩子铠甲");
    s_prev_uef = SetUnhandledExceptionFilter(GuardUef_);
}

// DLL_PROCESS_DETACH 调用：写收尾行（判读日志生死标记——末尾有此行
// = 正常退出；没有 = 异常终止），并卸 VEH。
static void GuardShutdown()
{
    if (s_veh_handle) {
        RemoveVectoredExceptionHandler(s_veh_handle);
        s_veh_handle = nullptr;
    }
    GuardLog_("进程退出收尾（DLL_PROCESS_DETACH）");
}
