// ========== 热键设置窗 ==========
// 随时可开的全局设置窗（默认 F11）：日志等级 + 全局真随机 + 热键修改。
// 键盘入口 0x4EC1C0：WndProc(0x4F8290) 把 WM_KEYDOWN(0x100)/WM_KEYUP(0x101)
// 交给它，bool __fastcall(hwnd, msg, wparam, lparam)；扫描码 = lparam>>16 & 0xFF，
// 与 H3 内部键码一致（F11=87、F12=88，H3API.hpp eVKey 表）。原版 F1/F4 帮助
// 就是在该函数里直接响应并打开界面的，模式同源。依据：H3Note 调研笔记。

static const UINT32 kKeyMsgProc_ = 0x4EC1C0;

// 输入管理器（DAT_00699530）：原函数入口检查非空且 +0x34==1 才处理消息，
// 弹窗沿用同一守卫，避免启动/切换期抢跑。
static bool InputMgrReady_()
{
    const UINT32 mgr = *reinterpret_cast<const UINT32*>(0x699530);
    return mgr != 0
        && *reinterpret_cast<const INT32*>(mgr + 0x34) == 1;
}

// ---- 状态 ----
static volatile LONG s_settings_dlg_running_ = 0; // 设置窗模态运行中
static volatile LONG s_listen_new_hotkey_ = 0;    // 等待玩家按新热键

// ---- 文本工具 ----

// 源码 UTF-8，游戏字体按 GBK 画字（开局模块已验证），运行时转换。
static int Utf8ToGbk_(const char* utf8, char* out, int out_size)
{
    if (!utf8 || !out || out_size <= 0) return 0;
    wchar_t wide[192];
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 192);
    if (wn <= 0) { out[0] = 0; return 0; }
    const int n = WideCharToMultiByte(936, 0, wide, -1, out, out_size,
        nullptr, nullptr);
    if (n <= 0) { out[0] = 0; return 0; }
    return n;
}

// 扫描码 -> 键名（ASCII 直接可用；中文名先产 UTF-8 再由调用方转 GBK）。
static void ScanToName_(int scan, char* out, int out_size)
{
    if (scan >= 0x3B && scan <= 0x44)
        _snprintf(out, out_size - 1, "F%d", scan - 0x3B + 1);
    else if (scan == 0x57) _snprintf(out, out_size - 1, "F11");
    else if (scan == 0x58) _snprintf(out, out_size - 1, "F12");
    else if (scan >= 0x1E && scan <= 0x26)
        _snprintf(out, out_size - 1, "%c", 'A' + scan - 0x1E);
    else if (scan >= 0x02 && scan <= 0x0A)
        _snprintf(out, out_size - 1, "%c", '1' + scan - 0x02);
    else if (scan == 0x0B) _snprintf(out, out_size - 1, "0");
    else if (scan == 0x39) _snprintf(out, out_size - 1, "空格");
    else if (scan == 0x1C) _snprintf(out, out_size - 1, "回车");
    else if (scan == 0x0F) _snprintf(out, out_size - 1, "Tab");
    else if (scan == 0x0E) _snprintf(out, out_size - 1, "退格");
    else if (scan == 1)    _snprintf(out, out_size - 1, "ESC");
    else _snprintf(out, out_size - 1, "键%d", scan);
    out[out_size - 1] = 0;
}

// ---- 持久化（写玩家层 user.ini，与开局勾选同机制）----

static void PersistTrueRandomFull_()
{
    char value[8] = {};
    _snprintf(value, sizeof(value) - 1, "%d", g_true_random_full ? 1 : 0);
    if (!IniWriteKeyUtf8(g_user_ini_path, "General", "TrueRandomFull", value))
        LogError("真随机: 写入 user.ini(TrueRandomFull) 失败");
}

static void PersistHotkey_()
{
    char value[8] = {};
    _snprintf(value, sizeof(value) - 1, "%d", g_settings_hotkey_scan);
    if (!IniWriteKeyUtf8(g_user_ini_path, "General", "SettingsHotkeyScan",
            value))
        LogError("真随机: 写入 user.ini(SettingsHotkeyScan) 失败");
}

