// Reward-only postprocessing. Included after the existing RNG/UI helpers.
// round-21 界面与功能分离：宝屋控件（复选框/滑块/百分比）与命中手势接线
// 已迁至 HdScenarioUi.inc.cpp；本文件保留功能层——奖励后处理、拒绝采样、
// 模型持久化（PersistBankSetting_ 写 g_bank_maximize / g_bank_ratio）。
#include "BankMaximizeCore.hpp"

static const int GUARD_BANK_REWARDS = GuardRegisterHook_("BankMaxRewards");

static_assert(offsetof(H3MapItem, objectType) == 0x1E, "SoD map object type");
static_assert(offsetof(H3MapItem, objectSubtype) == 0x22, "SoD map object subtype");
static_assert(offsetof(H3CreatureBank, resources) == 0x38, "Reward follows guardians");
static_assert(offsetof(H3CreatureBank, artifacts) == 0x5C, "Game artifact vector");
static_assert(offsetof(H3CreatureBankState, artifactTypeCounts) == 0x5B, "Artifact classes");
static_assert(offsetof(H3Main, randomArtifacts) == 0x4E224, "Artifact working pool");
static_assert(offsetof(H3Main, artifactsAllowed) == 0x4E2B4, "Map artifact bans");

static void PersistBankSetting_(const char* key, int value) {
    char text[16];
    _snprintf(text, sizeof(text), "%d", value);
    if (!IniWriteKeyUtf8(g_user_ini_path, "BankMaximize", key, text))
        LogError("宝屋最大化: 保存 %s 失败", key);
}

// Rejection sampling, fail-closed: never consume the game's RNG.
static int BankRoll_(int count) {
    if (count <= 0 || count > 32768) return -1;
    const int limit = 32768 - 32768 % count;
    for (int retry = 0; retry < 32; ++retry) {
        int value = NextTrue15_();
        if (value < 0) return -1;
        if (value < limit) return value % count;
    }
    return -1;
}
static int BankType_(const H3MapItem& item) {
    switch (item.objectType) {
    case 16: return item.objectSubtype >= 0 && item.objectSubtype <= 6 ? item.objectSubtype : -1;
    case 85: return 7;
    case 24: return 8;
    case 84: return 9;
    case 25: return 10;
    default: return -1;
    }
}
static bool ReplaceBankReward_(H3Main* main, H3CreatureBank& bank, int type) {
    // Unlike the API's static address operand at 0x47A3C2, this is the
    // runtime pointer used by the native SetupBank generator.
    H3CreatureBankSetup* table = *reinterpret_cast<H3CreatureBankSetup**>(0x67029C);
    H3ArtifactSetup* artifacts = H3ArtifactSetup::Get();
    if (!table || !artifacts || type < 0 || type > 10) return false;
    const H3CreatureBankState& state = table[type].states[3];
    for (int amount : *reinterpret_cast<const int(*)[7]>(&state.resources)) if (amount < 0) return false;
    signed char used[144];
    memcpy(used, main->randomArtifacts, sizeof(used));
    H3Vector<INT32> rewards;
    int total = 0;
    // Native SetupBank generates Relic, Major, Minor, Treasure, in that order.
    for (int cls = 3; cls >= 0; --cls) {
        const int count = state.artifactTypeCounts[cls];
        if (count < 0 || count > 16 || (total += count) > 64) return false;
        for (int n = 0; n < count; ++n) {
            int id = bankmax::PickArtifact(artifacts, main->artifactsAllowed, used, 2 << cls, &BankRoll_);
            if (id < 0 || !rewards.Add(id)) return false;
        }
    }
    BYTE guardians[sizeof(H3Army)];
    memcpy(guardians, &bank.guardians, sizeof(guardians));
    bankmax::CopyRewardFields(bank, state);
    bank.artifacts.swap(rewards);
    memcpy(main->randomArtifacts, used, sizeof(used));
    if (memcmp(guardians, &bank.guardians, sizeof(guardians)))
        LogError("宝屋最大化: 守军不变检查失败 type=%d", type);
    return true;
}
static void ApplyBankMaximize_(int ratio) {
    H3Main* main = H3Main::Get();
    if (!main || !main->mainSetup.mapitems || main->mainSetup.mapSize < 1
        || main->mainSetup.mapSize > 252 || main->mainSetup.hasUnderground > 1
        || main->creatureBanks.Count() > 4096) {
        LogWarn("宝屋最大化: 新档地图结构无效，跳过"); return;
    }
    BYTE* seen = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 4096));
    if (!seen) { LogWarn("宝屋最大化: 无法分配去重表，跳过"); return; }
    const DWORD start = GetTickCount();
    int totals[4] = {}, changed[4] = {}, skipped = 0;
    __try {
        const int count = main->mainSetup.mapSize * main->mainSetup.mapSize * (main->mainSetup.hasUnderground + 1);
        for (int index = 0; index < count; ++index) {
            H3MapItem& item = main->mainSetup.mapitems[index];
            if (!item.IsEntrance()) continue;
            int bankType = BankType_(item), group = -1;
            // Sea chests (objectType 82) are excluded by user decision: never
            // modify them, not even their artifact setup.
            if (bankType >= 0) group = 0;
            else if (item.objectType == 101) group = 1;
            else if (item.objectType == 79 && bankmax::ResourceLimit(item.objectSubtype)) group = 2;
            else if (item.objectType == 12 && item.campfire.resType <= 5) group = 3;
            if (group < 0) continue;
            if (group == 0) {
                unsigned id = item.creatureBank.id;
                if (id >= main->creatureBanks.Count() || item.creatureBank.taken) { ++skipped; continue; }
                if (seen[id]) continue;
                seen[id] = 1;
            }
            ++totals[group];
            const int draw = ratio >= 1000 ? 0 : BankRoll_(1000);
            if (draw < 0) { ++skipped; continue; }
            if (draw >= ratio) continue;
            if (group == 0) {
                if (!ReplaceBankReward_(main, main->creatureBanks[item.creatureBank.id], bankType)) { ++skipped; continue; }
            } else if (group == 1) item.setup = bankmax::ChestSetup(item.setup);
            else if (group == 2) item.setup = bankmax::ResourceSetup(item.setup, item.objectSubtype);
            else item.setup = bankmax::CampfireSetup(item.setup);
            ++changed[group];
        }
    } __finally { HeapFree(GetProcessHeap(), 0, seen); }
    LogInfo("宝屋最大化: 新档奖励后处理 比例=%d/1000 宝屋=%d/%d 宝箱=%d/%d "
        "资源=%d/%d 营火=%d/%d 跳过=%d 耗时=%lu毫秒（守军保持原样，海箱不处理）", ratio,
        changed[0], totals[0], changed[1], totals[1], changed[2], totals[2],
        changed[3], totals[3], skipped, GetTickCount() - start);
}
static void FinishBankMaximize_(bool newGame, unsigned result, int enabled, int ratio) {
    if (!bankmax::SuccessfulNewGame(newGame, result) || !enabled || ratio <= 0) return;
    __try { ApplyBankMaximize_(bankmax::ClampRatio(ratio)); }
    __except (GuardCrashFilter_(GUARD_BANK_REWARDS, GetExceptionInformation())) {}
}
