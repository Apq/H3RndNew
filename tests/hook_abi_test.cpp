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
void RunBankRewardLayoutTests_()
{
    // Raw game-layout storage avoids constructors that call Heroes3 addresses.
    H3CreatureBank* bank = static_cast<H3CreatureBank*>(std::calloc(1, sizeof(H3CreatureBank)));
    H3CreatureBankState* state = static_cast<H3CreatureBankState*>(std::calloc(1, sizeof(H3CreatureBankState)));
    CheckHook_(bank && state, "allocate native reward storage");
    std::memset(bank, 0xA5, sizeof(*bank));
    BYTE before[sizeof(H3CreatureBank)];
    std::memcpy(before, bank, sizeof(before));
    for (int i = 0; i < 7; ++i) state->resources.asArray[i] = 10 + i;
    state->creatureRewardType = 13;
    state->creatureRewardCount = 4;
    bankmax::CopyRewardFields(*bank, *state);
    CheckHook_(std::memcmp(before, &bank->guardians, sizeof(H3Army)) == 0,
        "native guardian bytes unchanged");
    CheckHook_(bank->resources.asArray[6] == 16 && bank->creatureRewardType == 13
        && bank->creatureRewardCount == 4, "native reward offsets updated");
    CheckHook_(std::memcmp(before + 0x59, reinterpret_cast<BYTE*>(bank) + 0x59,
        sizeof(*bank) - 0x59) == 0, "padding and artifact vector untouched by reward copy");
    std::free(bank); std::free(state);
    std::puts("PASS bank-max native reward layout and guardian byte preservation");
    // Exercise the production UTF-8 INI writer and layered reader using isolated
    // OS-created temporary files; never touch the player's configuration.
    wchar_t* tempDir = new wchar_t[kPathCap_ / 2];
    wchar_t* defaultFile = new wchar_t[kPathCap_ / 2];
    wchar_t* userFile = new wchar_t[kPathCap_ / 2];
    CheckHook_(GetTempPathW(kPathCap_ / 2, tempDir) != 0, "temporary directory");
    CheckHook_(GetTempFileNameW(tempDir, L"h3d", 0, defaultFile) != 0
        && GetTempFileNameW(tempDir, L"h3u", 0, userFile) != 0, "temporary INI files");
    char* savedDefault = g_ini_path;
    char* savedUser = g_user_ini_path;
    g_ini_path = new char[kPathCap_];
    g_user_ini_path = new char[kPathCap_];
    WideCharToMultiByte(CP_UTF8, 0, defaultFile, -1, g_ini_path, kPathCap_, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, userFile, -1, g_user_ini_path, kPathCap_, nullptr, nullptr);
    CheckHook_(IniWriteKeyUtf8(g_ini_path, "BankMaximize", "Enabled", "0"), "default checkbox write");
    CheckHook_(IniReadIntUtf8Layered("BankMaximize", "Enabled", 0) == 0, "first-use default unchecked");
    PersistBankSetting_("Enabled", 1);
    PersistBankSetting_("Ratio", 735);
    CheckHook_(IniReadIntUtf8Layered("BankMaximize", "Enabled", 0) == 1
        && IniReadIntUtf8Layered("BankMaximize", "Ratio", 1000) == 735,
        "saved user checkbox and ratio override factory defaults");
    PersistBankSetting_("Enabled", 0);
    CheckHook_(IniReadIntUtf8Layered("BankMaximize", "Enabled", 1) == 0
        && IniReadIntUtf8Layered("BankMaximize", "Ratio", 1000) == 735,
        "unchecking persists without resetting ratio");
    delete[] g_ini_path; delete[] g_user_ini_path;
    g_ini_path = savedDefault; g_user_ini_path = savedUser;
    DeleteFileW(defaultFile); DeleteFileW(userFile);
    delete[] tempDir; delete[] defaultFile; delete[] userFile;
    std::puts("PASS bank-max user INI persistence and default/user precedence");
}
// ====== Warehouse native adapter: real-patcher ABI + offline end-to-end ======
// Mock bodies simulate the real native sequence (stream open -> close inside
// the save/load call) so the production orchestration runs unmodified.
static const char* g_wh_test_path_ = nullptr;
static const char* g_wh_test_mode_ = nullptr;
static void* __fastcall MockWhStream_(int self, int pad, const char* path, const char* mode);
static void __fastcall MockWhClose_(int self);
void RunWarehouseHookTests_();
static int g_wh_calls_ = 0;