// ---- 设置窗 ----

static const int kSdTitleId_     = 0x7E10;
static const int kSdCheckId_     = 0x7E11; // sysopchk 开关
static const int kSdCheckHitId_  = 0x7E12; // 标签点击区（文字控件兼任）
static const int kSdKeyHintId_   = 0x7E13; // 热键行「热键：」文字
static const int kSdKeyFrameId_  = 0x7E14; // 可隐藏的热键下沉底板
static const int kSdKeyNameId_   = 0x7E15; // 键名文字（SinkArea 下沉框内）
static const int kSdOkId_        = 0x7E16;
static const int kSdLogLabelId_  = 0x7E18; // 「日志等级：」文字
static const int kSdLogDdId_     = 0x7E19; // 日志等级下拉框收起态
static const int kSdLogDdBgId_   = 0x7E1A; // 收起框底板和三角箭头
static const int kSdLogArrowHitId_ = 0x7E1B;
static const int kSdLogShieldId_ = 0x7E1C; // 展开时接管空白区域点击
static const int kSdLogListBgId_ = 0x7E1D; // 展开列表底板和边框
static const int kSdLogItem0Id_  = 0x7E20; // 展开列表项 0..4（五级）
static const int kSdW_ = 340;
static const int kSdH_ = 190;

// 日志等级下拉：布局常量（收起框 / 展开列表）。行放在标题正下方，
// 展开列表向下覆盖复选框/热键行，全部落在窗口内，无需加高窗口。
static const int kSdLogRow_     = 44;    // 行 y（标签与收起框）
static const int kSdLogDdX_     = 120;   // 第一行收起框 x
static const int kSdLogDdW_     = 120;
static const int kSdLogDdH_     = 24;
static const int kSdLogItemH_   = 20;    // 展开列表单行高
static const int kSdLogLevelCount_ = 5;  // trace/debug/info/warn/error
static const int kSdLogListY_ = kSdLogRow_ + kSdLogDdH_ + 2;
static const int kSdLogListH_ = kSdLogLevelCount_ * kSdLogItemH_ + 4;

// 用游戏原生 16 位对话框图层绘制下拉框底板。这样底板是控件自身的
// PCX，不会写入 vShow 保存的底图，也不会在关闭设置窗后残留。
static H3DlgPcx16* CreateLogSurface_(INT32 x, INT32 y, INT32 w, INT32 h,
    INT32 id, bool arrow, bool visible)
{
    H3LoadedPcx16* pcx = H3LoadedPcx16::Create("", w, h);
    if (!pcx) return nullptr;
    pcx->FillRectangle(0, 0, w, h, 68, 44, 20);
    pcx->DrawFrame(0, 0, w, h, 210, 170, 72);
    if (arrow) {
        const int ax = w - 15;
        const int ay = (h - 4) / 2;
        pcx->FillRectangle(ax, ay, 7, 1, 235, 205, 116);
        pcx->FillRectangle(ax + 1, ay + 1, 5, 1, 235, 205, 116);
        pcx->FillRectangle(ax + 2, ay + 2, 3, 1, 235, 205, 116);
        pcx->FillRectangle(ax + 3, ay + 3, 1, 1, 235, 205, 116);
    }
    H3DlgPcx16* surface = H3DlgPcx16::Create(x, y, w, h, id, nullptr);
    if (!surface) {
        pcx->Destroy();
        return nullptr;
    }
    surface->SetPcx(pcx);
    if (!visible) surface->HideDeactivate();
    return surface;
}

