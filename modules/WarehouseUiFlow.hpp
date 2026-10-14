#pragma once

// ========== WarehouseUiFlow.hpp ==========
// Pure geometry and transaction model for the warehouse UI.
// This header never touches game pointers, hooks, native calls, or I/O.

#include "WarehouseCore.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace warehouseui {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    constexpr std::int64_t Right() const noexcept {
        return static_cast<std::int64_t>(x) + width;
    }
    constexpr std::int64_t Bottom() const noexcept {
        return static_cast<std::int64_t>(y) + height;
    }
    constexpr bool Overlaps(const Rect& other) const noexcept {
        return x < other.Right() && other.x < Right()
            && y < other.Bottom() && other.y < Bottom();
    }
    constexpr bool Contains(int px, int py) const noexcept {
        return px >= x && px < Right() && py >= y && py < Bottom();
    }
};

// 背包面板区域判定（round-8）：空槽/底板等未知 ID 也要认成“背包内点击”。
inline constexpr bool PointInRect(int px, int py, const Rect& rect) noexcept {
    return rect.Contains(px, py);
}

// round-9：页签行 = 两端箭头 + 居中的 N 个页签。返回页签组在面板内的起始 X。
// （405 宽面板、4 个 36 宽页签、间距 4 → 起始 124。）
inline constexpr int CenteredTabsX(int panelWidth, int tabWidth, int slots,
                                  int gap) noexcept {
    if (tabWidth <= 0 || slots <= 0 || gap < 0) return 0;
    const int total = slots * tabWidth + (slots - 1) * gap;
    if (total >= panelWidth) return 0;
    return (panelWidth - total) / 2;
}

// round-9.1（用户裁定“能显示多少就显示多少”）：内区宽 span 里能放下多少个
// tabWidth 宽、间距 gap 的页签。span=333、36 宽、间距 4 → 8 个。
inline constexpr int FittingTabCount(int span, int tabWidth, int gap) noexcept {
    if (tabWidth <= 0 || gap < 0 || span < tabWidth) return 0;
    return (span + gap) / (tabWidth + gap);
}

// round-9：滚轮 → 页签窗口滚动方向。H3 消息层把 WM_MOUSEWHEEL 的增量放在
// subtype（H3Auto PanelInput 已实证：>0 上滚、<0 下滚，量级无关）。
// 上滚=窗口向左（更早的页签），下滚=窗口向右。
inline constexpr int TabWheelShift(int wheelDelta) noexcept {
    if (wheelDelta > 0) return -1;
    if (wheelDelta < 0) return 1;
    return 0;
}

inline constexpr bool Intersects(const Rect& left, const Rect& right) noexcept {
    return left.Overlaps(right);
}

enum class LayoutSide : std::uint8_t { None, Right, Left };

struct Layout {
    bool fits = false;
    Rect backpack;
    Rect warehouse;
    LayoutSide side = LayoutSide::None;
    std::size_t columns = 0;
    std::size_t page_size = 0;
    std::size_t rows = 0;
};

// The UI layer does not translate the native backpack controls. Only attach
// when layout planning preserves the game's actual native position.
inline constexpr bool LayoutPreservesBackpackPosition(const Layout& layout,
                                                       int actualX,
                                                       int actualY) noexcept {
    return layout.fits && layout.backpack.x == actualX
        && layout.backpack.y == actualY;
}

