// ========== HdScenarioUi.inc.cpp ==========
// 选图对话框界面（IScenarioUi 的 HD 原生实现）。
// round-21 界面与功能分离：本文件自 TrueRandom.inc.cpp（真随机复选框）与
// BankMaximize.inc.cpp（宝屋最大化 复选框/滑块/百分比）**原样迁入**控件与
// 命中代码，外观与交互逐像素不变；模型与持久化（g_true_random /
// g_bank_maximize / g_bank_ratio + PersistTrueRandom_ / PersistBankSetting_）
// 仍在功能文件里，本文件只做控件与命中。钩子层（TrueRandom 的
// OnScenarioCtor_/OnScenarioProc_）只经 g_uiScenario 调用本实现。
// 包含顺序：必须在 TrueRandom.inc.cpp 与 BankMaximize.inc.cpp 之后
//（用到它们的持久化函数，以及 SmallFontHeight_/FontTextHeight_）。

#include "UiPort.hpp"
#include "BankMaximizeCore.hpp"

// ---- 控件 id 与素材常量（迁自 TrueRandom/BankMaximize）----
// id 130 是右上区域下方的「随机图」按钮 (414,105) 200x20，仅作参照。
// 「显示可选场景」在它正上方一行，id 128 同行 (414,81)。
// 文字区：显示在 id=131 的空白 hitbox 上方，最终单独重画以保证层级。
static const int kLabelGap_ = 6;
static const int kLabelW_    = 90;
static const int kCheckId_    = 0x7D01;
static const int kLabelId_    = 0x7D02;
static const int kLabelHitId_ = 0x7D03;
static const int kCheckW_     = 32;
static const int kCheckH_     = 24;
// 游戏界面按 GBK 画字。源码是 UTF-8，这里存转好的字节。
static const char kCheckHint_[] = "\xBF\xAA\xBE\xD6\xD5\xE6\xCB\xE6\xBB\xFA";
static const int kBankCheckId_ = 0x7D10;
static const int kBankLabelId_ = 0x7D11;
static const int kBankHitId_ = 0x7D12;
static const int kBankSliderId_ = 0x7D13;
static const int kBankPercentId_ = 0x7D14;
static const int kBankPercentBgId_ = 0x7D15;
static const char kBankLabel_[] = "\xB1\xA6\xCE\xDD\xD7\xEE\xB4\xF3\xBB\xAF";
static H3SelectScenarioDialog* s_bank_drag_dlg_ = nullptr;
static bankmax::SliderGesture s_bank_gesture_;
static bool s_bank_controls_ready_ = false;
static void ResetBankDrag_() { s_bank_drag_dlg_ = nullptr; s_bank_gesture_.Reset(); }
static void ResetBankDialog_() { ResetBankDrag_(); s_bank_controls_ready_ = false; }

static bool BankOwnsItem_(H3BaseDlg* dlg, H3DlgItem* item) {
    if (!dlg || !item) return false;
    for (H3DlgItem* owned : dlg->GetList()) if (owned == item) return true;
    return false;
}

// HD「其它选项」用 ChkBlue.def 的静态图，帧 0 空、帧 1 勾。
static void ApplyCheckFrame_(H3DlgDef* box)
{
    if (!box) return;
    box->SetFrame(g_true_random ? 1 : 0);
    box->Draw();
    box->ParentRedraw();
}

static bool PointInside_(H3DlgItem* item, int x, int y)
{
    if (!item) return false;
    return x >= item->GetAbsoluteX()
        && x < item->GetAbsoluteX() + item->GetWidth()
        && y >= item->GetAbsoluteY()
        && y < item->GetAbsoluteY() + item->GetHeight();
}