static void PaintLogDropdownSurface_(H3DlgPcx16* surface, bool open)
{
    if (!surface || !surface->GetPcx()) return;
    H3LoadedPcx16* pcx = surface->GetPcx();
    const int w = surface->GetWidth();
    const int h = surface->GetHeight();
    pcx->FillRectangle(0, 0, w, h, open ? 104 : 68, open ? 70 : 44,
        open ? 28 : 20);
    pcx->DrawFrame(0, 0, w, h, open ? 232 : 210, open ? 184 : 170,
        open ? 76 : 72);
    const int ax = w - 15;
    const int ay = (h - 4) / 2;
    if (open) {
        pcx->FillRectangle(ax + 3, ay, 1, 1, 235, 205, 116);
        pcx->FillRectangle(ax + 2, ay + 1, 3, 1, 235, 205, 116);
        pcx->FillRectangle(ax + 1, ay + 2, 5, 1, 235, 205, 116);
        pcx->FillRectangle(ax, ay + 3, 7, 1, 235, 205, 116);
    } else {
        pcx->FillRectangle(ax, ay, 7, 1, 235, 205, 116);
        pcx->FillRectangle(ax + 1, ay + 1, 5, 1, 235, 205, 116);
        pcx->FillRectangle(ax + 2, ay + 2, 3, 1, 235, 205, 116);
        pcx->FillRectangle(ax + 3, ay + 3, 1, 1, 235, 205, 116);
    }
}

struct SettingsDlg_ final : public H3Dlg
{
    H3DlgDefButton* check_box_   = nullptr;
    H3DlgText*      key_name_    = nullptr;
    H3DlgPcx16*     log_dd_bg_ = nullptr;   // 底板含三角箭头
    H3DlgText*      log_dd_text_ = nullptr; // 收起态当前等级名
    H3DlgPcx16*     log_list_bg_ = nullptr; // 不透明列表浮层
    bool            log_dd_open_ = false;    // 下拉展开态

    SettingsDlg_() : H3Dlg(kSdW_, kSdH_) {}

