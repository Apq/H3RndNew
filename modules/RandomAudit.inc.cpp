// 两个 RNG Hook 的入口/结果审计。锁内仅操作计数和快照，绝不调用 RNG/日志/原函数。
// 入口总数 = 真随机 + 原版路径 + 失败回退 + 无取数 + 异常退出 + 在途。
// 会话累计不因开局/开关切换清零；全局累计只纳入入口时 Full=1 的调用。
enum RandomAuditOutcome_ {
    kAuditTrue_, kAuditOriginal_, kAuditFallback_, kAuditNoDraw_,
    kAuditAborted_, kAuditOutcomeCount_
};
struct RandomAuditCounters_ {
    unsigned long long entered;
    unsigned long long pending;
    unsigned long long outcome[kAuditOutcomeCount_];
};
struct RandomAuditSnapshot_ {
    RandomAuditCounters_ session[2]; // 0=Rand，1=_rand。按入口独立统计，不相加当取数次数。
    RandomAuditCounters_ full[2];
    unsigned long long completed;
};
static CRITICAL_SECTION g_random_audit_lock_;
static RandomAuditSnapshot_ g_random_audit_ = {};
static DWORD g_random_audit_last_log_ = 0;
static DWORD g_random_audit_last_info_ = 0;
static const DWORD kAuditDebugInterval_ = 30000u;
static const DWORD kAuditInfoInterval_ = 300000u;

static void InitRandomAudit_()
{
    InitializeCriticalSection(&g_random_audit_lock_);
    g_random_audit_last_info_ = g_random_audit_last_log_ = GetTickCount();
}

static void LogRandomAudit_(const char* phase, bool heartbeat = false)
{
    RandomAuditSnapshot_ snapshot;
    EnterCriticalSection(&g_random_audit_lock_);
    snapshot = g_random_audit_;
    if (!heartbeat)
        g_random_audit_last_info_ = g_random_audit_last_log_ = GetTickCount();
    LeaveCriticalSection(&g_random_audit_lock_);

    bool full_has_data = g_true_random_full != 0;
    for (int entry = 0; entry < 2 && !full_has_data; ++entry) {
        const RandomAuditCounters_& c = snapshot.full[entry];
        full_has_data = c.entered != 0 || c.pending != 0;
        for (int i = 0; i < kAuditOutcomeCount_ && !full_has_data; ++i)
            full_has_data = c.outcome[i] != 0;
    }

    for (int entry = 0; entry < 2; ++entry) {
        for (int scope = 0; scope < (full_has_data ? 2 : 1); ++scope) {
            const RandomAuditCounters_& c = scope ? snapshot.full[entry] : snapshot.session[entry];
            unsigned long long accounted = c.pending;
            for (int i = 0; i < kAuditOutcomeCount_; ++i) accounted += c.outcome[i];
            const char* scope_name = scope ? "全局开启期间" : "会话累计";
            const char* entry_name = entry ? "_rand" : "Rand";
            auto log = c.entered != accounted ? &LogWarn : heartbeat ? &LogDebug : &LogInfo;
            log("真随机: RNG审计[%s][%s][%s] 总=%llu 真随机=%llu "
                "伪随机原版路径=%llu 失败回退=%llu 无取数=%llu 异常退出=%llu 在途=%llu 对账=%s",
                phase, scope_name, entry_name, c.entered, c.outcome[kAuditTrue_],
                c.outcome[kAuditOriginal_], c.outcome[kAuditFallback_],
                c.outcome[kAuditNoDraw_], c.outcome[kAuditAborted_], c.pending,
                c.entered == accounted ? "OK" : "不一致");
        }
    }
}

static void BeginRandomAudit_(int entry, bool full)
{
    EnterCriticalSection(&g_random_audit_lock_);
    ++g_random_audit_.session[entry].entered;
    ++g_random_audit_.session[entry].pending;
    if (full) {
        ++g_random_audit_.full[entry].entered;
        ++g_random_audit_.full[entry].pending;
    }
    LeaveCriticalSection(&g_random_audit_lock_);
}

static void EndRandomAudit_(int entry, bool full, RandomAuditOutcome_ outcome)
{
    EnterCriticalSection(&g_random_audit_lock_);
    const DWORD now = GetTickCount();
    --g_random_audit_.session[entry].pending;
    ++g_random_audit_.session[entry].outcome[outcome];
    unsigned long long full_outcome = 0;
    if (full) {
        --g_random_audit_.full[entry].pending;
        full_outcome = ++g_random_audit_.full[entry].outcome[outcome];
    }
    ++g_random_audit_.completed;
    const bool report_info = static_cast<DWORD>(now - g_random_audit_last_info_)
        >= kAuditInfoInterval_;
    const bool report = report_info || static_cast<DWORD>(now - g_random_audit_last_log_)
        >= kAuditDebugInterval_;
    if (report) g_random_audit_last_log_ = now;
    if (report_info) g_random_audit_last_info_ = now;
    LeaveCriticalSection(&g_random_audit_lock_);
    if (full_outcome == 1 && (outcome == kAuditOriginal_ || outcome == kAuditFallback_
        || outcome == kAuditAborted_)) {
        LogWarn("真随机: RNG审计全局异常路径入口=%s 类别=%s（首例，详见汇总）",
            entry ? "_rand" : "Rand", outcome == kAuditOriginal_ ? "原版放行"
            : outcome == kAuditFallback_ ? "系统随机失败回退" : "异常退出");
    }
    if (report) LogRandomAudit_("调用心跳", !report_info);
}
