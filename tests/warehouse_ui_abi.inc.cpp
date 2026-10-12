// Controlled ABI fixture for the HD_SOD 0x39E66..0x39E80 LoHook window.
// No HD_SOD/game module or game UI API is loaded or called here.
#pragma optimize("", off)

static_assert(sizeof(void*) == 4, "warehouse UI ABI fixture requires x86");
static_assert(sizeof(FlagsRegister) == sizeof(DWORD), "HookContext flags must be DWORD");

static constexpr UINT32 kHsodWarehouseUiHookRva_ = 0x039E66u;
static constexpr UINT32 kHsodWarehouseUiEndRva_ = 0x039E80u;
static const BYTE kHsodWarehouseUiHookBytes_[] = {
    0x6A, 0x00,                   // push 0 (ShowAndRun mode)
    0xBA, 0x04, 0x00, 0x00, 0x00, // mov edx, 4
    0x6B, 0xC2, 0x06,             // imul eax, edx, 6
    0x8B, 0x4D, 0xF8,             // mov ecx, [ebp-8]
    0x8B, 0x11,                   // mov edx, [ecx]
    0x8B, 0x4D, 0xF8,             // mov ecx, [ebp-8]
    0x8B, 0x04, 0x02,             // mov eax, [edx+eax]
    0xFF, 0xD0                    // call eax (FF D0)
};
static const BYTE kHsodWarehouseUiEndBytes_[] = {
    0xC7, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00
};
static UINT32 g_wh_ui_site_ = 0;
static UINT32 g_wh_ui_after_prefix_site_ = 0;
static UINT32 g_wh_ui_end_site_ = 0;
static UINT32 g_wh_ui_dlg_ = 0;
static UINT32 g_wh_ui_expected_flags_ = 0;
static UINT32 g_wh_ui_saved_ebx_ = 0;
static UINT32 g_wh_ui_saved_esi_ = 0;
static UINT32 g_wh_ui_saved_edi_ = 0;
static UINT32 g_wh_ui_after_show_esp_ = 0;
static UINT32 g_wh_ui_show_result_ = 0;
static UINT32 g_wh_ui_wrapper_ebx_ = 0;
static UINT32 g_wh_ui_wrapper_esi_ = 0;
static UINT32 g_wh_ui_wrapper_edi_ = 0;
static UINT32 g_wh_ui_wrapper_eax_ = 0;
static int g_wh_ui_hook_calls_ = 0;
static int g_wh_ui_show_calls_ = 0;
static int g_wh_ui_show_self_ = 0;
static int g_wh_ui_show_mode_ = -1;
static bool g_wh_ui_show_throw_ = false;
static constexpr DWORD kWhUiException_ = 0xE0424833;
static constexpr int kWhUiShowResult_ = 0x24681357;
static bool WarehouseUiCallOpcodeSupported_(const BYTE* bytes)
{
    return bytes && (bytes[0] == 0xE8
        || (bytes[0] == 0xFF && bytes[1] == 0x15));
}
static void WarehouseUiCallOpcodeContract_()
{
    // CALL_ is documented and supported only for E8 and FF 15. FF D0 (call eax)
    // is outside the contract, so never install CALL_ on such an opcode.
    const BYTE callE8[] = { 0xE8, 0x00 };
    const BYTE callFf15[] = { 0xFF, 0x15 };
    const BYTE callFfD0[] = { 0xFF, 0xD0 };
    CheckHook_(WarehouseUiCallOpcodeSupported_(callE8)
        && WarehouseUiCallOpcodeSupported_(callFf15),
        "CALL_ supports E8 and FF 15 opcodes");
    CheckHook_(!WarehouseUiCallOpcodeSupported_(callFfD0),
        "FF D0 is not a supported CALL_ opcode and must not be installed blindly");
}

static DWORD WarehouseUiFlagsWord_(const FlagsRegister& flags)
{
    DWORD word = 0;
    std::memcpy(&word, &flags, sizeof(word));
    return word;
}