    BOOL OnCreate() override
    {
        char gbk[160];
        H3DlgText* title = H3DlgText::Create(0, 12, kSdW_, 24,
            Utf8ToGbk_("真随机 设置", gbk, sizeof(gbk)) ? gbk : "",
            "smalfont.fnt", 1, kSdTitleId_, 5, 0);
        if (title) AddItem(title);

        // 日志等级行：放标题正下方，展开列表向下覆盖复选框/热键行，
        // 全部落在窗口内，窗口保持 190 高。当前等级名由框架绘制。
        H3DlgText* log_label = H3DlgText::Create(60, kSdLogRow_, 52, 24,
            Utf8ToGbk_("日志：", gbk, sizeof(gbk)) ? gbk : "",
            "smalfont.fnt", 5, kSdLogLabelId_, 4, 0);
        if (log_label) {
            char lv_hint_gbk[160];
            const bool lv_hint_ok = Utf8ToGbk_(
                "低于该等级的日志不写盘；修改立即生效并保存到 user.ini",
                lv_hint_gbk, sizeof(lv_hint_gbk)) != 0;
            if (lv_hint_ok) log_label->SetHint(lv_hint_gbk);
            AddItem(log_label);
        }
        log_dd_bg_ = CreateLogSurface_(kSdLogDdX_, kSdLogRow_,
            kSdLogDdW_, kSdLogDdH_, kSdLogDdBgId_, true, true);
        if (log_dd_bg_) {
            log_dd_bg_->ShowActivate();
            AddItem(log_dd_bg_);
        }
        log_dd_text_ = H3DlgText::Create(kSdLogDdX_ + 4, kSdLogRow_,
            kSdLogDdW_ - 24, kSdLogDdH_, "",
            "smalfont.fnt", 5, kSdLogDdId_, 5, 0);
        if (log_dd_text_) {
            char lv_hint_gbk[160];
            const bool lv_hint_ok = Utf8ToGbk_(
                "点击展开日志等级选项",
                lv_hint_gbk, sizeof(lv_hint_gbk)) != 0;
            if (lv_hint_ok) log_dd_text_->SetHint(lv_hint_gbk);
            AddItem(log_dd_text_);
            UpdateLogLevelText_();
        }

        H3DlgText* arrow_hit = H3DlgText::Create(
            kSdLogDdX_ + kSdLogDdW_ - 20, kSdLogRow_, 20, kSdLogDdH_,
            "", "smalfont.fnt", 5, kSdLogArrowHitId_, 5, 0);
        if (arrow_hit) AddItem(arrow_hit);

        // 确定按钮：closeDialog=TRUE，点击自动关窗；Enter 等效。
        H3DlgDefButton* ok = H3DlgDefButton::Create(138, 140, kSdOkId_,
            "iokay.def", 0, 1, TRUE, NH3VKey::H3VK_ENTER);
        if (ok) AddItem(ok);

        // 全局真随机：系统选项同款开关复选框（sysopchk.def，金色系）
        // + 可点标签。frame 0=关 1=开，点击自动翻转，不用手工重绘。
        static const char kFullHint_[] =
            "开启后游戏全程随机数改用系统级随机；"
            "关闭时仅新游戏建档期间替换";
        char hint_gbk[160];
        const bool hint_ok = Utf8ToGbk_(kFullHint_, hint_gbk,
            sizeof(hint_gbk)) != 0;
        check_box_ = H3DlgDefButton::Create(30, 84, kSdCheckId_,
            NH3Dlg::Assets::ON_OFF_CHECKBOX, g_true_random_full ? 1 : 0,
            1 - (g_true_random_full ? 1 : 0), FALSE, 0);
        if (check_box_) {
            if (hint_ok) check_box_->SetHint(hint_gbk);
            AddItem(check_box_);
        } else {
            LogError("真随机: 设置窗创建勾选框失败");
        }
        H3DlgText* full_label = H3DlgText::Create(68, 84, 112, 24,
            Utf8ToGbk_("全局真随机", gbk, sizeof(gbk)) ? gbk : "",
            "smalfont.fnt", 1, kSdCheckHitId_, 4, 0);
        if (full_label) {
            if (hint_ok) full_label->SetHint(hint_gbk);
            AddItem(full_label);
        }

        // 热键行：说明文字 + 原生文字/PCX 组合控件。
        H3DlgText* key_label = H3DlgText::Create(192, 84, 44, 24,
            Utf8ToGbk_("热键：", gbk, sizeof(gbk)) ? gbk : "",
            "smalfont.fnt", 5, kSdKeyHintId_, 4, 0);
        if (key_label) {
            char key_hint_gbk[160];
            const bool key_hint_ok = Utf8ToGbk_(
                "点击框内后按新键（ESC 取消）；F12 已被 SoD_SP 设置占用",
                key_hint_gbk, sizeof(key_hint_gbk)) != 0;
            if (key_hint_ok) key_label->SetHint(key_hint_gbk);
            AddItem(key_label);
        }
        // 下沉边缘画在独立底板上，隐藏热键时不会留下背景中的凹槽。
        if (H3LoadedPcx16* bg = GetBackgroundPcx()) {
            H3LoadedPcx16* pcx = H3LoadedPcx16::Create("", 70, 30);
            if (pcx) {
                pcx->CopyRegion(bg, 238, 80);
                pcx->SinkArea(0, 0, 70, 30);
                H3DlgPcx16* frame = H3DlgPcx16::Create(238, 80, 70, 30,
                    kSdKeyFrameId_, nullptr);
                if (frame) {
                    frame->SetPcx(pcx);
                    AddItem(frame);
                } else {
                    pcx->Destroy();
                }
            }
        }
        // OnCreate 在 vShow 保存底层画面之前运行，这里只设置文字，绝不绘制。
        char initial_key_gbk[64] = {};
        char initial_key_utf8[64];
        ScanToName_(g_settings_hotkey_scan, initial_key_utf8,
            sizeof(initial_key_utf8));
        Utf8ToGbk_(initial_key_utf8, initial_key_gbk, sizeof(initial_key_gbk));
        key_name_ = H3DlgText::Create(240, 84, 66, 24, initial_key_gbk,
            "smalfont.fnt", 5, kSdKeyNameId_, 5);
        if (key_name_) {
            char key_hint_gbk[160];
            const bool key_hint_ok = Utf8ToGbk_(
                "点击框内后按新键（ESC 取消）；F12 已被 SoD_SP 设置占用",
                key_hint_gbk, sizeof(key_hint_gbk)) != 0;
            if (key_hint_ok) key_name_->SetHint(key_hint_gbk);
            AddItem(key_name_);
        }
        H3DlgText* shield = H3DlgText::Create(0, 0, kSdW_, kSdH_,
            "", "smalfont.fnt", 5, kSdLogShieldId_, 5, 0);
        if (shield) {
            shield->HideDeactivate();
            AddItem(shield);
        }

        // 展开列表浮层：不透明深棕底 + 金色边框，默认隐藏。它先加入，
        // 后面的选项文字自然绘制在底板之上；整个浮层不会透出下方控件。
        log_list_bg_ = CreateLogSurface_(kSdLogDdX_, kSdLogListY_ - 2,
            kSdLogDdW_, kSdLogListH_, kSdLogListBgId_, false, false);
        if (log_list_bg_) AddItem(log_list_bg_);

        // 展开列表项：默认隐藏，展开时 Show、收起时 Hide（框架绘制，
        // 不碰背景纹理）。y 排在收起框下方；创建顺序在底板之后，
        // 展开时自然盖在底板之上。
        for (int i = 0; i < kSdLogLevelCount_; ++i) {
            char gbk[64];
            if (!Utf8ToGbk_(LogLevelDisplayName_(i), gbk, sizeof(gbk)))
                continue;
            H3DlgText* item = H3DlgText::Create(kSdLogDdX_ + 6,
                kSdLogListY_ + i * kSdLogItemH_,
                kSdLogDdW_ - 12, kSdLogItemH_, gbk,
                "smalfont.fnt", 5, kSdLogItem0Id_ + i, 4, 0);
            if (item) {
                item->HideDeactivate();
                AddItem(item);
            }
        }
        return TRUE;
    }

