// ========== WarehouseUi.hpp ==========
// 仓库 UI 阶段 A：HD_SOD 版本门卫 + 纯 UI 生命周期核心。
// 本头文件不含任何游戏 Hook、宝物转移或弹窗绘制逻辑；只提供可离线测试
// 的版本/字节门卫和 session 状态机会。模块化拆分的原因：
//   1) UI 阶段必须先验证目标版本与生命周期，再接任何真实背包事件；
//   2) 生命周期错误（旧 dialog 指针、重开、外部关闭）比功能缺失更危险，
//      必须先有可回归的状态机，再允许接入生产事件容器。
// 实现文件 WarehouseUi.inc.cpp 只做接线与日志；所有判定逻辑在这里保持
// 纯函数化，不依赖 H3API、patcher 或游戏内存。

#ifndef H3RNDNEW_WAREHOUSE_UI_HPP_
#define H3RNDNEW_WAREHOUSE_UI_HPP_

#include <cstdint>
#include <cstddef>

namespace warehouseui {

// 当前受支持的 HD_SOD 指纹。字节签名来自只读 UI 审计
// （D:\logs\dsh\scripts\h3-warehouse-ui\ui-audit-report.md），
// 目标是本机 D:\Heroes3\Heroes3_2026.10.09\HD_SOD.dll。
// 换游戏版本、升级 HD_SOD 或任何关键 RVA 改变时，必须重新审计并更新本表，
// 不得在签名不匹配时继续尝试挂 UI，否则会把偏移错配当成功能失败。
struct HsodFingerprint_ {
    const char* label;       // 便于日志识别的检查点名称。
    std::uint32_t imageRva;  // 相对 HD_SOD 模块基址的 RVA。
    const std::uint8_t* bytes;
    std::size_t size;
};

// 模块版本与文件大小门禁。版本为核心事实，文件大小做第二证据。
inline constexpr wchar_t kHsodProductVersion[] = L"5.0.0.4";
inline constexpr wchar_t kHsodFileVersion[] = L"0.2.0.92";
inline constexpr std::uint32_t kHsodFileSize = 1451520u;
// 入口与回调关键字节（RVA 由 PE section 现场计算，不当作绝对地址使用）。
inline constexpr std::uint8_t kHsodCtorSig_[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x5C, 0xA1, 0x38, 0x95, 0x69, 0x00 };
inline constexpr std::uint8_t kHsodCallbackSig_[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C, 0xA1, 0xB8, 0x92, 0x69, 0x00 };
inline constexpr std::uint8_t kHsodModalCallSig_[] = {
    0xFF, 0xD0, 0x8B, 0x4D, 0xF8, 0xC7, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00 };
inline constexpr std::uint8_t kHsodModalReturnSig_[] = {
    0xC2, 0x18, 0x00, 0xCC, 0xCC };
inline constexpr std::uint8_t kHsodModalSiteSig_[] = {
    0x6A, 0x00, 0xBA, 0x04, 0x00, 0x00, 0x00, 0x6B,
    0xC2, 0x06, 0x8B, 0x4D, 0xF8, 0x8B, 0x11 };
inline constexpr std::uint8_t kHsodModalEndSig_[] = {
    0xC7, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00, 0x8B,
    0x55, 0xF8 };

inline constexpr HsodFingerprint_ kHsodFingerprints_[] = {
    { "backpack-ctor", 0x039A00u, kHsodCtorSig_, sizeof(kHsodCtorSig_) },
    { "backpack-callback", 0x0395D0u, kHsodCallbackSig_, sizeof(kHsodCallbackSig_) },
    { "backpack-modal-site", 0x039E66u, kHsodModalSiteSig_, sizeof(kHsodModalSiteSig_) },
    { "backpack-modal-call", 0x039E7Bu, kHsodModalCallSig_, sizeof(kHsodModalCallSig_) },
    { "backpack-modal-end", 0x039E80u, kHsodModalEndSig_, sizeof(kHsodModalEndSig_) },
    { "backpack-modal-return", 0x039EC0u, kHsodModalReturnSig_, sizeof(kHsodModalReturnSig_) },
};

inline constexpr std::size_t kHsodFingerprintCount_ =
    sizeof(kHsodFingerprints_) / sizeof(kHsodFingerprints_[0]);

inline bool MatchesFingerprint(const std::uint8_t* image, std::size_t imageSize,
                               const HsodFingerprint_& fingerprint) noexcept
{
    if (!image || fingerprint.imageRva > imageSize
        || fingerprint.size > imageSize - fingerprint.imageRva)
        return false;
    for (std::size_t i = 0; i < fingerprint.size; ++i) {
        if (image[fingerprint.imageRva + i] != fingerprint.bytes[i]) return false;
    }
    return true;
}

inline bool MatchesAllFingerprints(const std::uint8_t* image,
                                   std::size_t imageSize) noexcept
{
    for (const auto& fingerprint : kHsodFingerprints_) {
        if (!MatchesFingerprint(image, imageSize, fingerprint)) return false;
    }
    return true;
}

// UI 生命周期。这些状态只描述插件对当前完整背包弹窗的观察身份，
// 不保存游戏对象指针，也不表示弹窗一定仍然可访问。
enum class UiState : std::uint8_t {
    Inactive_ = 0,   // 没有活动 UI session。
    Attaching_ = 1,  // 已识别打开请求，正在绑定身份。
    Active_ = 2,     // 已绑定并允许刷新自有状态。
    Closing_ = 3,    // 已观察到关闭/离开路径，等待最终清理。
    Invalidated_ = 4,// 身份或版本失效；旧 session 必须整体作废。
};

// 关闭原因。日志按原因区分排障，不吞并成同一种“关闭”。
enum class CloseReason : std::uint8_t {
    None_ = 0,
    Esc_ = 1,
    ExternalClose_ = 2,
    ResultNegativeTwo_ = 3,
    HeroChanged_ = 4,
    VersionInvalid_ = 5,
    Exception_ = 6,
};

// UI session 身份。token 由插件在打开观察时递增生成，用于拒绝旧窗口。
// 不使用游戏对象地址作为身份纯文本：地址会在弹窗销毁后复用。
struct UiSession_ {
    std::uint64_t token;
    UiState state;
    CloseReason closeReason;
    bool pendingSelection; // 阶段 C 的“已选背包宝物”占位；阶段 A 恒 false。
};

class UiSessionStore_ {
public:
    UiSessionStore_() = default;