static int __fastcall MockWarehouseUiShow_(int self, int, int mode)
{
    ++g_wh_ui_show_calls_;
    g_wh_ui_show_self_ = self;
    g_wh_ui_show_mode_ = mode;
    if (g_wh_ui_show_throw_)
        RaiseException(kWhUiException_, 0, 0, nullptr);
    g_wh_ui_show_result_ = kWhUiShowResult_;
    return kWhUiShowResult_;
}

static __declspec(naked) int __stdcall MockWarehouseUiDialog_(
    int, int, int, int, int, int)
{
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 8
        mov eax, dword ptr [g_wh_ui_dlg_]
        mov dword ptr [ebp - 8], eax

        mov dword ptr [g_wh_ui_saved_ebx_], ebx
        mov dword ptr [g_wh_ui_saved_esi_], esi
        mov dword ptr [g_wh_ui_saved_edi_], edi
        mov eax, 11223344h
        mov ecx, 55667788h
        mov edx, 66778899h
        mov ebx, 778899AAh
        mov esi, 12344321h
        mov edi, 23455432h
        cmp eax, eax
        pushfd
        pop dword ptr [ebp - 4]
        push dword ptr [ebp - 4]
        pop dword ptr [g_wh_ui_expected_flags_]

        mov dword ptr [g_wh_ui_site_], offset wh_ui_site_
        mov dword ptr [g_wh_ui_end_site_], offset wh_ui_end_site_
wh_ui_site_:
        // 0x39E66..0x39E80 reproduced instruction-for-instruction (0x1A bytes):
        // push 0 / mov edx,4 / imul eax,edx,6 / mov ecx,[ebp-8] / mov edx,[ecx]
        // / mov ecx,[ebp-8] / mov eax,[edx+eax] / FF D0 call eax / mov ecx,[ebp-8]
        push 0
        mov edx, 4
        imul eax, edx, 6
        mov ecx, dword ptr [ebp - 8]
        mov edx, dword ptr [ecx]
        mov ecx, dword ptr [ebp - 8]
        mov eax, dword ptr [edx + eax]
        call eax
        mov ecx, dword ptr [ebp - 8]
wh_ui_end_site_:
        mov dword ptr [ecx + 8], 0
        mov dword ptr [g_wh_ui_show_result_], eax
        mov dword ptr [g_wh_ui_after_show_esp_], esp

        mov ebx, dword ptr [g_wh_ui_saved_ebx_]
        mov esi, dword ptr [g_wh_ui_saved_esi_]
        mov edi, dword ptr [g_wh_ui_saved_edi_]
        mov eax, dword ptr [g_wh_ui_show_result_]
        mov esp, ebp
        pop ebp
        ret 24
    }
}

static __declspec(naked) int CallMockWarehouseUiDialog_()
{
    __asm {
        push ebx
        push esi
        push edi
        mov ebx, 0A1B2C3D4h
        mov esi, 0B1C2D3E4h
        mov edi, 0C1D2E3F4h
        push 0A6h
        push 0A5h
        push 0A4h
        push 033333333h
        push 0202h
        push 0101h
        call MockWarehouseUiDialog_
        mov dword ptr [g_wh_ui_wrapper_eax_], eax
        mov dword ptr [g_wh_ui_wrapper_ebx_], ebx
        mov dword ptr [g_wh_ui_wrapper_esi_], esi
        mov dword ptr [g_wh_ui_wrapper_edi_], edi
        pop edi
        pop esi
        pop ebx
        ret
    }
}

struct WarehouseUiHookSnapshot_ {
    DWORD eax, ecx, edx, ebx, esp, ebp, esi, edi;
    DWORD returnAddress;
    DWORD flags;
    DWORD dlg;
    DWORD args[6];
};
static WarehouseUiHookSnapshot_ g_wh_ui_hook_snapshot_ = {};
static WarehouseUiHookSnapshot_ g_wh_ui_prefix_snapshot_ = {};