    // 日志等级名（中文显示名，UI 层；INI 仍写 trace/debug/…）。
    static const char* LogLevelDisplayName_(int level)
    {
        switch (level) {
        case LOG_TRACE: return "全部(trace)";
        case LOG_DEBUG: return "调试(debug)";
        case LOG_INFO:  return "信息(info)";
        case LOG_WARN:  return "警告(warn)";
        default:        return "错误(error)";
        }
    }

    void UpdateLogLevelText_()
    {
        if (!log_dd_text_) return;
        char gbk[64];
        if (!Utf8ToGbk_(LogLevelDisplayName_(g_log_level), gbk, sizeof(gbk)))
            return;
        log_dd_text_->SetText(gbk);
    }

    // 展开/收起：选项是常驻但默认隐藏的文字控件，交给框架绘制，
    // 不在背景纹理上留任何痕迹（背景是常驻图，画上去就擦不掉）。
    void SetLogDropdownOpen_(bool open)
    {
        log_dd_open_ = open;
        PaintLogDropdownSurface_(log_dd_bg_, open);
        if (H3DlgItem* shield = GetH3DlgItem(kSdLogShieldId_)) {
            if (open) shield->ShowActivate();
            else shield->HideDeactivate();
        }
        if (log_list_bg_) {
            H3LoadedPcx16* pcx = log_list_bg_->GetPcx();
            if (pcx) {
                for (int i = 0; i < kSdLogLevelCount_; ++i) {
                    const bool selected = i == g_log_level;
                    pcx->FillRectangle(2, 2 + i * kSdLogItemH_,
                        kSdLogDdW_ - 4, kSdLogItemH_,
                        selected ? 136 : 68, selected ? 88 : 42,
                        selected ? 24 : 18);
                    pcx->DrawFrame(2, 2 + i * kSdLogItemH_,
                        kSdLogDdW_ - 4, kSdLogItemH_,
                        selected ? 232 : 166, selected ? 184 : 112,
                        selected ? 76 : 40);
                }
            }
            if (open) log_list_bg_->ShowActivate();
            else log_list_bg_->HideDeactivate();
        }
        // 浮层期间停用并隐藏下方控件，避免标签透出及按下/松开穿透。
        const UINT16 covered[] = { kSdCheckId_, kSdCheckHitId_,
            kSdKeyHintId_, kSdKeyFrameId_, kSdKeyNameId_, kSdOkId_ };
        for (UINT16 id : covered) {
            if (H3DlgItem* item = GetH3DlgItem(id)) {
                if (open) item->HideDeactivate();
                else item->ShowActivate();
            }
        }
        for (int i = 0; i < kSdLogLevelCount_; ++i) {
            H3DlgText* item = GetText(
                static_cast<UINT16>(kSdLogItem0Id_ + i));
            if (!item) continue;
            if (open)
                item->ShowActivate();
            else
                item->HideDeactivate();
        }
        Redraw();
    }