inline constexpr int kBackpackWidth = 405;    // HD 完整背包视觉区（0x01006620 实测）
inline constexpr int kBackpackHeight = 459;
inline constexpr int kPanelWidth = 405;       // 仓库面板宽与完整背包一致（用户裁定）
// 面板高 = 完整背包 459 + Tab 行 32 + 分页行 32 = 523（用户裁定：抄整个扩展界面，
// 顶部预留 Tab 页、网格下多一个分页行）。
inline constexpr int kTabRowHeight = 32;      // Tab 行预留（本版不画控件）
inline constexpr int kPagerRowHeight = 32;    // 分页行（< > 与计数）
inline constexpr int kPanelHeight = 459 + kTabRowHeight + kPagerRowHeight; // 523
inline constexpr int kPanelGap = 8;
inline constexpr int kScreenMargin = 8;
// HD 完整背包内部几何（反汇编 0x039A00 构造实测，仓库面板同构复用）：
// 标题 405x30 @ (0,18)；BkPack.pcx 369x369 @ (18,50)；
// 8x8 网格 hit 47x47 步距 46 @ (18+col*46, 50+row*46)；
// DEF 图标 44x44 @ (20+col*46, 51+row*46)；底部状态条 389x18 @ (8,441)。
// 仓库面板把网格整体下移 kTabRowHeight：网格原点 (18, 82)。
inline constexpr int kTitleHeight = 30;
inline constexpr int kTitleOffsetY = 18;
inline constexpr int kGridOriginDX = 18;
inline constexpr int kGridOriginDY = 50 + kTabRowHeight; // 82
// 原生背包网格原点（无 Tab 行偏移）：完整背包窗口自身网格在 (18,50)。
// 覆盖图标必须用这个值，误用仓库的 82 会整体下移 32px 造成“网格与物品错位”。
inline constexpr int kBackpackGridOriginDY = 50;
// 分页栏（round-7）：两侧箭头（复用 arrow 素材 18x22）+ 中间 4 个页码按钮。
inline constexpr int kPagerButtonWidth = 36;
inline constexpr int kPagerButtonHeight = 22;
inline constexpr int kPagerButtonGap = 4;
inline constexpr int kPagerButtonCount = 4;
inline constexpr int kGridPitch = 46;
inline constexpr int kGridHitSize = 47;
inline constexpr int kGridDefSize = 44;
inline constexpr int kGridDefDX = 2;
inline constexpr int kGridDefDY = 1;
// 分页行：背景底(82+369=451)与状态条之间；状态条沿用扩展底位 441+32=473。
inline constexpr int kPagerRowY = kGridOriginDY + 369 + 8; // 459：分页行顶
inline constexpr int kStatusTextY = kPanelHeight - 18;    // 505：底部状态条（预留）
inline constexpr std::size_t kGridColumns = 8;
// 背包网格行数（8x8=64 格）；与仓库面板分页行数同值但语义不同，单独命名。
inline constexpr std::size_t kGridRows = 8;
inline constexpr std::size_t kPageRows = 8;
inline constexpr std::size_t kPageSize = kGridColumns * kPageRows; // 64，与扩展同构
inline constexpr int kMaxArtifactId = 143;
inline constexpr int kScrollArtifactId = 1;
inline constexpr int kMaxScrollSubtype = 69;
inline constexpr int kMinScreenWidth = kScreenMargin + kBackpackWidth
    + kPanelGap + kPanelWidth + kScreenMargin; // 834：背包 405 与面板 405 并排
inline constexpr int kMinScreenHeight = kPanelHeight;

inline int ClampPanelY(int requestedY, int screenHeight) noexcept {
    const int maximum = screenHeight - kPanelHeight;
    if (requestedY < 0) return 0;
    if (requestedY > maximum) return maximum;
    return requestedY;
}

inline std::size_t PanelColumns(int) noexcept {
    return kGridColumns;
}

inline std::size_t PanelPageSize(int) noexcept {
    return kPageSize;
}