static void WarehouseUiCaptureHookContext_(HookContext& context,
    WarehouseUiHookSnapshot_& snapshot)
{
    snapshot.eax = static_cast<DWORD>(context.eax);
    snapshot.ecx = static_cast<DWORD>(context.ecx);
    snapshot.edx = static_cast<DWORD>(context.edx);
    snapshot.ebx = static_cast<DWORD>(context.ebx);
    snapshot.esp = static_cast<DWORD>(context.esp);
    snapshot.ebp = static_cast<DWORD>(context.ebp);
    snapshot.esi = static_cast<DWORD>(context.esi);
    snapshot.edi = static_cast<DWORD>(context.edi);
    snapshot.returnAddress = context.return_address;
    snapshot.flags = WarehouseUiFlagsWord_(context.flags);
    snapshot.dlg = static_cast<DWORD>(context.Local(2));
    for (DWORD i = 1; i <= 6; ++i)
        snapshot.args[i - 1] = static_cast<DWORD>(context.Arg(i));
}

static int __stdcall WarehouseUiLoHook_(LoHook* hook, HookContext* context)
{
    ++g_wh_ui_hook_calls_;
    CheckHook_(hook != nullptr && context != nullptr, "LoHook receives hook/context");
    if (!context) return EXEC_DEFAULT;
    WarehouseUiCaptureHookContext_(*context, g_wh_ui_hook_snapshot_);
    return EXEC_DEFAULT;
}

// The second LoHook sits right before IMUL, so it observes the exact state the
// first hook restored after push 0 / mov edx,4 executed.
static int __stdcall WarehouseUiPrefixLoHook_(LoHook* hook, HookContext* context)
{
    ++g_wh_ui_hook_calls_;
    CheckHook_(hook != nullptr && context != nullptr, "prefix LoHook receives hook/context");
    if (!context) return EXEC_DEFAULT;
    WarehouseUiCaptureHookContext_(*context, g_wh_ui_prefix_snapshot_);
    return EXEC_DEFAULT;
}

static UINT32 g_wh_ui_callback_target_ = 0;
static int g_wh_ui_callback_calls_ = 0;
static int g_wh_ui_callback_self_ = 0;
static int g_wh_ui_callback_msg_ = 0;
static int g_wh_ui_callback_result_ = 0;
static int g_wh_ui_replace_calls_ = 0;
static int g_wh_ui_replace_self_ = 0;
static int g_wh_ui_replace_msg_ = 0;
static int g_wh_ui_replace_result_ = 0;

static int __stdcall MockWarehouseUiCallback_(void* self, int msg)
{
    ++g_wh_ui_callback_calls_;
    g_wh_ui_callback_self_ = static_cast<int>(reinterpret_cast<intptr_t>(self));
    g_wh_ui_callback_msg_ = msg;
    g_wh_ui_callback_result_ = 0x13572468;
    return g_wh_ui_callback_result_;
}

static int __stdcall ReplacementWarehouseUiCallback_(void* self, int msg)
{
    ++g_wh_ui_replace_calls_;
    g_wh_ui_replace_self_ = static_cast<int>(reinterpret_cast<intptr_t>(self));
    g_wh_ui_replace_msg_ = msg;
    g_wh_ui_replace_result_ = 0x24681357;
    return g_wh_ui_replace_result_;
}

static __declspec(naked) int __stdcall CallWarehouseUiCallback_(void*, int)
{
    // The game's 010FB2A0 bridge pushes msg then this and calls the stored
    // callback; the callback returns with ret 8 (stdcall this,msg).
    __asm {
        mov ecx, dword ptr [esp + 4]
        mov edx, dword ptr [esp + 8]
        push edx
        push ecx
        call dword ptr [g_wh_ui_callback_target_]
        ret 8
    }
}