    BOOL OnLeftClick(INT itemId, H3Msg& msg) override
    {
        // 展开期间优先按浮层坐标选项，不依赖下方控件的命中顺序。
        if (log_dd_open_) {
            const int x = msg.GetX() - GetX();
            const int y = msg.GetY() - GetY();
            if (x >= kSdLogDdX_ && x < kSdLogDdX_ + kSdLogDdW_
                && y >= kSdLogListY_
                && y < kSdLogListY_ + kSdLogLevelCount_ * kSdLogItemH_) {
                const int level = (y - kSdLogListY_) / kSdLogItemH_;
                g_log_level = level;
                const char* value = LogLevelName_(level);
                if (!IniWriteKeyUtf8(g_user_ini_path, "Logging", "MinLevel", value))
                    LogError("真随机: 写入 user.ini(MinLevel) 失败");
                UpdateLogLevelText_();
                LogInfo("真随机: 日志等级切换为 %s", value);
            }
            SetLogDropdownOpen_(false);
            return TRUE;
        }
        if (itemId == kSdLogDdId_ || itemId == kSdLogDdBgId_
            || itemId == kSdLogArrowHitId_) {
            InterlockedExchange(&s_listen_new_hotkey_, 0);
            RefreshKeyName_();
            SetLogDropdownOpen_(true);
            return TRUE;
        }
        if (itemId == kSdCheckId_ || itemId == kSdCheckHitId_) {
            LogRandomAudit_("全局切换前");
            g_true_random_full = g_true_random_full ? 0 : 1;
            PersistTrueRandomFull_();
            // DefButton 的 clickFrame 是“按住期间”的显示帧；把它设成与
            // 松开后的状态帧相同，按下→松开全程无视觉跳变。
            if (check_box_) {
                const int frame = g_true_random_full ? 1 : 0;
                check_box_->SetFrame(frame);
                check_box_->SetClickFrame(frame);
                check_box_->Draw();
                check_box_->ParentRedraw();
            }
            LogInfo("真随机: 全局模式切换为 %d（%s）", g_true_random_full,
                g_true_random_full ? "全程系统级随机" : "仅开局窗口");
            return TRUE;
        }
        if (itemId == kSdKeyNameId_) {
            InterlockedExchange(&s_listen_new_hotkey_, 1);
            RefreshKeyName_();
            LogInfo("真随机: 等待输入新热键");
            return TRUE;
        }
        return FALSE; // OK 按钮由 closeDialog 默认逻辑关窗
    }

    BOOL OnLeftClickOutside() override
    {
        if (!log_dd_open_) return FALSE;
        SetLogDropdownOpen_(false);
        return TRUE;
    }

    BOOL OnKeyPress(eVKey key, eMsgFlag flag) override
    {
        if (log_dd_open_ && (key == NH3VKey::H3VK_ESCAPE
            || key == NH3VKey::H3VK_ENTER)) {
            SetLogDropdownOpen_(false);
            return TRUE;
        }
        return H3Dlg::OnKeyPress(key, flag);
    }

    // Start() 先跑 OnCreate 再 vShowAndRun 保存底层画面；创建阶段绝不能
    // 绘制动态文字，否则会被烙进“关闭时恢复的背景”。这里由消息驱动：
    // 开窗后首轮消息触发的 ShowMessage/重绘链自然带出键名。
    void RefreshKeyName_()
    {
        if (!key_name_) return;
        char utf8[64];
        if (s_listen_new_hotkey_)
            _snprintf(utf8, sizeof(utf8) - 1, "请按新键…");
        else
            ScanToName_(g_settings_hotkey_scan, utf8, sizeof(utf8));
        char gbk[64];
        if (!Utf8ToGbk_(utf8, gbk, sizeof(gbk))) return;
        key_name_->SetText(gbk);
        Redraw();
    }

