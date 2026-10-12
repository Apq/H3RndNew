// Reward-only postprocessing. Included after the existing RNG/UI helpers.
#include "BankMaximizeCore.hpp"

static const int kBankCheckId_ = 0x7D10;
static const int kBankLabelId_ = 0x7D11;
static const int kBankHitId_ = 0x7D12;
static const int kBankSliderId_ = 0x7D13;
static const int kBankPercentId_ = 0x7D14;
static const int kBankPercentBgId_ = 0x7D15;
static H3SelectScenarioDialog* s_bank_drag_dlg_ = nullptr;
static bankmax::SliderGesture s_bank_gesture_;
static bool s_bank_controls_ready_ = false;
static void ResetBankDrag_() { s_bank_drag_dlg_ = nullptr; s_bank_gesture_.Reset(); }
static void ResetBankDialog_() { ResetBankDrag_(); s_bank_controls_ready_ = false; }
static const int GUARD_BANK_REWARDS = GuardRegisterHook_("BankMaxRewards");
static const char kBankLabel_[] = "\xB1\xA6\xCE\xDD\xD7\xEE\xB4\xF3\xBB\xAF";

static_assert(offsetof(H3MapItem, objectType) == 0x1E, "SoD map object type");
static_assert(offsetof(H3MapItem, objectSubtype) == 0x22, "SoD map object subtype");
static_assert(offsetof(H3CreatureBank, resources) == 0x38, "Reward follows guardians");
static_assert(offsetof(H3CreatureBank, artifacts) == 0x5C, "Game artifact vector");
static_assert(offsetof(H3CreatureBankState, artifactTypeCounts) == 0x5B, "Artifact classes");
static_assert(offsetof(H3Main, randomArtifacts) == 0x4E224, "Artifact working pool");
static_assert(offsetof(H3Main, artifactsAllowed) == 0x4E2B4, "Map artifact bans");

