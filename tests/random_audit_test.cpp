// Standalone Windows test of the production audit counters (does not start Heroes3).
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>
#include <atomic>
static std::atomic<int> bad_log{0};
static std::atomic<int> audit_lines{0};
static std::atomic<int> warning_lines{0};
static void LogInfo(const char* format, ...)
{
    char text[2048];
    va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args);
    if (strstr(text, "RNG")) {
        ++audit_lines;
        if (!strstr(text, "=OK")) ++bad_log;
    }
}
static void LogWarn(const char*, ...) { ++warning_lines; }
#include "../modules/RandomAudit.inc.cpp"
#include "../modules/ScenarioContext.hpp"
static void require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void verify(const RandomAuditCounters_& c)
{
    unsigned long long sum = c.pending;
    for (int i = 0; i < kAuditOutcomeCount_; ++i) sum += c.outcome[i];
    require(c.entered == sum, "counter equation");
}
int main()
{
    // Window authorization is independent of mouse/key messages and elapsed time.
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "unknown denies window");
    ObserveScenarioContext_(ClassifyScenarioContext_(false, true));
    require(ConsumeScenarioContext_() == kScenarioLoad_, "load before battle");
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "context consumed once");
    ObserveScenarioContext_(ClassifyScenarioContext_(false, true));
    ObserveScenarioContext_(ClassifyScenarioContext_(false, false));
    require(AllowsNewGameWindow_(ConsumeScenarioContext_()), "load cancelled then single scenario");
    ObserveScenarioContext_(ClassifyScenarioContext_(false, true));
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "new game then load");
    ObserveScenarioContext_(ClassifyScenarioContext_(true, false));
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "campaign denies window");
    // A message-pre snapshot also works when a constructor hook was bypassed.
    ObserveScenarioContext_(ClassifyScenarioContext_(false, true));
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "load keyboard confirm");
    ObserveScenarioContext_(ClassifyScenarioContext_(false, false));
    require(AllowsNewGameWindow_(ConsumeScenarioContext_()), "new game keyboard confirm");
    require(ClassifyScenarioContext_(true, true) == kScenarioLoad_, "load takes priority");
    InitRandomAudit_();
    LogRandomAudit_("startup");
    require(audit_lines == 4, "initial four scope/entry lines");
    // Nested calls and outstanding entries remain accounted for.
    BeginRandomAudit_(0, true);
    BeginRandomAudit_(1, true);
    LogRandomAudit_("nested pending");
    EndRandomAudit_(1, true, kAuditTrue_);
    EndRandomAudit_(0, true, kAuditOriginal_);
    // Every outcome, both scopes and both entries.
    for (int e = 0; e < 2; ++e) {
        for (int o = 0; o < kAuditOutcomeCount_; ++o) {
            BeginRandomAudit_(e, false);
            EndRandomAudit_(e, false, static_cast<RandomAuditOutcome_>(o));
            BeginRandomAudit_(e, true);
            EndRandomAudit_(e, true, static_cast<RandomAuditOutcome_>(o));
        }
    }
    require(g_random_audit_.session[0].entered == 11, "session totals");
    require(g_random_audit_.full[0].entered == 6, "Full=false excluded");
    // No replacement is required to trigger a heartbeat.
    const int before = audit_lines;
    EnterCriticalSection(&g_random_audit_lock_);
    g_random_audit_last_log_ = GetTickCount() - 5001u;
    LeaveCriticalSection(&g_random_audit_lock_);
    BeginRandomAudit_(1, false); EndRandomAudit_(1, false, kAuditOriginal_);
    require(audit_lines == before + 4, "original-path time heartbeat");
    // Parallel mutation plus snapshots: every observed equation must hold.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([t]() {
        for (int i = 0; i < 20000; ++i) {
            const bool full = i % 2 == 0;
            BeginRandomAudit_(t % 2, full);
            if (i % 997 == 0) LogRandomAudit_("concurrent pending");
            EndRandomAudit_(t % 2, full, static_cast<RandomAuditOutcome_>(i % kAuditOutcomeCount_));
        }
    });
    for (auto& t : threads) t.join();
    for (int e = 0; e < 2; ++e) {
        verify(g_random_audit_.session[e]); verify(g_random_audit_.full[e]);
        require(g_random_audit_.session[e].pending == 0, "no stranded pending");
        require(g_random_audit_.session[e].entered == 40011u + (e == 1 ? 1u : 0u), "parallel exact session total");
        require(g_random_audit_.full[e].entered == 20006u, "parallel exact full total");
    }
    LogRandomAudit_("final");
    require(bad_log == 0, "all formatted snapshots reconcile");
    // ULL counters work beyond signed 32-bit range.
    EnterCriticalSection(&g_random_audit_lock_);
    g_random_audit_.session[0].entered += 0x100000000ULL;
    g_random_audit_.session[0].outcome[kAuditTrue_] += 0x100000000ULL;
    LeaveCriticalSection(&g_random_audit_lock_);
    LogRandomAudit_("64-bit");
    require(bad_log == 0, "64-bit format and equation");
    require(warning_lines == 6, "first full-path warning per entry/category");
    DeleteCriticalSection(&g_random_audit_lock_);
    std::printf("PASS: outcomes, nested/pending, original heartbeat, 80000 concurrent calls, 64-bit counters\n");
    return 0;
}