    void Reset() noexcept
    {
        token_ = 0;
        state_ = UiState::Inactive_;
        closeReason_ = CloseReason::None_;
        pending_ = false;
    }

    std::uint64_t BeginAttach() noexcept
    {
        // 同一时刻只允许一个 session 绑定；若上层未观察到关闭，
        // 旧 session 一律失效，绝不共享状态。
        if (state_ != UiState::Inactive_
            && state_ != UiState::Closing_
            && state_ != UiState::Invalidated_) {
            state_ = UiState::Invalidated_;
            closeReason_ = CloseReason::ExternalClose_;
        }
        ++token_;
        state_ = UiState::Attaching_;
        closeReason_ = CloseReason::None_;
        pending_ = false;
        return token_;
    }

    bool Activate(std::uint64_t token) noexcept
    {
        if (token != token_ || state_ != UiState::Attaching_) return false;
        state_ = UiState::Active_;
        return true;
    }

    bool BeginClose(std::uint64_t token, CloseReason reason) noexcept
    {
        if (token != token_) return false;
        if (state_ != UiState::Active_ && state_ != UiState::Attaching_) return false;
        state_ = UiState::Closing_;
        closeReason_ = reason;
        pending_ = false;
        return true;
    }

    bool FinishClose(std::uint64_t token) noexcept
    {
        if (token != token_ || state_ != UiState::Closing_) return false;
        state_ = UiState::Inactive_;
        closeReason_ = CloseReason::None_;
        return true;
    }

    void Invalidate(CloseReason reason) noexcept
    {
        state_ = UiState::Invalidated_;
        closeReason_ = reason;
        pending_ = false;
    }

    bool IsValidFor(std::uint64_t token) const noexcept
    {
        if (token != token_) return false;
        return state_ == UiState::Active_ || state_ == UiState::Attaching_;
    }

    bool ConsumePendingSelection(std::uint64_t token) noexcept
    {
        if (!IsValidFor(token)) return false;
        const bool had = pending_;
        pending_ = false;
        return had;
    }

    bool SetPendingSelection(std::uint64_t token) noexcept
    {
        if (!IsValidFor(token)) return false;
        pending_ = true;
        return true;
    }

    std::uint64_t token() const noexcept { return token_; }
    UiState state() const noexcept { return state_; }
    CloseReason closeReason() const noexcept { return closeReason_; }
    bool pendingSelection() const noexcept { return pending_; }

private:
    std::uint64_t token_ = 0;
    UiState state_ = UiState::Inactive_;
    CloseReason closeReason_ = CloseReason::None_;
    bool pending_ = false;
};

} // namespace warehouseui

#endif // H3RNDNEW_WAREHOUSE_UI_HPP_
