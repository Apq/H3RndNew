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
static std::atomic<int> debug_lines{0};
static int g_true_random_full = 0;
static void CheckAuditLog_(const char* format, va_list args)
{
    char text[2048];
    vsnprintf(text, sizeof(text), format, args);
    if (strstr(text, "RNG")) {
        ++audit_lines;
        if (!strstr(text, "=OK")) ++bad_log;
    }
}
static void LogInfo(const char* format, ...)
{
    va_list args; va_start(args, format);
    CheckAuditLog_(format, args); va_end(args);
}
static void LogDebug(const char* format, ...)
{
    ++debug_lines;
    va_list args; va_start(args, format);
    CheckAuditLog_(format, args); va_end(args);
}
static void LogWarn(const char*, ...) { ++warning_lines; }
#include "../modules/RandomAudit.inc.cpp"
#include "../modules/ScenarioContext.hpp"
#include "../modules/TextLayout.hpp"
#include "../modules/BankMaximizeCore.hpp"
struct ArtifactMock_ {
    int type;
    int disabled;
};
static int MockRoll_(int count) { return count > 1 ? count - 1 : 0; }
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
void RunHookAbiTests_();
void RunBankRewardLayoutTests_();
void RunWarehouseTests_();
void RunWarehouseCoreTests_();
void RunWarehouseStoreTests_();
void RunWarehouseNativeTests_();
int main()
{
    const bankmax::ControlsLayout controls(422);
    require(controls.Fits(766, 24) && controls.Fits(800, 24)
        && !controls.Fits(765, 24) && !controls.Fits(766, 25)
        && controls.percentX + controls.percentWidth == 760, "HD/native layout width and font boundaries");
    require(bankmax::ClampRatio(-1) == 0 && bankmax::ClampRatio(1001) == 1000,
        "ratio clamping");
    require(bankmax::SuccessfulNewGame(true, 0xABCD01u)
        && !bankmax::SuccessfulNewGame(true, 0xABCD00u)
        && !bankmax::SuccessfulNewGame(false, 1u), "new-game result low byte gate");
    require(bankmax::ResourceLimit(0) == 10 && bankmax::ResourceLimit(1) == 6
        && bankmax::ResourceLimit(6) == 10, "resource maximums");
    require(bankmax::RatioFromPixel(-5, 100) == 0
        && bankmax::RatioFromPixel(500, 100) == 1000
        && bankmax::RatioFromPixel(99, 100) == 1000
        && bankmax::RatioFromPixel(0, 100) == 0
        && bankmax::RatioFromPixel(49, 100) == 495, "pixel-to-ratio mapping");
    bankmax::SliderGesture gesture;
    bool inside = true;
    require(gesture.Handle(bankmax::SliderEvent::Down, inside)
        == bankmax::SliderAction::Update, "slider press updates ratio");
    require(gesture.Handle(bankmax::SliderEvent::Move, true)
        == bankmax::SliderAction::Update, "slider drag updates ratio");
    require(gesture.Handle(bankmax::SliderEvent::Click, false)
        == bankmax::SliderAction::Consume, "click swallowed after drag");
    require(gesture.Handle(bankmax::SliderEvent::Click, false)
        == bankmax::SliderAction::Pass, "later click not swallowed");
    require(gesture.Handle(bankmax::SliderEvent::Down, false)
        == bankmax::SliderAction::Pass, "press outside passes");
    require(gesture.Handle(bankmax::SliderEvent::Click, inside)
        == bankmax::SliderAction::Pass, "outside click passes");
    require(gesture.Handle(bankmax::SliderEvent::Down, true)
        == bankmax::SliderAction::Update, "new slider drag starts");
    require(gesture.Handle(bankmax::SliderEvent::Move, false, true)
        == bankmax::SliderAction::Update, "held drag outside continues");
    require(gesture.Handle(bankmax::SliderEvent::Move, true, false)
        == bankmax::SliderAction::Consume && !gesture.dragging,
        "missing release stops on physical button-up without updating ratio");
    require(gesture.Handle(bankmax::SliderEvent::Move, true, false)
        == bankmax::SliderAction::Pass, "hover after release never moves slider");
    require(gesture.Handle(bankmax::SliderEvent::Click, false)
        == bankmax::SliderAction::Consume, "late release click still swallowed");
    require((bankmax::ChestSetup(0xF800u) & 0x7800u) == 0x2000u,
        "treasure chest 2000-gold tier");
    for (int type = 0; type <= 6; ++type) {
        const unsigned flags = 0xFFA80000u;
        require((bankmax::ResourceSetup(flags | 1u, type) & 0x7FFFFu)
            == static_cast<unsigned>(bankmax::ResourceLimit(type)), "resource max by type");
        require((bankmax::ResourceSetup(flags | 1u, type) & ~0x7FFFFu)
            == flags, "resource message and guardian flags preserved");
        require(bankmax::ResourceSetup(flags | 99u, type) == (flags | 99u),
            "higher authored resource remains intact");
    }
    for (unsigned type = 0; type < 6; ++type) {
        require(bankmax::CampfireSetup((4u << 4) | type) == ((6u << 4) | type),
            "campfire max quantity preserves type");
        require(bankmax::CampfireSetup((10u << 4) | type) == ((10u << 4) | type),
            "higher campfire preserved");
    }
    ArtifactMock_ info[144] = {};
    signed char bans[144] = {}, pool[144] = {};
    info[10].type = info[11].type = info[12].type = info[13].type = 2;
    info[11].disabled = 1;
    bans[12] = 1;
    pool[13] = 1;
    info[20].type = 16;
    require(bankmax::PickArtifact(info, bans, pool, 2, &MockRoll_) == 10,
        "strict selection excludes disabled map-ban used and wrong class");
    require(bankmax::PickArtifact(info, bans, pool, 2, &MockRoll_) == 13
        && pool[20] == 0, "exhausted class resets only itself");
    bans[10] = bans[13] = 1;
    require(bankmax::PickArtifact(info, bans, pool, 2, &MockRoll_) == -1,
        "all treasure banned has no cross-class fallback");
    std::puts("PASS bank-max rewards: success gate, setup bits, map artifact bans, pixel ratio and gestures");
    require(FontTextHeight_(0, 17) == 17, "zero height font floor");
    require(FontTextHeight_(14, 17) == 17, "short height font floor");
    require(FontTextHeight_(-1, 0) == 17, "positive fallback height");
    const SmallFontLayout_ defaults = MakeSmallFontLayout_(17);
    require(defaults.fits && defaults.text_height == 24 && defaults.item_height == 20
        && defaults.log_y == 44 && defaults.list_y == 70 && defaults.list_height == 104
        && defaults.full_y == 84 && defaults.ok_y == 140 && defaults.window_height == 190,
        "default dialog geometry unchanged");
    for (int font = 1; font <= 32; ++font) {
        const SmallFontLayout_ layout = MakeSmallFontLayout_(font);
        require(layout.fits && layout.text_height >= font && layout.item_height >= font,
            "font floor on every text row");
        require(12 + layout.text_height <= layout.log_y
            && layout.log_y + layout.text_height <= layout.full_y
            && layout.full_y + layout.text_height <= layout.ok_y,
            "closed dialog rows do not overlap");
        require(layout.list_y + 5 * layout.item_height + 2 <= layout.window_height,
            "list and surface inside dialog");
        for (int level = 0; level < 5; ++level) {
            const int top = layout.list_y + level * layout.item_height;
            require((top - layout.list_y) / layout.item_height == level
                && (top + layout.item_height - 1 - layout.list_y) / layout.item_height == level,
                "list pitch and first/last pixel hit agree");
        }
    }
    require(!MakeSmallFontLayout_(33).fits && !MakeSmallFontLayout_(255).fits,
        "oversized font has bounded layout rejection");
    std::puts("PASS small-font text height and dialog geometry");
    RunBankRewardLayoutTests_();
    // Pure warehouse tests do not require the optional installed patcher DLL;
    // run them before the ABI integration gate so their result is observable.
    RunWarehouseTests_();
    RunWarehouseCoreTests_();
    RunWarehouseStoreTests_();
    RunWarehouseNativeTests_();
    RunHookAbiTests_();
    // Window authorization is independent of mouse/key messages and elapsed time.
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "unknown denies window");
    ObserveScenarioContext_(ClassifyScenarioContext_(true, false));
    require(ConsumeScenarioContext_() == kScenarioLoad_, "load before battle");
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "context consumed once");
    ObserveScenarioContext_(ClassifyScenarioContext_(true, false));
    ObserveScenarioContext_(ClassifyScenarioContext_(false, false));
    require(AllowsNewGameWindow_(ConsumeScenarioContext_()), "load cancelled then single scenario");
    ObserveScenarioContext_(ClassifyScenarioContext_(true, false));
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "new game then load");
    ObserveScenarioContext_(ClassifyScenarioContext_(false, true));
    require(ConsumeScenarioContext_() == kScenarioSave_, "save context");
    require(!AllowsNewGameWindow_(kScenarioSave_), "save denies window");
    // A message-pre snapshot also works when a constructor hook was bypassed.
    ObserveScenarioContext_(ClassifyScenarioContext_(true, false));
    require(!AllowsNewGameWindow_(ConsumeScenarioContext_()), "load keyboard confirm");
    ObserveScenarioContext_(ClassifyScenarioContext_(false, false));
    require(AllowsNewGameWindow_(ConsumeScenarioContext_()), "new game keyboard confirm");
    require(ClassifyScenarioContext_(true, true) == kScenarioLoad_, "load takes priority");
    InitRandomAudit_();
    LogRandomAudit_("startup");
    require(audit_lines == 2, "empty inactive full scope omitted");
    g_true_random_full = 1;
    LogRandomAudit_("full enabled before first draw");
    require(audit_lines == 6, "enabled empty full scope retained");
    g_true_random_full = 0;
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
    const int history_before = audit_lines;
    LogRandomAudit_("full disabled with history");
    require(audit_lines == history_before + 4, "disabled full history retained");
    // Original paths trigger bounded Debug and Info heartbeats without replacement.
    const int before = audit_lines;
    const int debug_before = debug_lines;
    EnterCriticalSection(&g_random_audit_lock_);
    g_random_audit_last_log_ = GetTickCount() - kAuditDebugInterval_ - 1u;
    g_random_audit_last_info_ = GetTickCount();
    LeaveCriticalSection(&g_random_audit_lock_);
    BeginRandomAudit_(1, false); EndRandomAudit_(1, false, kAuditOriginal_);
    require(audit_lines == before + 4 && debug_lines == debug_before + 4,
        "original-path Debug time heartbeat");
    // A call-count milestone cannot bypass the time limit.
    EnterCriticalSection(&g_random_audit_lock_);
    g_random_audit_.completed = 0xFFFFu;
    g_random_audit_last_log_ = g_random_audit_last_info_ = GetTickCount();
    LeaveCriticalSection(&g_random_audit_lock_);
    BeginRandomAudit_(1, false); EndRandomAudit_(1, false, kAuditOriginal_);
    require(audit_lines == before + 4, "65536 calls do not force output");
    EnterCriticalSection(&g_random_audit_lock_);
    g_random_audit_last_info_ = GetTickCount() - kAuditInfoInterval_ - 1u;
    LeaveCriticalSection(&g_random_audit_lock_);
    BeginRandomAudit_(1, false); EndRandomAudit_(1, false, kAuditOriginal_);
    require(audit_lines == before + 8 && debug_lines == debug_before + 4,
        "original-path Info time heartbeat");
    // Reset simulated timestamps before the concurrent test.
    LogRandomAudit_("heartbeat reset");
    const int bounded_debug = debug_lines;
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
        require(g_random_audit_.session[e].entered == 40011u + (e == 1 ? 3u : 0u), "parallel exact session total");
        require(g_random_audit_.full[e].entered == 20006u, "parallel exact full total");
    }
    require(debug_lines == bounded_debug, "80000 concurrent calls do not flood heartbeat");
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
    ++g_random_audit_.session[0].entered;
    LogRandomAudit_("mismatch must remain visible", true);
    require(warning_lines == 7, "heartbeat mismatch elevated to warning");
    --g_random_audit_.session[0].entered;
    DeleteCriticalSection(&g_random_audit_lock_);
    std::printf("PASS: outcomes, nested/pending, original heartbeat, 80000 concurrent calls, 64-bit counters\n");
    return 0;
}