inline Layout MakeLayout(int screenWidth, int screenHeight, int backpackX,
                         int backpackY) noexcept {
    Layout result;
    if (screenWidth < kMinScreenWidth || screenHeight < kMinScreenHeight)
        return result;
    const std::int64_t oldX = backpackX;
    const std::int64_t oldRight = oldX + kBackpackWidth;
    const int top = ClampPanelY(backpackY, screenHeight);
    const bool originalOnScreen = oldX >= kScreenMargin
        && oldRight <= static_cast<std::int64_t>(screenWidth) - kScreenMargin;
    const std::int64_t rightX = oldRight + kPanelGap;
    const std::int64_t rightWidth = static_cast<std::int64_t>(screenWidth)
        - kScreenMargin - rightX;
    const std::int64_t leftWidth = oldX - kPanelGap - kScreenMargin;
    int placedBackpackX = backpackX;
    int panelX = 0;
    if (originalOnScreen && rightWidth >= kPanelWidth) {
        panelX = static_cast<int>(rightX);
        result.warehouse = {panelX, top, kPanelWidth, kPanelHeight};
        result.side = LayoutSide::Right;
    } else if (originalOnScreen && leftWidth >= kPanelWidth) {
        panelX = static_cast<int>(oldX - kPanelGap - kPanelWidth);
        result.warehouse = {panelX, top, kPanelWidth, kPanelHeight};
        result.side = LayoutSide::Left;
    } else {
        // 原位两侧都放不下同尺寸面板：背包贴左缘，面板固定 405 宽在其右侧。
        const int fallbackX = kScreenMargin;
        const std::int64_t fallbackPanelX =
            static_cast<std::int64_t>(fallbackX) + kBackpackWidth + kPanelGap;
        const std::int64_t fallbackWidth = static_cast<std::int64_t>(screenWidth)
            - kScreenMargin - fallbackPanelX;
        if (fallbackWidth < kPanelWidth) return result;
        placedBackpackX = fallbackX;
        panelX = static_cast<int>(fallbackPanelX);
        result.warehouse = {panelX, top, kPanelWidth, kPanelHeight};
        result.side = LayoutSide::Right;
    }

    result.backpack = {placedBackpackX, top, kBackpackWidth, kBackpackHeight};
    result.fits = !result.backpack.Overlaps(result.warehouse);
    if (!result.fits) {
        result = {};
        return result;
    }
    result.columns = kGridColumns;
    result.page_size = kPageSize;
    result.rows = kPageRows;
    return result;
}

inline std::size_t PageCount(std::size_t itemCount,
                             std::size_t pageSize = kPageSize) noexcept {
    if (pageSize == 0) return 0;
    return itemCount / pageSize + (itemCount % pageSize == 0 ? 0u : 1u);
}

inline std::size_t PageBegin(std::size_t itemCount, std::size_t pageIndex,
                             std::size_t pageSize = kPageSize) noexcept {
    const std::size_t pages = PageCount(itemCount, pageSize);
    if (pageSize == 0 || pageIndex >= pages) return itemCount;
    if (pageIndex > (std::numeric_limits<std::size_t>::max)() / pageSize)
        return itemCount;
    const std::size_t begin = pageIndex * pageSize;
    return begin <= itemCount ? begin : itemCount;
}

inline std::size_t PageItemCount(std::size_t itemCount, std::size_t pageIndex,
                                 std::size_t pageSize = kPageSize) noexcept {
    if (pageSize == 0) return 0;
    const std::size_t begin = PageBegin(itemCount, pageIndex, pageSize);
    if (begin > itemCount) return 0;
    const std::size_t remaining = itemCount - begin;
    return remaining < pageSize ? remaining : pageSize;
}

struct PageView {
    std::size_t begin = 0;
    std::size_t count = 0;
};

inline PageView MakePageView(std::size_t itemCount, std::size_t pageIndex,
                             std::size_t pageSize = kPageSize) noexcept {
    return {PageBegin(itemCount, pageIndex, pageSize),
            PageItemCount(itemCount, pageIndex, pageSize)};
}

inline bool PageRecordIndex(std::size_t itemCount, std::size_t pageIndex,
                            std::size_t row, std::size_t column,
                            std::size_t columns, std::size_t& index) noexcept {
    if (columns == 0
        || columns > (std::numeric_limits<std::size_t>::max)() / kPageRows)
        return false;
    const PageView view = MakePageView(itemCount, pageIndex, columns * kPageRows);
    if (column >= columns || row >= kPageRows) return false;
    if (row > (std::numeric_limits<std::size_t>::max)() / columns)
        return false;
    const std::size_t rowOffset = row * columns;
    if (column > (std::numeric_limits<std::size_t>::max)() - rowOffset)
        return false;
    const std::size_t relative = rowOffset + column;
    if (relative >= view.count) return false;
    index = view.begin + relative;
    return true;
}

enum class Result : std::uint8_t {
    Ok = 0,
    InvalidSession,
    InvalidHero,
    InvalidOwner,
    InvalidToken,
    InvalidBackpack,
    InvalidRecord,
    InvalidIndex,
    StaleSelection,
    StaleWarehouse,
    Locked,
    HeldArtifact,
    CapacityReached,
    NoSelection,
    DuplicateOperation,
    AllocationFailure,
    EpochExhausted,

