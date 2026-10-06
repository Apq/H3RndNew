// Exercise the production callbacks with the installed x86 patcher, not Heroes3.
#define DllMain UnusedPluginDllMainForTest
#include "../H3RndNew.cpp"
#undef DllMain
#include <cstdio>
#include <cstdlib>

static void CheckHook_(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL hook ABI: %s\n", message); std::exit(1); }
}
// A patched local function must remain a standard ABI boundary. Otherwise MSVC
// can preserve volatile registers across a call using its known local body.
#pragma optimize("", off)
static unsigned test_seed_ = 0;
static int test_calls_ = 0;
static volatile int test_lo_ = 0, test_hi_ = 0, test_raw_ = 0;
static bool test_throw_ = false;
static constexpr DWORD kTestException_ = 0xE0424242;

static __declspec(noinline) int __cdecl MockCrt_()
{
    ++test_calls_;
    test_seed_ = test_seed_ * 0x343FDu + 0x269EC3u;
    return (test_seed_ >> 16) & 0x7FFFu;
}
static __declspec(noinline) int __cdecl MockOldCrt_()
{
    ++test_calls_;
    return 0x1357;
}
static __declspec(naked) int CallOldCrt_()
{
    __asm {
        mov eax, 10203040h
        call MockOldCrt_
        ret
    }
}
static __declspec(noinline) int __fastcall MockGame_(int lo, int hi)
{
    test_lo_ = lo;
    test_hi_ = hi;
    if (test_throw_) {
        ++test_calls_;
        RaiseException(kTestException_, 0, 0, nullptr);
    }
    if (hi <= lo) return lo;
    const int raw = MockCrt_();
    test_raw_ = raw;
    return raw % (hi - lo + 1) + lo;
}
static __declspec(noinline) int __fastcall MockStart_(int self)
{
    ++test_calls_;
    return self ^ 0x123456;
}
static __declspec(noinline) H3SelectScenarioDialog* __fastcall MockCtor_(
    H3SelectScenarioDialog* dlg, int, int mode)
{
    ++test_calls_;
    dlg->isCampaignMaybe = mode == 1;
    dlg->isLoadingMaybe = mode == 2;
    return reinterpret_cast<H3SelectScenarioDialog*>(0x12345678);
}
static bool CatchDefaultException_()
{
    __try { MockGame_(5, 5); }
    __except (GetExceptionCode() == kTestException_
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}

void RunHookAbiTests_()
{
    // Load the exact deployed patcher before GetPatcher resolves it by basename.
    wchar_t path[MAX_PATH] = {};
    CheckHook_(GetEnvironmentVariableW(L"H3RND_PATCHER_DLL", path, MAX_PATH) > 0,
        "H3RND_PATCHER_DLL must point to installed patcher_x86.dll");
    CheckHook_(LoadLibraryW(path) != nullptr, "load installed patcher");
    _P = GetPatcher();
    CheckHook_(_P != nullptr, "GetPatcher");
    _PI = _P->CreateInstance("H3RndNew.HookAbiTest");
    CheckHook_(_PI != nullptr, "create test instance");
    InitRandomAudit_();
    g_disable_log = true;
    g_true_random = 0;
    g_true_random_full = 0;
    InterlockedExchange(&g_in_new_game_, 0);

    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockGame_), SPLICE_,
        EXTENDED_, FASTCALL_, reinterpret_cast<void*>(&OnGameRand_)) != nullptr,
        "install production Rand callback");
    HiHook* crt_hook = _PI->WriteHiHook(reinterpret_cast<UINT32>(&MockCrt_), SPLICE_,
        EXTENDED_, STDCALL_, reinterpret_cast<void*>(&OnCrtRand_));
    CheckHook_(crt_hook != nullptr, "install production CRT callback");
    RngChainSnapshot_ chain_before = {}, chain_same = {}, chain_after = {};
    CaptureRngHookChain_(reinterpret_cast<UINT32>(&MockOldCrt_), chain_before);
    CaptureRngHookChain_(reinterpret_cast<UINT32>(&MockOldCrt_), chain_same);
    CheckHook_(memcmp(&chain_before, &chain_same, sizeof(chain_before)) == 0,
        "unchanged chain snapshot compares equal");
    HiHook* old_hook = _PI->WriteHiHook(reinterpret_cast<UINT32>(&MockOldCrt_), SPLICE_,
        EXTENDED_, CDECL_, reinterpret_cast<void*>(&OnCrtRand_));
    CheckHook_(old_hook != nullptr, "install negative-control CDECL bridge");
    CaptureRngHookChain_(reinterpret_cast<UINT32>(&MockOldCrt_), chain_after);
    CheckHook_(memcmp(&chain_before, &chain_after, sizeof(chain_before)) != 0
        && chain_after.count > 0, "new hook changes chain snapshot");
    CaptureRngHookChain_(reinterpret_cast<UINT32>(&MockOldCrt_), chain_same);
    CheckHook_(memcmp(&chain_after, &chain_same, sizeof(chain_after)) == 0,
        "installed hook snapshot remains stable");
    CheckHook_(CDECL_0(int, old_hook->GetDefaultFunc()) == 0x1357, "old original bridge correct");
    CheckHook_(CallOldCrt_() != 0x1357, "installed CDECL bridge reproduces lost result");
    unsigned expected = 0x13579BDFu;
    test_seed_ = expected;
    test_calls_ = 0;
    for (int i = 0; i < 10000; ++i) {
        expected = expected * 0x343FDu + 0x269EC3u;
        const int actual = MockGame_(-17, 93);
        const int wanted = static_cast<int>((expected >> 16) & 0x7FFFu) % 111 - 17;
        if (actual != wanted) std::fprintf(stderr,
            "RNG mismatch i=%d actual=%d wanted=%d seed=%08X expected=%08X calls=%d lo=%d hi=%d raw=%d\n",
            i, actual, wanted, test_seed_, expected, test_calls_, test_lo_, test_hi_, test_raw_);
        CheckHook_(actual == wanted, "original-path result sequence");
        CheckHook_(test_seed_ == expected, "original-path seed sequence");
    }
    CheckHook_(test_calls_ == 10000, "nested default function called exactly once");
    const unsigned before = test_seed_;
    CheckHook_(MockGame_(7, 7) == 7 && MockGame_(9, 4) == 9, "no-draw bounds");
    CheckHook_(test_seed_ == before, "no-draw leaves seed untouched");
    test_calls_ = 0;
    test_throw_ = true;
    CheckHook_(CatchDefaultException_(), "default SEH propagates");
    CheckHook_(test_calls_ == 1, "throwing default not called twice");
    test_throw_ = false;
    CheckHook_(g_random_audit_.session[0].pending == 0, "SEH clears pending audit");
    g_true_random_full = 1;
    const unsigned full_seed = test_seed_;
    const unsigned long long true_before = g_random_audit_.session[1].outcome[kAuditTrue_];
    test_calls_ = 0;
    for (int i = 0; i < 1000; ++i) {
        const int roll = MockCrt_();
        CheckHook_(roll >= 0 && roll <= 0x7FFF, "full replacement return range");
    }
    CheckHook_(test_seed_ == full_seed && test_calls_ == 0, "full mode leaves default seed untouched");
    CheckHook_(g_random_audit_.session[1].outcome[kAuditTrue_] == true_before + 1000,
        "full CRT replacement audited");
    g_true_random_full = 0;

    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockStart_), SPLICE_,
        EXTENDED_, THISCALL_, reinterpret_cast<void*>(&OnStartGame_)) != nullptr,
        "install production one-register callback");
    unsigned stack_before = 0;
    unsigned stack_after = 0;
    __asm { mov stack_before, esp }
    // No game UI methods execute in the mode=1 post-constructor path.
    void* storage = std::calloc(1, sizeof(H3SelectScenarioDialog));
    CheckHook_(storage != nullptr, "allocate scenario storage");
    H3SelectScenarioDialog* dlg = static_cast<H3SelectScenarioDialog*>(storage);
    test_calls_ = 0;
    for (int i = 0; i < 1000; ++i) {
        ObserveScenarioContext_(kScenarioLoad_);
        CheckHook_(MockStart_(reinterpret_cast<int>(dlg)) == (reinterpret_cast<int>(dlg) ^ 0x123456),
            "one-register callback argument/return and stack balance");
    }
    __asm { mov stack_after, esp }
    CheckHook_(stack_before == stack_after, "one-register hook preserves ESP");
    CheckHook_(test_calls_ == 1000 && g_in_new_game_ == 0, "load delegates once without window");
    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockCtor_), SPLICE_,
        EXTENDED_, THISCALL_, reinterpret_cast<void*>(&OnScenarioCtor_)) != nullptr,
        "install production constructor callback");
    CheckHook_(MockCtor_(dlg, 0, 1) == reinterpret_cast<H3SelectScenarioDialog*>(0x12345678),
        "constructor preserves original returned pointer");
    CheckHook_(ConsumeScenarioContext_() == kScenarioLoad_, "constructor classifies load byte");
    std::free(storage);
    DeleteCriticalSection(&g_random_audit_lock_);
    std::printf("PASS: installed patcher, production callbacks, 10000 RNG results/seeds, no-draw, SEH once, 1000 full CRT draws, 1000 one-register calls/ESP, constructor return, chain dedup/change\n");
}