    // 键盘钩子捕获新热键/取消后，从同一线程的窗口外调这里刷新显示。
    void OnHotkeyChanged_()
    {
        RefreshKeyName_();
    }
};

static SettingsDlg_* s_active_dlg_ = nullptr;

// ---- 弹窗（SEH 与 C++ 对象展开不能同函数，拆两层）----

static void RunSettingsDialog_()
{
    SettingsDlg_ dlg;
    s_active_dlg_ = &dlg;
    dlg.Start(); // 模态，返回即关窗；恢复背景由 vShow 的保存链负责
    InterlockedExchange(&s_listen_new_hotkey_, 0);
    s_active_dlg_ = nullptr;
}

static void OpenSettingsDialog_()
{
    if (InterlockedCompareExchange(&s_settings_dlg_running_, 1, 0) != 0)
        return;
    LogInfo("真随机: 打开设置窗（全局=%d 热键码=%d）",
        g_true_random_full, g_settings_hotkey_scan);
    LogRandomAudit_("打开设置");
    __try {
        RunSettingsDialog_();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogError("真随机: 设置窗异常 0x%08X", GetExceptionCode());
    }
    InterlockedExchange(&s_listen_new_hotkey_, 0);
    InterlockedExchange(&s_settings_dlg_running_, 0);
    LogRandomAudit_("关闭设置");
}

// ---- 键盘入口 hook ----

static bool IsModifierScan_(int scan)
{
    return scan == 29 || scan == 42 || scan == 54 // L-Ctrl, L/R-Shift
        || scan == 56 || scan == 58                // L-Alt, R-Alt(扩展)
        || scan == 0x5B || scan == 0x5C;           // L/R-Win
}

static bool __stdcall OnKeyMsg_(HiHook* hook, void* hwnd, UINT msg,
    UINT32 wp, UINT32 lp)
{
    (void)wp;
    const bool key_down = (msg == 0x100);
    const int scan = (int)((lp >> 16) & 0xFF);

    if (s_settings_dlg_running_) {
        if (key_down && s_listen_new_hotkey_) {
            if (scan == 1) { // ESC 取消修改
                InterlockedExchange(&s_listen_new_hotkey_, 0);
                if (s_active_dlg_) s_active_dlg_->OnHotkeyChanged_();
                LogInfo("真随机: 热键修改取消");
            } else if (!IsModifierScan_(scan)) {
                char name[32];
                ScanToName_(scan, name, sizeof(name));
                g_settings_hotkey_scan = scan;
                PersistHotkey_();
                InterlockedExchange(&s_listen_new_hotkey_, 0);
                if (s_active_dlg_) s_active_dlg_->OnHotkeyChanged_();
                LogInfo("真随机: 设置热键改为 %s（扫描码 %d）", name, scan);
            }
            return 0; // 侦听期间按键全部吞掉（修饰键继续侦听）
        }
        // 设置窗开着时，热键本身吞掉避免递归弹窗；其余交给窗/原函数。
        if (key_down && scan == g_settings_hotkey_scan)
            return 0;
        return FASTCALL_4(bool, hook->GetDefaultFunc(), hwnd, msg, wp, lp);
    }

    if (key_down && !(lp & 0x40000000)   // 忽略按住自动重复
        && scan == g_settings_hotkey_scan && InputMgrReady_()) {
        OpenSettingsDialog_();
        return 0; // 该键已消费，不进游戏
    }
    return FASTCALL_4(bool, hook->GetDefaultFunc(), hwnd, msg, wp, lp);
}

static void InstallSettingsHooks_()
{
    if (!_PI) return;
    _PI->WriteHiHook(kKeyMsgProc_, SPLICE_, EXTENDED_, FASTCALL_,
        reinterpret_cast<void*>(&OnKeyMsg_));
    char name[32];
    ScanToName_(g_settings_hotkey_scan, name, sizeof(name));
    LogInfo("真随机: 设置热键 Hook 已安装（0x4EC1C0，当前 %s/%d）",
        name, g_settings_hotkey_scan);
}