    // Semantic aliases for adapters; the underlying UI enum remains independent
    // from warehouse::Result and its numeric ordering stays stable.
    WrongHero = InvalidHero,
    WrongOwner = InvalidOwner,
    WrongSession = InvalidToken,
    StaleSource = StaleSelection,
    WarehouseChanged = StaleWarehouse
};

using FlowResult = Result;

inline bool ResultSuccess(Result result) noexcept { return result == Result::Ok; }

inline bool IsValidGameRecord(const warehouse::ArtifactRecord& record) noexcept {
    if (record.artifactId < 0 || record.artifactId > kMaxArtifactId) return false;
    if (record.artifactId == kScrollArtifactId
        && (record.subtype < 0 || record.subtype > kMaxScrollSubtype)) return false;
    return true;
}

inline Result ValidateNative(const warehouse::NativeBackpack& native,
                             int declaredCount) noexcept {
    if (declaredCount < 0
        || declaredCount > static_cast<int>(warehouse::kBackpackCapacity))
        return Result::InvalidBackpack;
    int occupied = 0;
    for (const auto& record : native) {
        if (record.artifactId == -1) continue;
        if (!IsValidGameRecord(record)) return Result::InvalidRecord;
        ++occupied;
    }
    return occupied == declaredCount ? Result::Ok : Result::InvalidBackpack;
}

inline bool SameNative(const warehouse::NativeBackpack& left,
                       const warehouse::NativeBackpack& right) noexcept {
    for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i)
        if (!(left[i] == right[i])) return false;
    return true;
}

// 展示排序（round-8 用户裁定）：分类页展示道具时相同道具必须连续。
// 以 (宝物ID, 子类型) 为同一性键做稳定排序，同组内保持原有仓库顺序，
// 因此只改变展示/取回顺序，不改变仓库存储顺序，也不影响记录下标语义。
template <typename Records>
inline void SortRecordIndicesByIdentity(std::vector<std::size_t>& indices,
                                      const Records& records) {
    std::stable_sort(indices.begin(), indices.end(),
        [&records](std::size_t left, std::size_t right) noexcept {
            if (left >= records.size() || right >= records.size()) return left < right;
            if (records[left].artifactId != records[right].artifactId)
                return records[left].artifactId < records[right].artifactId;
            return records[left].subtype < records[right].subtype;
        });
}

// 存储层整理（round-8 用户裁定，“整理”按钮）：直接重排仓库记录本体，
// 同一性键与展示排序一致；空记录（artifactId<0）一律排到末尾。
template <typename Records>
inline void SortRecordsByIdentity(Records& records) {
    std::stable_sort(records.begin(), records.end(),
        [](const typename Records::value_type& left,
           const typename Records::value_type& right) noexcept {
            const bool leftEmpty = left.artifactId < 0;
            const bool rightEmpty = right.artifactId < 0;
            if (leftEmpty != rightEmpty) return !leftEmpty;
            if (left.artifactId != right.artifactId)
                return left.artifactId < right.artifactId;
            return left.subtype < right.subtype;
        });
}

class Flow {
public:
    Flow() noexcept = default;

    Flow(const warehouse::NativeBackpack& native, int declaredCount,
         std::int32_t heroId, std::int32_t ownerId,
         std::uint64_t warehouseEpoch, std::uint64_t sessionToken) noexcept
        : native_(native), declaredCount_(declaredCount), heroId_(heroId),
          ownerId_(ownerId), warehouseEpoch_(warehouseEpoch),
          sessionToken_(sessionToken),
          valid_(warehouse::IsValidHeroId(heroId) && sessionToken != 0
              && ValidateNative(native, declaredCount) == Result::Ok) {}

    bool IsValid() const noexcept { return valid_; }
    bool HasSelection() const noexcept { return selected_; }
    std::size_t SelectedIndex() const noexcept { return selectedIndex_; }
    std::int32_t HeroId() const noexcept { return heroId_; }
    std::int32_t OwnerId() const noexcept { return ownerId_; }
    std::uint64_t WarehouseEpoch() const noexcept { return warehouseEpoch_; }
    std::uint64_t SessionToken() const noexcept { return sessionToken_; }
    int DeclaredCount() const noexcept { return declaredCount_; }
    const warehouse::NativeBackpack& Native() const noexcept { return native_; }