// Debug 只保留布局锚点、模式按钮和自有控件；Trace 才展开完整清单。
// 两个阶段仍可对照 HD 重排；提示按 GBK 十六进制记录。
static void LogInventory_(H3SelectScenarioDialog* dlg, const char* phase)
{
    if (!dlg || g_disable_log || g_log_level > LOG_DEBUG) return;
    H3DlgItem** const items = dlg->GetList().begin();
    const int count = static_cast<int>(dlg->GetList().Count());
    const bool complete = g_log_level == LOG_TRACE;
    LogDebug("真随机: [%s] 控件共 %d 项，输出=%s", phase, count,
        complete ? "完整(trace)" : "关键(debug)");
    for (int i = 0; i < count; ++i) {
        H3DlgItem* item = items[i];
        if (!item) continue;
        const int id = item->GetID();
        const bool key = (id >= 128 && id <= 131) || id == 186 || id == 335
            || id == kCheckId_ || id == kLabelId_ || id == kLabelHitId_;
        if (!complete && !key) continue;
        const UINT32 vt = *reinterpret_cast<UINT32*>(item);
        const char* asset = "?";
        if (vt == H3DlgDef::VTABLE && item->Cast<H3DlgDef>()->GetDef())
            asset = item->Cast<H3DlgDef>()->GetDef()->GetName();
        else if (vt == H3DlgDefButton::VTABLE && item->Cast<H3DlgDefButton>()->GetDef())
            asset = item->Cast<H3DlgDefButton>()->GetDef()->GetName();
        char hintHex[64] = "(无)";
        const UINT32 hintPtr = *reinterpret_cast<UINT32*>(
            reinterpret_cast<BYTE*>(item) + 0x20); // H3DlgItem::hint
        if (hintPtr) {
            hintHex[0] = '\0';
            const BYTE* hint = reinterpret_cast<const BYTE*>(hintPtr);
            for (int j = 0; j < 16 && hint[j]; ++j)
                _snprintf(hintHex + j * 3, sizeof(hintHex) - j * 3,
                    "%02X ", hint[j]);
        }
        auto log = complete ? &LogTrace : &LogDebug;
        log("真随机: [%s] id=%d (%d,%d) %dx%d vt=0x%08X %s 提示=%s",
            phase, item->GetID(), item->GetX(), item->GetY(),
            item->GetWidth(), item->GetHeight(), vt, asset, hintHex);
    }
}


// 已经记过清单的对话框。指针变了就是新开的一次选图界面。
static H3SelectScenarioDialog* s_seen_dlg_ = nullptr;
// H3DlgText::Draw 只改绘图缓冲，不保证当前窗口立即刷新；保存指针，
// 在该对话框每次消息处理结束后再画一次并 Refresh，避免后续重绘抹掉文字。
static H3DlgText* s_label_ = nullptr;

static void RefreshTrueRandomLabel_(H3SelectScenarioDialog* dlg)
{
    if (!dlg || !s_label_) return;
    bool owned = false;
    for (H3DlgItem* item : dlg->GetList()) if (item == s_label_) { owned = true; break; }
    if (!owned) { s_label_ = nullptr; return; }
    if (s_label_->GetID() != kLabelId_) return;
    s_label_->ShowActivate();
    // Draw 写入当前画布，Refresh 再把文字区域刷到窗口；顺序不能反。
    s_label_->Draw();
    s_label_->Refresh();
}

// 对话框收到第一条消息时才放：构造后 HD 还会重排，那时读到的
// 控件位置才是最终位置。id 128 与「显示可选场景」按钮同行，
// 在 (414,81) 200x20；勾选框放在它右边界外 4 像素、行内居中。
static const int kShowMapsRowId_ = 128;