static bool CatchWarehouseUiDefaultException_()
{
    __try {
        CallMockWarehouseUiDialog_();
    }
    __except (GetExceptionCode() == kWhUiException_
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}

static void RunWarehouseUiLoAbiTests_()
{
    DWORD dialog[16] = {};
    DWORD vtable[8] = {};
    // dialog+0 is the vtable pointer; dialog+8 is the field the window's
    // terminal mov [ecx+8],0 clears. vtable+18h (slot 6) is ShowAndRun.
    dialog[0] = reinterpret_cast<DWORD>(vtable);
    vtable[6] = reinterpret_cast<DWORD>(&MockWarehouseUiShow_);
    g_wh_ui_dlg_ = reinterpret_cast<DWORD>(dialog);

    UINT32 espBefore = 0, espAfter = 0;
    __asm { mov espBefore, esp }
    const int baseline = CallMockWarehouseUiDialog_();
    __asm { mov espAfter, esp }
    CheckHook_(baseline == kWhUiShowResult_, "controlled stdcall dialog returns show result");
    CheckHook_(espBefore == espAfter,
        "unhooked six-argument stdcall fixture balances ESP");
    CheckHook_(g_wh_ui_site_ != 0 && g_wh_ui_end_site_ != 0,
        "controlled function exposes internal hook/end labels");
    // The prefix probe starts at IMUL (7 bytes in), before any EAX/EFLAGS change.
    g_wh_ui_after_prefix_site_ = g_wh_ui_site_ + 7u;
    CheckHook_(g_wh_ui_show_calls_ == 1 && g_wh_ui_show_self_ == g_wh_ui_dlg_
        && g_wh_ui_show_mode_ == 0,
        "FF D0 vtable+18h show receives thiscall-equivalent arguments");
    CheckHook_(g_wh_ui_show_result_ == kWhUiShowResult_ && dialog[2] == 0,
        "show return captured and end mov [ecx+8],0 executed");
    CheckHook_(g_wh_ui_wrapper_ebx_ == 0xA1B2C3D4u
        && g_wh_ui_wrapper_esi_ == 0xB1C2D3E4u
        && g_wh_ui_wrapper_edi_ == 0xC1D2E3F4u,
        "unhooked fixture preserves caller nonvolatile registers");

    CheckHook_(std::memcmp(reinterpret_cast<const void*>(g_wh_ui_site_),
        kHsodWarehouseUiHookBytes_, sizeof(kHsodWarehouseUiHookBytes_)) == 0
        && g_wh_ui_end_site_ - g_wh_ui_site_ == 0x1Au,
        "controlled window reproduces HD_SOD 0x39E66..0x39E80 byte-for-byte (0x1A)");
    CheckHook_(std::memcmp(reinterpret_cast<const void*>(g_wh_ui_end_site_),
        kHsodWarehouseUiEndBytes_, sizeof(kHsodWarehouseUiEndBytes_)) == 0,
        "controlled HD_SOD 0x39E80 bytes are mov [ecx+8],0");

    WarehouseUiCallOpcodeContract_();
    PatcherInstance* uiInstance = _P->CreateInstance("H3RndNew.WarehouseUiAbiTest");
    CheckHook_(uiInstance != nullptr, "create isolated warehouse UI ABI patcher instance");
    if (!uiInstance) return;
    LoHook* loHook = uiInstance->WriteLoHook(g_wh_ui_site_, &WarehouseUiLoHook_);
    CheckHook_(loHook != nullptr, "install production-like LoHook at window start");
    LoHook* prefixHook = uiInstance->WriteLoHook(g_wh_ui_after_prefix_site_,
        &WarehouseUiPrefixLoHook_);
    CheckHook_(prefixHook != nullptr, "install observation LoHook right before IMUL");
    if (!loHook || !prefixHook) return;

    g_wh_ui_hook_calls_ = 0;
    g_wh_ui_show_calls_ = 0;
    g_wh_ui_show_throw_ = false;
    dialog[2] = 0xFFFFFFFFu;
    __asm { mov espBefore, esp }
    const int hooked = CallMockWarehouseUiDialog_();
    __asm { mov espAfter, esp }
    CheckHook_(hooked == kWhUiShowResult_ && g_wh_ui_show_result_ == kWhUiShowResult_,
        "LoHook EXEC_DEFAULT preserves vtable show return");
    CheckHook_(espBefore == espAfter
        && g_wh_ui_after_show_esp_ == g_wh_ui_hook_snapshot_.ebp - 8,
        "LoHook and FF D0 show preserve six-argument stdcall ESP");
    CheckHook_(g_wh_ui_show_calls_ == 1 && g_wh_ui_hook_calls_ == 2,
        "FF D0 original show and both LoHooks fire exactly once");
    CheckHook_(g_wh_ui_show_self_ == g_wh_ui_dlg_
        && g_wh_ui_show_mode_ == 0 && dialog[2] == 0,
        "hooked show arguments and terminal dialog field match");

    const WarehouseUiHookSnapshot_& seen = g_wh_ui_hook_snapshot_;
    const WarehouseUiHookSnapshot_& resumed = g_wh_ui_prefix_snapshot_;
    CheckHook_(seen.eax == 0x11223344u && seen.ecx == 0x55667788u
        && seen.edx == 0x66778899u && seen.ebx == 0x778899AAu
        && seen.esi == 0x12344321u && seen.edi == 0x23455432u,
        "LoHook context exposes every seeded general register");
    CheckHook_(seen.ebp != 0 && seen.esp == seen.ebp - 8
        && seen.dlg == g_wh_ui_dlg_, "Local(2) resolves dlg at ebp-8 and ESP frame");
    CheckHook_(seen.args[0] == 0x101u && seen.args[1] == 0x202u
        && seen.args[2] == 0x33333333u && seen.args[3] == 0xA4u
        && seen.args[4] == 0xA5u && seen.args[5] == 0xA6u,
        "LoHook Arg(1..6) reads six stdcall stack arguments");
    CheckHook_(seen.flags == g_wh_ui_expected_flags_
        && resumed.flags == seen.flags,
        "all EFLAGS survive the callback and its push/mov default instructions");
    CheckHook_(resumed.eax == 0x11223344u && resumed.ecx == 0x55667788u
        && resumed.edx == 4u && resumed.ebx == 0x778899AAu
        && resumed.esi == 0x12344321u && resumed.edi == 0x23455432u
        && resumed.ebp == seen.ebp && resumed.esp == seen.esp - 4,
        "EXEC_DEFAULT restores registers; push 0/mov edx,4 effects visible at IMUL");
    CheckHook_(g_wh_ui_wrapper_ebx_ == 0xA1B2C3D4u
        && g_wh_ui_wrapper_esi_ == 0xB1C2D3E4u
        && g_wh_ui_wrapper_edi_ == 0xC1D2E3F4u,
        "hooked six-argument stdcall fixture restores caller registers");

    g_wh_ui_hook_calls_ = 0;
    g_wh_ui_show_calls_ = 0;
    g_wh_ui_show_throw_ = true;
    CheckHook_(CatchWarehouseUiDefaultException_(), "default show SEH propagates through both hooks");
    CheckHook_(g_wh_ui_hook_calls_ == 2 && g_wh_ui_show_calls_ == 1,
        "SEH path enters both hooks once and original show exactly once");
    g_wh_ui_show_throw_ = false;

    const void* callbackSelf = reinterpret_cast<void*>(0x12345678u);
    const int callbackMsg = 0x20000C; // command 0x200, subtype 0x0C (left-down)
    g_wh_ui_callback_target_ = reinterpret_cast<UINT32>(&MockWarehouseUiCallback_);
    __asm { mov espBefore, esp }
    const int directResult = CallWarehouseUiCallback_(const_cast<void*>(callbackSelf), callbackMsg);
    __asm { mov espAfter, esp }
    CheckHook_(directResult == 0x13572468 && espBefore == espAfter
        && g_wh_ui_callback_calls_ == 1 && g_wh_ui_callback_self_ == 0x12345678
        && g_wh_ui_callback_msg_ == callbackMsg,
        "direct stdcall(this,msg) callback returns correctly with ret 8");

    g_wh_ui_callback_target_ = reinterpret_cast<UINT32>(&ReplacementWarehouseUiCallback_);
    __asm { mov espBefore, esp }
    const int replacementResult = CallWarehouseUiCallback_(const_cast<void*>(callbackSelf), callbackMsg);
    __asm { mov espAfter, esp }
    CheckHook_(replacementResult == 0x24681357 && espBefore == espAfter
        && g_wh_ui_replace_calls_ == 1 && g_wh_ui_replace_self_ == 0x12345678
        && g_wh_ui_replace_msg_ == callbackMsg,
        "replacement stdcall(this,msg) callback preserves ret 8");

    std::printf("PASS warehouse UI LoHook ABI: RVA 0x%X window %Xh, EBP locals/6 stdcall args, regs/EFLAGS/ESP, show result, SEH, callback ret8\n",
        kHsodWarehouseUiHookRva_,
        kHsodWarehouseUiEndRva_ - kHsodWarehouseUiHookRva_);
}