    void Select(std::size_t nativeIndex) noexcept {
        selectedIndex_ = nativeIndex;
        selected_ = true;
        committedDeposit_ = false;
    }

    void CancelSelection() noexcept {
        selected_ = false;
        committedDeposit_ = false;
    }

    Result CheckDeposit(const warehouse::State& state,
                        const warehouse::NativeBackpack& native, int count,
                        std::int32_t currentHero, std::int32_t currentOwner,
                        std::uint64_t token, bool locked, bool held) const noexcept {
        return Guard(state, native, count, currentHero, currentOwner, token,
                     locked, held);
    }

    Result Deposit(warehouse::State& state, warehouse::NativeBackpack& native,
                   int& count, std::int32_t currentHero,
                   std::int32_t currentOwner, std::uint64_t token,
                   bool locked, bool held) noexcept {
        if (committedDeposit_) return Result::DuplicateOperation;
        if (!selected_) return Result::NoSelection;
        const Result guard = Guard(state, native, count, currentHero,
                                   currentOwner, token, locked, held);
        if (guard != Result::Ok) return guard;
        if (selectedIndex_ >= warehouse::kBackpackCapacity
            || native_[selectedIndex_].artifactId < 0)
            return Result::InvalidIndex;
        const auto moved = native_[selectedIndex_];
        if (!IsValidGameRecord(moved)) return Result::InvalidRecord;
        auto& target = state.heroes[static_cast<std::size_t>(heroId_)];
        for (const auto& record : target)
            if (!IsValidGameRecord(record)) return Result::InvalidRecord;
        if (target.size() >= target.max_size()
            || target.size() == (std::numeric_limits<std::size_t>::max)())
            return Result::AllocationFailure;
        if (state.mutationEpoch == (std::numeric_limits<std::uint64_t>::max)())
            return Result::EpochExhausted;

        warehouse::NativeBackpack nativeAfter;
        nativeAfter.fill({-1, -1});
        std::array<warehouse::ArtifactRecord, warehouse::kBackpackCapacity> emptySlots{};
        std::size_t output = 0;
        std::size_t emptyCount = 0;
        for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i) {
            if (i == selectedIndex_) continue;
            if (native_[i].artifactId == -1) {
                emptySlots[emptyCount++] = native_[i];
            } else {
                nativeAfter[output++] = native_[i];
            }
        }
        for (std::size_t i = 0; i < emptyCount; ++i)
            nativeAfter[output + i] = emptySlots[i];
        warehouse::Backpack targetAfter;
        try {
            targetAfter.reserve(target.size() + 1);
            targetAfter.insert(targetAfter.end(), target.begin(), target.end());
            targetAfter.push_back(moved);
        } catch (const std::bad_alloc&) {
            return Result::AllocationFailure;
        } catch (const std::length_error&) {
            return Result::AllocationFailure;
        }