static bool BankOwnsItem_(H3BaseDlg* dlg, H3DlgItem* item) {
    if (!dlg || !item) return false;
    for (H3DlgItem* owned : dlg->GetList()) if (owned == item) return true;
    return false;
}
static void PersistBankSetting_(const char* key, int value) {
    char text[16];
    _snprintf(text, sizeof(text), "%d", value);
    if (!IniWriteKeyUtf8(g_user_ini_path, "BankMaximize", key, text))
        LogError("宝屋最大化: 保存 %s 失败", key);
}
static void RefreshBankControls_(H3SelectScenarioDialog* dlg) {
    if (!dlg || !s_bank_controls_ready_) return;
    const bool visible = !dlg->isCampaignMaybe && !dlg->isLoadingMaybe;
    for (int id : {kBankCheckId_, kBankLabelId_, kBankHitId_, kBankSliderId_, kBankPercentBgId_, kBankPercentId_}) {
        H3DlgItem* item = dlg->GetH3DlgItem(static_cast<UINT16>(id));
        if (!BankOwnsItem_(dlg, item)) continue;
        if (!visible) { item->HideDeactivate(); continue; }
        item->ShowActivate();
        if (id == kBankCheckId_) static_cast<H3DlgDef*>(item)->SetFrame(g_bank_maximize ? 1 : 0);
        if (id == kBankPercentId_) {
            char text[16];
            _snprintf(text, sizeof(text), "%d.%d%%", g_bank_ratio / 10, g_bank_ratio % 10);
            static_cast<H3DlgText*>(item)->SetText(text);
        }
        if (id == kBankSliderId_) {
            H3LoadedPcx16* pcx = static_cast<H3DlgPcx16*>(item)->GetPcx();
            if (pcx) {
                const int width = item->GetWidth();
                const int knob = (width - 8) * g_bank_ratio / 1000;
                pcx->FillRectangle(0, 0, width, 16, 45, 40, 35);
                pcx->FillRectangle(4, 7, width - 8, 2, 110, 100, 70);
                pcx->FillRectangle(4, 7, knob, 2, 235, 205, 116);
                pcx->FillRectangle(knob, 2, 8, 12, 160, 125, 55);
                pcx->DrawFrame(knob, 2, 8, 12, 235, 205, 116);
            }
        }
        // Compose the complete image before presenting any rectangle. In
        // particular, never present the blank percentage background alone.
        if (id != kBankHitId_) item->Draw();
    }
    for (int id : {kBankCheckId_, kBankLabelId_, kBankSliderId_, kBankPercentId_}) {
        H3DlgItem* item = dlg->GetH3DlgItem(static_cast<UINT16>(id));
        if (visible && BankOwnsItem_(dlg, item)) item->Refresh();
    }
}
static H3DlgPcx16* CreateBankSlider_(int x, int y, int width) {
    H3LoadedPcx16* pcx = H3LoadedPcx16::Create("", width, 16);
    if (!pcx) return nullptr;
    H3DlgPcx16* item = H3DlgPcx16::Create(x, y, width, 16, kBankSliderId_, nullptr);
    if (!item) { pcx->Destroy(); return nullptr; }
    item->SetPcx(pcx);
    return item;
}
static H3DlgPcx16* CreateBankPercentBackground_(H3SelectScenarioDialog* dlg, int x, int y, int height) {
    H3LoadedPcx16* pcx = H3LoadedPcx16::Create("", 47, height);
    if (!pcx) return nullptr;
    H3WindowManager* wm = H3WindowManager::Get();
    H3LoadedPcx16* source = wm ? wm->GetDrawBuffer() : nullptr;
    const int absX = dlg->GetX() + x, absY = dlg->GetY() + y;
    if (source && absX >= 0 && absY >= 0 && absX + 47 <= source->width
        && absY + height <= source->height) pcx->CopyRegion(source, absX, absY);
    else pcx->FillRectangle(0, 0, 47, height, 45, 40, 35);
    H3DlgPcx16* item = H3DlgPcx16::Create(x, y, 47, height, kBankPercentBgId_, nullptr);
    if (!item) { pcx->Destroy(); return nullptr; }
    item->SetPcx(pcx);
    return item;
}
static void SetBankRatioFromMouse_(H3SelectScenarioDialog* dlg, H3DlgItem* slider, int mouseX) {
    if (!BankOwnsItem_(dlg, slider)) return;
    int ratio = bankmax::RatioFromPixel(mouseX - slider->GetAbsoluteX(), slider->GetWidth());
    if (ratio == g_bank_ratio) return;
    g_bank_ratio = ratio;
    PersistBankSetting_("Ratio", ratio);
    RefreshBankControls_(dlg);
}
static void PlaceBankControls_(H3SelectScenarioDialog* dlg) {
    if (!dlg || dlg->isCampaignMaybe || dlg->isLoadingMaybe
        || dlg->GetH3DlgItem(kBankCheckId_)) return;
    // id=100 is repeated. Locate the victory title by its native rectangle,
    // after HD's first-message rearrangement; do not take the first id=100.
    H3DlgItem* title = nullptr;
    for (H3DlgItem* item : dlg->GetList()) {
        if (item && item->GetID() == 100 && item->GetX() >= 410
            && item->GetY() >= 280 && item->GetY() <= 300
            && item->GetWidth() >= 200 && item->GetHeight() <= 30) {
            title = item; break;
        }
    }
    if (!title) { LogWarn("宝屋最大化: 未找到胜利条件标题，未添加控件"); return; }
    const bankmax::ControlsLayout layout(title->GetX());
    const int x = layout.checkX;
    const int y = title->GetY() - 4;
    const int textHeight = FontTextHeight_(24, SmallFontHeight_());
    const int sliderX = layout.sliderX;
    const int sliderW = bankmax::ControlsLayout::sliderWidth;
    if (!layout.Fits(dlg->GetWidth(), textHeight)) {
        LogWarn("宝屋最大化: 胜利条件标题右侧空间不足，未添加控件"); return;
    }
    H3DlgDef* box = H3DlgDef::Create(x, y, 32, 24, kBankCheckId_,
        "ChkBlue.def", g_bank_maximize ? 1 : 0, 0, FALSE, FALSE);
    H3DlgText* label = H3DlgText::Create(x + 35, y, 60, textHeight,
        kBankLabel_, "smalfont.fnt", 1, kBankLabelId_, 4, 0);
    H3DlgTransparentItem* hit = H3DlgTransparentItem::Create(x + 35, y, 60, textHeight, kBankHitId_);
    H3DlgPcx16* slider = CreateBankSlider_(sliderX, y + 4, sliderW);
    H3DlgPcx16* percentBg = CreateBankPercentBackground_(dlg, layout.percentX, y, textHeight);
    H3DlgText* percentage = H3DlgText::Create(sliderX + sliderW + 3, y, 47, textHeight,
        "100.0%", "smalfont.fnt", 1, kBankPercentId_, 4, 0);
    if (!box || !label || !hit || !slider || !percentBg || !percentage) {
        // Transfer successful partial allocations to the live dialog so its
        // normal destructor frees them using each control's native vtable.
        for (H3DlgItem* item : {static_cast<H3DlgItem*>(box), static_cast<H3DlgItem*>(label),
            static_cast<H3DlgItem*>(hit), static_cast<H3DlgItem*>(slider),
            static_cast<H3DlgItem*>(percentBg), static_cast<H3DlgItem*>(percentage)}) {
            if (item) { dlg->AddItem(item); item->HideDeactivate(); }
        }
        LogError("宝屋最大化: 创建控件失败"); return;
    }
    // Reserve the left 90 pixels for the native title, leaving the separately
    // painted victory body unchanged.
    title->SetWidth(90);
    dlg->AddItem(box); dlg->AddItem(hit); dlg->AddItem(label);
    dlg->AddItem(slider); dlg->AddItem(percentBg); dlg->AddItem(percentage);
    s_bank_controls_ready_ = true;
    dlg->Redraw();
    RefreshBankControls_(dlg);
    LogInfo("宝屋最大化: 已添加控件 (%d,%d) 比例=%d/1000 勾选=%d", x, y, g_bank_ratio, g_bank_maximize);
}
static bool HandleBankControls_(H3SelectScenarioDialog* dlg, H3Msg* msg) {
    if (!dlg || !msg || !s_bank_controls_ready_ || dlg->isCampaignMaybe || dlg->isLoadingMaybe) {
        ResetBankDrag_(); return false;
    }
    if (s_bank_drag_dlg_ != dlg) { ResetBankDrag_(); s_bank_drag_dlg_ = dlg; }
    H3DlgItem* slider = dlg->GetH3DlgItem(kBankSliderId_);
    if (!BankOwnsItem_(dlg, slider)) { ResetBankDrag_(); return false; }
    bankmax::SliderEvent event = bankmax::SliderEvent::Other;
    if (msg->command == eMsgCommand::ITEM_COMMAND) {
        if (msg->subtype == eMsgSubtype::LBUTTON_DOWN) event = bankmax::SliderEvent::Down;
        else if (msg->subtype == eMsgSubtype::LBUTTON_CLICK) event = bankmax::SliderEvent::Click;
    } else if (msg->command == eMsgCommand::MOUSE_OVER) event = bankmax::SliderEvent::Move;
    else if (msg->command == eMsgCommand::LBUTTON_UP) event = bankmax::SliderEvent::Release;
    const bankmax::SliderAction action = s_bank_gesture_.Handle(event,
        PointInside_(slider, msg->GetX(), msg->GetY()),
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    if (action == bankmax::SliderAction::Update) {
        SetBankRatioFromMouse_(dlg, slider, msg->GetX()); return true;
    }
    if (action == bankmax::SliderAction::Consume) return true;
    if (msg->command != eMsgCommand::ITEM_COMMAND || msg->subtype != eMsgSubtype::LBUTTON_DOWN) return false;
    if (!PointInside_(dlg->GetH3DlgItem(kBankCheckId_), msg->GetX(), msg->GetY())
        && !PointInside_(dlg->GetH3DlgItem(kBankHitId_), msg->GetX(), msg->GetY())) return false;
    g_bank_maximize = !g_bank_maximize;
    PersistBankSetting_("Enabled", g_bank_maximize);
    RefreshBankControls_(dlg);
    LogInfo("宝屋最大化: 勾选=%d 比例=%d/1000", g_bank_maximize, g_bank_ratio);
    return true;
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