static __declspec(noinline) int __fastcall MockWhSave_(int self, int n1, int n2,
    int n3, int n4, int n5, int n6)
{
    ++g_wh_calls_;
    (void)self; (void)n1; (void)n2; (void)n3; (void)n4; (void)n5; (void)n6;
    MockWhStream_(0x1234, 0, g_wh_test_path_, g_wh_test_mode_);
    MockWhClose_(0x1234);
    return 1;
}
static __declspec(noinline) int __fastcall MockWhLoad_(int self, int n1, int n2,
    int n3, int n4)
{
    ++g_wh_calls_;
    (void)self; (void)n1; (void)n2; (void)n3; (void)n4;
    MockWhStream_(0x5678, 0, g_wh_test_path_, g_wh_test_mode_);
    MockWhClose_(0x5678);
    return 1;
}
static __declspec(noinline) void* __fastcall MockWhStream_(int self, int pad,
    const char* path, const char* mode)
{
    ++g_wh_calls_;
    (void)pad; (void)path; (void)mode;
    return reinterpret_cast<void*>(self);
}
static __declspec(noinline) void __fastcall MockWhClose_(int self)
{
    ++g_wh_calls_;
    (void)self;
}
static __declspec(noinline) int __fastcall MockWhStart_(int self)
{
    ++g_wh_calls_;
    return self ^ 0x5151;
}

static std::vector<std::uint8_t> WhBuildGzip_(const void* data, unsigned size)
{
    std::vector<std::uint8_t> out(size + size / 2 + 128);
    z_stream stream{};
    bool ok = deflateInit2(&stream, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    CheckHook_(ok, "deflateInit2 raw");
    stream.next_in = const_cast<Bytef*>(static_cast<const Bytef*>(data));
    stream.avail_in = size;
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    CheckHook_(deflate(&stream, Z_FINISH) == Z_STREAM_END, "deflate finish");
    deflateEnd(&stream);
    out.resize(stream.total_out);
    std::vector<std::uint8_t> file = { 0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0 };
    file.insert(file.end(), out.begin(), out.end());
    const uLong crc = crc32(0L, Z_NULL, 0);
    const uLong crcValue = crc32(crc, static_cast<const Bytef*>(data), size);
    for (int i = 0; i < 4; ++i) file.push_back(
        static_cast<std::uint8_t>((crcValue >> (8 * i)) & 0xFF));
    const uLong isize = size;
    for (int i = 0; i < 4; ++i) file.push_back(
        static_cast<std::uint8_t>((isize >> (8 * i)) & 0xFF));
    return file;
}
static bool WhWriteWhole_(const char* path, const std::vector<std::uint8_t>& bytes)
{
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return ok;
}
static bool WhReadWhole_(const char* path, std::vector<std::uint8_t>& bytes)
{
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    bytes.resize(static_cast<std::size_t>(size));
    const bool ok = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return ok;
}

static void CheckWarehouseLogEncoding_()
{
    wchar_t* tempDir = new wchar_t[kPathCap_ / 2];
    wchar_t* tempLog = new wchar_t[kPathCap_ / 2];
    CheckHook_(GetTempPathW(kPathCap_ / 2, tempDir) != 0
        && GetTempFileNameW(tempDir, L"whl", 0, tempLog) != 0,
        "UTF-8 log temp path");

    const wchar_t sourcePath[] = L"D:\\测试\\warehouse.GM1";
    char ansiPath[128] = {};
    CheckHook_(WideCharToMultiByte(CP_ACP, 0, sourcePath, -1, ansiPath,
        sizeof(ansiPath), nullptr, nullptr) != 0, "encode ANSI fixture path");
    char* utf8Path = WhPathUtf8ForLog_(ansiPath);
    CheckHook_(utf8Path != nullptr
        && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path,
            -1, nullptr, 0) > 0,
        "ANSI path becomes valid UTF-8 before logging");

    wchar_t* savedLogPath = g_log_path_w;
    const bool savedDisableLog = g_disable_log;
    const int savedLogLevel = g_log_level;
    g_log_path_w = tempLog;
    g_disable_log = false;
    g_log_level = LOG_TRACE;
    WriteLogLv(LOG_INFO, "仓库日志编码回归 path=%s", utf8Path ? utf8Path : "?");
    g_log_path_w = savedLogPath;
    g_disable_log = savedDisableLog;
    g_log_level = savedLogLevel;

    HANDLE file = CreateFileW(tempLog, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CheckHook_(file != INVALID_HANDLE_VALUE, "open UTF-8 log fixture");
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        CheckHook_(GetFileSizeEx(file, &size) && size.QuadPart >= 3,
            "UTF-8 log has contents");
        std::vector<char> bytes(static_cast<std::size_t>(size.QuadPart) + 1, 0);
        DWORD read = 0;
        CheckHook_(ReadFile(file, bytes.data(), static_cast<DWORD>(size.QuadPart),
            &read, nullptr) && read == static_cast<DWORD>(size.QuadPart),
            "read UTF-8 log fixture");
        CloseHandle(file);
        CheckHook_(static_cast<unsigned char>(bytes[0]) == 0xEF
            && static_cast<unsigned char>(bytes[1]) == 0xBB
            && static_cast<unsigned char>(bytes[2]) == 0xBF,
            "log starts with UTF-8 BOM");
        CheckHook_(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            bytes.data() + 3, static_cast<int>(read - 3), nullptr, 0) > 0,
            "complete log payload is strict UTF-8");
        CheckHook_(utf8Path && std::strstr(bytes.data() + 3, utf8Path) != nullptr,
            "UTF-8 log preserves converted Chinese path");
    }
    delete[] utf8Path;
    DeleteFileW(tempLog);
    delete[] tempLog;
    delete[] tempDir;
}