        target.swap(targetAfter);
        native = nativeAfter;
        count = static_cast<int>(output);
        ++state.mutationEpoch;
        native_ = nativeAfter;
        declaredCount_ = count;
        warehouseEpoch_ = state.mutationEpoch;
        selected_ = false;
        committedDeposit_ = true;
        return Result::Ok;
    }

    Result Deposit(warehouse::State& state, warehouse::NativeBackpack& native,
                   int& count, bool locked, bool held) noexcept {
        return Deposit(state, native, count, heroId_, ownerId_, sessionToken_,
                       locked, held);
    }

    Result Retrieve(std::size_t index, warehouse::State& state,
                    warehouse::NativeBackpack& native, int& count,
                    std::int32_t currentHero, std::int32_t currentOwner,
                    std::uint64_t token, bool locked, bool held) noexcept {
        const Result guard = Guard(state, native, count, currentHero,
                                   currentOwner, token, locked, held);
        if (guard != Result::Ok) return guard;
        auto& source = state.heroes[static_cast<std::size_t>(heroId_)];
        if (index >= source.size()) return Result::InvalidIndex;
        for (const auto& record : source)
            if (!IsValidGameRecord(record)) return Result::InvalidRecord;
        if (count >= static_cast<int>(warehouse::kBackpackCapacity))
            return Result::CapacityReached;
        if (state.mutationEpoch == (std::numeric_limits<std::uint64_t>::max)())
            return Result::EpochExhausted;

        std::size_t empty = warehouse::kBackpackCapacity;
        for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i) {
            if (native_[i].artifactId == -1) {
                empty = i;
                break;
            }
        }
        if (empty == warehouse::kBackpackCapacity)
            return Result::InvalidBackpack;
        warehouse::NativeBackpack nativeAfter = native_;
        nativeAfter[empty] = source[index];
        try {
            warehouse::Backpack sourceAfter;
            sourceAfter.reserve(source.size() - 1);
            for (std::size_t i = 0; i < source.size(); ++i)
                if (i != index) sourceAfter.push_back(source[i]);
            source.swap(sourceAfter);
        } catch (const std::bad_alloc&) {
            return Result::AllocationFailure;
        } catch (const std::length_error&) {
            return Result::AllocationFailure;
        }

        native = nativeAfter;
        ++count;
        ++state.mutationEpoch;
        native_ = nativeAfter;
        declaredCount_ = count;
        warehouseEpoch_ = state.mutationEpoch;
        selected_ = false;
        committedDeposit_ = false;
        return Result::Ok;
    }

    Result Retrieve(std::size_t index, warehouse::State& state,
                    warehouse::NativeBackpack& native, int& count,
                    bool locked, bool held) noexcept {
        return Retrieve(index, state, native, count, heroId_, ownerId_,
                        sessionToken_, locked, held);
    }

    // include is indexed by the complete 64-slot native snapshot, including holes.
    // Prepare every allocation before committing either inventory or the epoch.
    Result DepositAll(warehouse::State& state, warehouse::NativeBackpack& native,
                      int& count, std::int32_t currentHero,
                      std::int32_t currentOwner, std::uint64_t token,
                      bool locked, bool held, const std::vector<char>& include,
                      std::size_t& moved) noexcept {
        moved = 0;
        const Result guard = Guard(state, native, count, currentHero,
                                   currentOwner, token, locked, held);
        if (guard != Result::Ok) return guard;
        if (include.size() != warehouse::kBackpackCapacity)
            return Result::InvalidIndex;
        auto& target = state.heroes[static_cast<std::size_t>(heroId_)];
        for (const auto& record : target)
            if (!IsValidGameRecord(record)) return Result::InvalidRecord;
        std::size_t moving = 0;
        for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i)
            if (include[i] && native_[i].artifactId != -1) ++moving;
        if (moving == 0) return Result::Ok;
        if (state.mutationEpoch == (std::numeric_limits<std::uint64_t>::max)())
            return Result::EpochExhausted;
        if (moving > target.max_size() - target.size())
            return Result::AllocationFailure;

        warehouse::NativeBackpack nativeAfter;
        nativeAfter.fill({-1, -1});
        std::array<warehouse::ArtifactRecord, warehouse::kBackpackCapacity> emptySlots{};
        std::size_t output = 0;
        std::size_t emptyCount = 0;
        for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i) {
            if (native_[i].artifactId == -1) {
                if (!include[i]) emptySlots[emptyCount++] = native_[i];
            } else if (!include[i]) {
                nativeAfter[output++] = native_[i];
            }
        }
        for (std::size_t i = 0; i < emptyCount; ++i)
            nativeAfter[output + i] = emptySlots[i];
        warehouse::Backpack targetAfter;
        try {
            targetAfter.reserve(target.size() + moving);
            targetAfter.insert(targetAfter.end(), target.begin(), target.end());
            for (std::size_t i = 0; i < warehouse::kBackpackCapacity; ++i)
                if (include[i] && native_[i].artifactId != -1)
                    targetAfter.push_back(native_[i]);
        } catch (const std::bad_alloc&) {
            return Result::AllocationFailure;
        } catch (const std::length_error&) {
            return Result::AllocationFailure;
        }

        target.swap(targetAfter);
        native = nativeAfter;
        count = static_cast<int>(output);
        ++state.mutationEpoch;
        native_ = nativeAfter;
        declaredCount_ = count;
        warehouseEpoch_ = state.mutationEpoch;
        selected_ = false;
        committedDeposit_ = false;
        moved = moving;
        return Result::Ok;
    }

    // Consume selected warehouse entries in their original order. Fill native
    // holes in slot order, preserving every untouched complete native pair.
    Result RetrieveAll(warehouse::State& state, warehouse::NativeBackpack& native,
                       int& count, std::int32_t currentHero,
                       std::int32_t currentOwner, std::uint64_t token,
                       bool locked, bool held, const std::vector<char>& include,
                       std::size_t& moved) noexcept {
        moved = 0;
        const Result guard = Guard(state, native, count, currentHero,
                                   currentOwner, token, locked, held);
        if (guard != Result::Ok) return guard;
        auto& source = state.heroes[static_cast<std::size_t>(heroId_)];
        if (include.size() != source.size()) return Result::InvalidIndex;
        bool hasMatch = false;
        for (std::size_t i = 0; i < source.size(); ++i) {
            if (!IsValidGameRecord(source[i])) return Result::InvalidRecord;
            if (include[i]) hasMatch = true;
        }
        if (!hasMatch) return Result::Ok;
        const std::size_t capacity = warehouse::kBackpackCapacity
            - static_cast<std::size_t>(count);
        if (capacity == 0) return Result::CapacityReached;
        if (state.mutationEpoch == (std::numeric_limits<std::uint64_t>::max)())
            return Result::EpochExhausted;

        warehouse::NativeBackpack nativeAfter = native_;
        warehouse::Backpack sourceAfter;
        std::size_t moving = 0;
        std::size_t empty = 0;
        try {
            sourceAfter.reserve(source.size());
            for (std::size_t i = 0; i < source.size(); ++i) {
                if (include[i] && moving < capacity) {
                    while (nativeAfter[empty].artifactId != -1) ++empty;
                    nativeAfter[empty++] = source[i];
                    ++moving;
                } else {
                    sourceAfter.push_back(source[i]);
                }
            }
        } catch (const std::bad_alloc&) {
            return Result::AllocationFailure;
        } catch (const std::length_error&) {
            return Result::AllocationFailure;
        }

        source.swap(sourceAfter);
        native = nativeAfter;
        count += static_cast<int>(moving);
        ++state.mutationEpoch;
        native_ = nativeAfter;
        declaredCount_ = count;
        warehouseEpoch_ = state.mutationEpoch;
        selected_ = false;
        committedDeposit_ = false;
        moved = moving;
        return Result::Ok;
    }

