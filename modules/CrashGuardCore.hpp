// CrashGuardCore.hpp - 崩溃防御的纯逻辑部分（无 Windows 依赖）。
// 崩溃钩子、钩子铠甲与版本门卫的本体在 modules/CrashGuard.inc.cpp
// （依赖 ConfigLog 的落盘函数），此处只放可被 PolicyCoreTests 直接
// 覆盖的判定与数据结构。
#pragma once

namespace H3AutoGuard {

// 异常聚合限流参数：同一钩子首次异常记完整详情，之后每 kFaultLogEvery
// 次记一行计数汇总。钩子永远保持可用（异常每次都被吞掉并走安全默认，
// 下次调用继续尝试——熔断会让功能静默失效，不做）。
enum { kRingCap = 8, kFaultLogEvery = 100 };

// ---- 异常码分类与命名 ----

// 不进首次机会历史环的异常码：这些是进程正常运行的组成部分，
// 记录会刷爆环形缓冲并干扰判读。
inline bool IsNoiseExceptionCode(unsigned long code)
{
    switch (code) {
    case 0xE06D7363: return true;  // C++ throw（msc）
    case 0x40010006: return true;  // OutputDebugStringA
    case 0x4001000A: return true;  // OutputDebugStringW
    case 0x406D1388: return true;  // SetThreadName（VS 线程命名）
    case 0x80000004: return true;  // 单步（调试器专用）
    default:         return false;
    }
}

// 常见严重异常码的中文名；未知返回 nullptr（调用方打原始码）。
inline const char* ExceptionCodeName(unsigned long code)
{
    switch (code) {
    case 0xC0000005: return "访问冲突";
    case 0xC000001D: return "非法指令";
    case 0xC0000025: return "不可继续的异常";
    case 0xC000008C: return "数组越界";
    case 0xC0000094: return "整数除零";
    case 0xC0000095: return "整数溢出";
    case 0xC00000FD: return "栈溢出";
    case 0xC0000374: return "堆损坏";
    case 0xC0000409: return "快速失败(栈保护)";
    case 0x80000003: return "断点";
    case 0xC0000135: return "DLL 缺失";
    case 0xC0000139: return "入口点缺失";
    default:         return nullptr;
    }
}

// 访问冲突的操作类型（ExceptionInformation[0]）。
inline const char* AVOperationName(unsigned long op)
{
    switch (op) {
    case 0: return "读取";
    case 1: return "写入";
    case 8: return "执行(DEP)";
    default: return "访问";
    }
}

// ---- 首次机会异常历史环 ----
// VEH 每次过滤后的严重异常推入一条；崩溃报告时按最旧→最新输出，
// 用于还原崩溃前的异常序列（如同一地址反复读写冲突）。
// 线程安全由 .inc.cpp 侧的自旋锁保证；本结构保持纯净可测。
struct RingRecord {
    unsigned long   tick_ms;  // GetTickCount 快照
    unsigned long   tid;      // 线程 id
    unsigned long   code;     // 异常码
    unsigned long long addr;  // 出错地址
    unsigned long long info0; // AV: 操作类型
    unsigned long long info1; // AV: 目标地址
};

struct Ring {
    RingRecord rec[kRingCap];
    int        next;   // 总推送次数（游标，%kRingCap 回绕）

    Ring() : next(0) {}

    void Push(const RingRecord& r)
    {
        rec[next % kRingCap] = r;
        ++next;
    }

    int Count() const { return next < kRingCap ? next : kRingCap; }

    // idx: 0=最旧 … Count()-1=最新。
    bool Get(int idx, RingRecord* out) const
    {
        const int n = Count();
        if (!out || idx < 0 || idx >= n) return false;
        const int start = (next > kRingCap) ? (next % kRingCap) : 0;
        *out = rec[(start + idx) % kRingCap];
        return true;
    }
};

// ---- SoD 数据指纹（版本门卫）----
// 力场（大力神盾）障碍信息表是 SoD 特有的静态数据（实测：
//   0x63CF18: +0x02 WORD numSquares=2, +0x04 起 signed char 格 {0,-16}，
//             +0x10 char* -> "C15spE1.def"；
//   0x63CF2C: numSquares=3, 格 {0,-16,-34}, -> "C15spE10.def"）。
// 完整版/HotA/改版 exe 不可能同时吻合全部特征值。
// def2/def3 为运行时从表内指针读出的字符串（可为 nullptr，读取方负责
// 保证解引用安全）。
inline bool ForceFieldTableLooksLikeSod(
    int n2, int c2a, int c2b,
    int n3, int c3a, int c3b, int c3c,
    const char* def2, const char* def3)
{
    static const char kPrefix[] = "C15sp";
    if (n2 != 2 || n3 != 3) return false;
    if (c2a != 0 || c2b != -16) return false;
    if (c3a != 0 || c3b != -16 || c3c != -34) return false;
    if (!def2 || !def3) return false;
    for (int i = 0; i < 5; ++i) {
        if (def2[i] != kPrefix[i] || def3[i] != kPrefix[i]) return false;
    }
    return true;
}

} // namespace H3AutoGuard