void RunWarehouseHookTests_()
{
    CheckWarehouseLogEncoding_();

    // 1) Chained SaveGame-thread entry: replicate production install order
    //    (TrueRandom first, warehouse second) and verify the context peek.
    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockWhStart_), SPLICE_,
        EXTENDED_, THISCALL_, reinterpret_cast<void*>(&OnStartGame_)) != nullptr,
        "install production StartGame first");
    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockWhStart_), SPLICE_,
        EXTENDED_, THISCALL_, reinterpret_cast<void*>(&OnWhStartGame_)) != nullptr,
        "chained warehouse StartGame hook accepted");
    void* startSelf = std::calloc(1, 0x400);
    CheckHook_(startSelf != nullptr, "warehouse start self storage");
    WarehouseTestReset_();
    warehouse::State* st = WarehouseTestState_();
    st->heroes[9].push_back({ 3, 7 });
    ObserveScenarioContext_(kScenarioNewGame_);
    const int selfValue = static_cast<int>(reinterpret_cast<intptr_t>(startSelf));
    CheckHook_(MockWhStart_(selfValue)
        == (selfValue ^ 0x5151), "start hook preserves original result");
    CheckHook_(st->heroes[9].empty() && st->mutationEpoch == 1,
        "new-game context clears warehouse exactly once");
    CheckHook_(ConsumeScenarioContext_() == kScenarioUnknown_,
        "chained peek leaves context consumption to TrueRandom");
    st->heroes[9].push_back({ 4, 2 });
    ObserveScenarioContext_(kScenarioLoad_);
    CheckHook_(MockWhStart_(selfValue)
        == (selfValue ^ 0x5151), "start hook result on load entry");
    CheckHook_(!st->heroes[9].empty() && st->heroes[9][0].artifactId == 4
        && st->heroes[9][0].subtype == 2, "load context preserves warehouse");
    std::free(startSelf);

    // 2) Save -> load end-to-end through production hooks with real files.
    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockWhSave_), SPLICE_,
        EXTENDED_, THISCALL_, reinterpret_cast<void*>(&OnSaveGame_)) != nullptr,
        "install production SaveGame callback");
    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockWhLoad_), SPLICE_,
        EXTENDED_, THISCALL_, reinterpret_cast<void*>(&OnLoadGame_)) != nullptr,
        "install production LoadGame callback");
    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockWhStream_), SPLICE_,
        EXTENDED_, THISCALL_, reinterpret_cast<void*>(&OnStreamOpen_)) != nullptr,
        "install production stream-open callback");
    CheckHook_(_PI->WriteHiHook(reinterpret_cast<UINT32>(&MockWhClose_), SPLICE_,
        EXTENDED_, THISCALL_, reinterpret_cast<void*>(&OnStreamClose_)) != nullptr,
        "install production stream-close callback");

    wchar_t* tempDir = new wchar_t[kPathCap_ / 2];
    wchar_t* tempFile = new wchar_t[kPathCap_ / 2];
    CheckHook_(GetTempPathW(kPathCap_ / 2, tempDir) != 0
        && GetTempFileNameW(tempDir, L"h3w", 0, tempFile) != 0, "warehouse temp file");
    char* nativePath = new char[kPathCap_];
    WideCharToMultiByte(CP_UTF8, 0, tempFile, -1, nativePath, kPathCap_, nullptr, nullptr);
    g_wh_test_path_ = nativePath;

    const char nativePayload[] = "warehouse native save fixture for offline ABI checks";
    std::vector<std::uint8_t> native = WhBuildGzip_(nativePayload, sizeof(nativePayload) - 1);
    CheckHook_(WhWriteWhole_(nativePath, native), "write native save fixture");
    const std::size_t nativeSize = native.size();

    WarehouseTestReset_();
    st->heroes[9].push_back({ 21, 5 });
    g_wh_test_mode_ = "wb";
    g_wh_calls_ = 0;
    unsigned esp_before = 0, esp_after = 0;
    __asm { mov esp_before, esp }
    const int saveResult = MockWhSave_(1, 2, 3, 4, 5, 6, 7);
    __asm { mov esp_after, esp }
    CheckHook_(saveResult == 1, "save hook returns original result");
    CheckHook_(g_wh_calls_ == 3, "save opens/closes stream exactly once");
    CheckHook_(esp_before == esp_after, "save hook preserves ESP");
    std::vector<std::uint8_t> saved;
    CheckHook_(WhReadWhole_(nativePath, saved), "read post-save file");
    CheckHook_(saved.size() > nativeSize
        && std::memcmp(saved.data(), native.data(), nativeSize) == 0,
        "native prefix preserved byte-for-byte");
    CheckHook_(saved.size() > 4
        && std::memcmp(saved.data() + nativeSize, "H3WHBGN1", 8) == 0,
        "v1 attachment appended directly after native boundary");
    CheckHook_(WarehouseTestCapturedPath_() == nullptr,
        "capture window closed and cleaned after save");
    CheckHook_(WarehouseTestCaptureClosed_() == 1, "close observed on captured stream");

    WarehouseTestReset_();
    g_wh_test_mode_ = "rb";
    const int loadResult = MockWhLoad_(9, 1, 2, 3, 4);
    CheckHook_(loadResult == 1, "load hook returns original result");
    CheckHook_(st->heroes[9].size() == 1 && st->heroes[9][0].artifactId == 21
        && st->heroes[9][0].subtype == 5 && st->mutationEpoch == 1,
        "attachment replaced warehouse state on load");

    // 3) Damaged footer -> lock; further saves must refuse and keep the file.
    CheckHook_(WhReadWhole_(nativePath, saved), "re-read for damage");
    saved[saved.size() - 3] ^= 0x01;
    CheckHook_(WhWriteWhole_(nativePath, saved), "write damaged attachment");
    WarehouseTestReset_();
    MockWhLoad_(9, 1, 2, 3, 4);
    CheckHook_(st->heroes[9].empty() && WarehouseTestLocked_() == 1,
        "damaged attachment locks and keeps state");
    std::vector<std::uint8_t> damaged;
    CheckHook_(WhReadWhole_(nativePath, damaged), "read damaged file size");
    const std::size_t sizeAfterDamage = damaged.size();
    st->heroes[3].push_back({ 8, 8 });
    MockWhSave_(1, 2, 3, 4, 5, 6, 7);
    std::vector<std::uint8_t> afterLocked;
    CheckHook_(WhReadWhole_(nativePath, afterLocked), "read after locked save");
    CheckHook_(afterLocked.size() == sizeAfterDamage, "locked save leaves file unchanged");

    g_wh_test_path_ = nullptr;
    g_wh_test_mode_ = nullptr;
    delete[] nativePath;
    delete[] tempDir;
    DeleteFileW(tempFile);
    delete[] tempFile;
    std::printf("PASS warehouse hooks: chained start context, save/load results and ESP, atomic attachment append via mocks, damaged-attachment lock\n");
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

#include "warehouse_ui_abi.inc.cpp"

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
    RunWarehouseHookTests_();
    RunWarehouseUiLoAbiTests_();
    DeleteCriticalSection(&g_random_audit_lock_);
    std::printf("PASS: installed patcher, production callbacks, 10000 RNG results/seeds, no-draw, SEH once, 1000 full CRT draws, 1000 one-register calls/ESP, constructor return, chain dedup/change\n");
}