private:
    Result Guard(const warehouse::State& state,
                 const warehouse::NativeBackpack& native, int count,
                 std::int32_t currentHero, std::int32_t currentOwner,
                 std::uint64_t token, bool locked, bool held) const noexcept {
        if (!valid_) return Result::InvalidSession;
        if (!warehouse::IsValidHeroId(currentHero) || currentHero != heroId_)
            return Result::InvalidHero;
        if (currentOwner != ownerId_) return Result::InvalidOwner;
        if (token != sessionToken_) return Result::InvalidToken;
        if (locked) return Result::Locked;
        if (held) return Result::HeldArtifact;
        if (state.mutationEpoch != warehouseEpoch_)
            return Result::StaleWarehouse;
        const Result nativeValidity = ValidateNative(native, count);
        if (nativeValidity != Result::Ok) return nativeValidity;
        if (count != declaredCount_ || !SameNative(native, native_))
            return Result::StaleSelection;
        return Result::Ok;
    }

    warehouse::NativeBackpack native_{};
    int declaredCount_ = 0;
    std::int32_t heroId_ = -1;
    std::int32_t ownerId_ = -1;
    std::uint64_t warehouseEpoch_ = 0;
    std::uint64_t sessionToken_ = 0;
    std::size_t selectedIndex_ = 0;
    bool valid_ = false;
    bool selected_ = false;
    bool committedDeposit_ = false;
};

} // namespace warehouseui