static void PlaceTrueRandomCheckbox_(H3SelectScenarioDialog* dlg,
    const char* phase)
{
    if (!dlg || dlg->isCampaignMaybe || dlg->isLoadingMaybe) return;
    if (dlg->GetH3DlgItem(static_cast<UINT16>(kCheckId_))) return;
    H3DlgItem* row = dlg->GetH3DlgItem(static_cast<UINT16>(kShowMapsRowId_));
    if (!row) {
        LogError("真随机: [%s] 找不到显示可选场景所在行控件", phase);
        return;
    }
    const int x = row->GetX() + row->GetWidth() + 4;
    const int y = row->GetY() + (row->GetHeight() - kCheckH_) / 2;
    const int text_height = FontTextHeight_(kCheckH_, SmallFontHeight_());
    const int text_y = row->GetY() + (row->GetHeight() - text_height) / 2;
    H3DlgItem* next_row = dlg->GetH3DlgItem(130);
    const int bottom = next_row ? next_row->GetY() : row->GetY() + row->GetHeight() + 4;
    if (text_y < 0 || text_y + text_height > bottom
        || x + kCheckW_ + kLabelGap_ + kLabelW_ > dlg->GetWidth()) {
        LogWarn("真随机: 小字体行高 %d 超出选图标签可用布局，未添加勾选控件", SmallFontHeight_());
        return;
    }
    // 与 HD「其它选项」相同：ChkBlue.def 静态图，点击在 0x0C 消息里翻转。
    H3DlgDef* box = H3DlgDef::Create(x, y, kCheckW_, kCheckH_,
        kCheckId_, "ChkBlue.def",
        g_true_random ? 1 : 0, 0, FALSE, FALSE);
    if (!box) {
        LogError("真随机: 创建勾选框失败");
        return;
    }
    box->SetHint(kCheckHint_);
    dlg->AddItem(box);
    box->Draw();
    // 透明点击区先加（更高层），文字在上面绘制，避免被点击区遮挡。
    H3DlgTransparentItem* hit = H3DlgTransparentItem::Create(
        x + kCheckW_ + kLabelGap_, text_y, kLabelW_, text_height, kLabelHitId_);
    if (!hit) {
        LogError("真随机: 创建文字点击区失败");
        return;
    }
    hit->SetHint(kCheckHint_);
    dlg->AddItem(hit);

    H3DlgText* label = H3DlgText::Create(x + kCheckW_ + kLabelGap_, text_y,
        kLabelW_, text_height, kCheckHint_, "smalfont.fnt",
        1, kLabelId_, 4, 0);
    if (!label) {
        LogError("真随机: 创建文字失败");
        return;
    }
    label->SetHint(kCheckHint_);
    dlg->AddItem(label);
    s_label_ = label;
    // AddItem 会加载控件，但原生对话框后续重绘可能覆盖它；最后单独绘制文字。
    dlg->Redraw();
    RefreshTrueRandomLabel_(dlg);
    box->ParentRedraw();
    RefreshTrueRandomLabel_(dlg);
    // 一次开框只留一条汇总；细节排查用 [Logging] MinLevel=debug 配合清单。
    LogInfo("真随机: [%s] 已加复选框 TrueRandom=%d（显示行 (%d,%d) %dx%d，"
        "勾选框 (%d,%d)，文字 visible=%d active=%d）",
        phase, g_true_random, row->GetX(), row->GetY(),
        row->GetWidth(), row->GetHeight(), x, y,
        label->IsVisible(), label->IsActive());
}

// ---- 宝屋最大化控件（迁自 BankMaximize.inc.cpp）----

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

// ---- IScenarioUi 的 HD 原生实现：钩子层只认这里 ----

class HdScenarioUi final : public IScenarioUi {
public:
    void DialogCreated(H3SelectScenarioDialog* dlg, int mode) override
    {
        s_seen_dlg_ = nullptr;
        s_label_ = nullptr;
        ResetBankDialog_();
        // 构造刚完，HD 的重排还没跑，这里只留一份清单做对照。
        if (mode == 0) LogInventory_(dlg, "构造");
    }

    bool DialogMessage(H3SelectScenarioDialog* dlg, H3Msg* msg) override
    {
        if (HandleBankControls_(dlg, msg)) return true;
        if (msg && dlg
            && msg->command == eMsgCommand::ITEM_COMMAND
            && msg->subtype == eMsgSubtype::LBUTTON_DOWN)
        {
            H3DlgItem* clicked = dlg->GetH3DlgItem(
                static_cast<UINT16>(msg->itemId));
            const bool ours = clicked
                && (clicked->GetID() == kCheckId_
                    || clicked->GetID() == kLabelHitId_);
            const bool over_check = PointInside_(dlg->GetH3DlgItem(
                static_cast<UINT16>(kCheckId_)), msg->GetX(), msg->GetY());
            const bool over_label = PointInside_(dlg->GetH3DlgItem(
                static_cast<UINT16>(kLabelHitId_)), msg->GetX(), msg->GetY());
            if (ours || over_check || over_label) {
                g_true_random = g_true_random ? 0 : 1;
                PersistTrueRandom_();
                ApplyCheckFrame_(dlg->GetDef(static_cast<UINT16>(kCheckId_)));
                LogInfo("真随机: 勾选改为 %d，消息 id=%d",
                    g_true_random, msg->itemId);
                return true;
            }
        }
        return false;
    }

    void DialogMessageDone(H3SelectScenarioDialog* dlg, int result) override
    {
        if (result == 2) { ResetBankDialog_(); s_label_ = nullptr; return; }
        RefreshTrueRandomLabel_(dlg);
        // 原函数处理完这条消息后再动控件列表，避免在它的遍历中途插入。
        if (dlg && dlg != s_seen_dlg_) {
            s_seen_dlg_ = dlg;
            LogInventory_(dlg, "首消息");
            PlaceTrueRandomCheckbox_(dlg, "首消息");
            PlaceBankControls_(dlg);
        }
        RefreshBankControls_(dlg);
    }
};

static HdScenarioUi g_hdScenarioUi;
