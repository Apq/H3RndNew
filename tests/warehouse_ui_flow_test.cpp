#include "../modules/WarehouseUiFlow.hpp"
#include "../modules/WarehouseUi.hpp"
#include "../modules/WarehouseUiCatalog.hpp"
#include "../modules/WarehouseUiCallback.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

// Round-6 callback policy: a consumed move is refreshed in place; the
// native callback is still invoked exactly once and never finalizes a close.
namespace warehouseui {} // namespace warehouseui

namespace {

using warehouse::ArtifactRecord;
using warehouse::Backpack;
using warehouse::NativeBackpack;
using warehouse::State;
using warehouseui::Flow;
using warehouseui::LayoutSide;
using warehouseui::Result;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL warehouse UI flow: %s\n", label);
    }
}

NativeBackpack EmptyNative() {
    NativeBackpack native{};
    native.fill({-1, -1});
    return native;
}

NativeBackpack NativeWithCount(std::size_t count) {
    NativeBackpack native = EmptyNative();
    for (std::size_t i = 0; i < count && i < native.size(); ++i)
        native[i] = {static_cast<std::int32_t>(10 + i),
                     static_cast<std::int32_t>(i)};
    return native;
}

bool SameState(const State& left, const State& right) {
    if (left.mutationEpoch != right.mutationEpoch) return false;
    for (std::size_t i = 0; i < warehouse::kHeroCount; ++i)
        if (left.heroes[i] != right.heroes[i]) return false;
    return true;
}

void CheckLayoutAndPaging() {
    // 面板与完整背包同尺寸 405x459：并排最小宽 = 8+405+8+405+8 = 834。
    for (int width : {834, 1024, 1920}) {
        const auto layout = warehouseui::MakeLayout(width, 600, 8, -20);
        Check(layout.fits, "834/1024/1920 layout fits");
        Check(layout.backpack.height == 459 && layout.warehouse.height == 523,
              "backpack 459 tall; warehouse panel adds tab+pager rows (523)");
        Check(layout.warehouse.width == 405,
              "warehouse panel is exactly the backpack-sized 405 wide");
        Check(layout.backpack.y == 0, "negative y clamps to zero");
        Check(layout.backpack.x >= 0 && layout.warehouse.x >= 0
                  && layout.warehouse.Right() <= width,
              "layout remains inside screen");
        Check(!layout.backpack.Overlaps(layout.warehouse),
              "backpack and warehouse never overlap");
        Check(layout.columns == 8 && layout.page_size == 64,
              "panel mirrors the native 8x8 grid page");
    }
    Check(warehouseui::LayoutPreservesBackpackPosition(
              warehouseui::MakeLayout(1024, 600, 8, 0), 8, 0),
          "native position is accepted when layout preserves it");
    Check(!warehouseui::LayoutPreservesBackpackPosition(
              warehouseui::MakeLayout(900, 600, 500, 500), 500, 500)
              && !warehouseui::LayoutPreservesBackpackPosition(
                  warehouseui::MakeLayout(900, 600, 500, 500), 8, 100),
          "fallback requiring native backpack translation fails closed");
    Check(!warehouseui::MakeLayout(833, 600, 8, 0).fits,
          "width below 834 fails closed");

    const auto right = warehouseui::MakeLayout(1024, 600, 8, 999);
    Check(right.fits && right.side == LayoutSide::Right
              && right.backpack.y == 77,
          "right layout and positive y clamp");

    const auto left = warehouseui::MakeLayout(1024, 600, 500, 20);
    Check(left.fits && left.side == LayoutSide::Left,
          "left fallback is preferred when right is too narrow");
    Check(left.warehouse.Right() + 8 <= left.backpack.x,
          "left panel keeps eight pixel gap");

    const auto moved = warehouseui::MakeLayout(900, 600, 500, 500);
    Check(moved.fits && moved.backpack.x == 8
              && moved.side == LayoutSide::Right,
          "narrow fallback moves backpack to x=8 and uses right panel");
    const auto squeezed = warehouseui::MakeLayout(900, 600, 400, 0);
    Check(squeezed.fits && squeezed.backpack.x == 8
              && squeezed.side == LayoutSide::Right,
          "centered backpack with no side room falls back to left-edge pair");
    Check(!warehouseui::MakeLayout(1024, 522, 8, 0).fits,
          "height below 523 fails closed");
    Check(warehouseui::MakeLayout(900, 600, -100, 0).fits,
          "off-screen preferred position uses safe fallback");

    const std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
    Check(warehouseui::PageCount(0) == 0
              && warehouseui::PageCount(64) == 1
              && warehouseui::PageCount(65) == 2,
          "page boundaries 0/64/65");
    Check(warehouseui::PageBegin(65, 1) == 64
              && warehouseui::PageItemCount(65, 1) == 1,
          "page begin and item count");
    Check(warehouseui::PageCount(maximum)
              == maximum / 64 + (maximum % 64 == 0 ? 0u : 1u),
          "page count is size_t overflow safe");
    const auto last = warehouseui::MakePageView(
        maximum, warehouseui::PageCount(maximum) - 1);
    Check(last.begin <= maximum && last.count > 0 && last.count <= 64,
          "huge final page remains bounded");
    Check(warehouseui::PageBegin(maximum, maximum) == maximum,
          "huge out-of-range page is rejected");
}

void CheckDepositAndCompaction() {
    State state;
    state.heroes[3] = {{90, 1}, {90, 1}, {91, 2}};
    NativeBackpack native = EmptyNative();
    native[0] = {1, 0};
    native[1] = {-1, 55};
    native[2] = {-1, 56};
    native[3] = {42, 9};
    native[10] = {1, 69};
    int count = 3;
    Flow flow(native, count, 3, 77, state.mutationEpoch, 9);
    Check(flow.IsValid(), "flow captures complete native array and count");
    const State stateBeforeSelect = state;
    const NativeBackpack nativeBeforeSelect = native;
    const int countBeforeSelect = count;
    flow.Select(3);
    Check(flow.HasSelection() && flow.SelectedIndex() == 3,
          "Select records index only");
    Check(SameState(stateBeforeSelect, state)
              && nativeBeforeSelect == native && countBeforeSelect == count,
          "Select does not mutate state/native/count");

    Check(flow.Deposit(state, native, count, 3, 77, 9, false, false)
              == Result::Ok,
          "deposit succeeds after complete second verification");
    Check(count == 2 && state.mutationEpoch == 1
              && state.heroes[3].back() == ArtifactRecord{42, 9},
          "deposit commits pair, count and epoch");
    Check(native[0] == ArtifactRecord{1, 0}
              && native[1] == ArtifactRecord{1, 69}
              && native[2] == ArtifactRecord{-1, 55}
              && native[3] == ArtifactRecord{-1, 56}
              && native[4] == ArtifactRecord{-1, -1},
          "deposit compacts records and preserves non-operation empty subtypes");
    Check(!flow.HasSelection(), "successful deposit clears selection");
    Check(flow.Deposit(state, native, count, 3, 77, 9, false, false)
              == Result::DuplicateOperation,
          "duplicate deposit is rejected");

    NativeBackpack duplicateNative = EmptyNative();
    duplicateNative[0] = {1, 4};
    duplicateNative[1] = {1, 4};
    duplicateNative[2] = {1, 5};
    State duplicateState;
    Flow duplicate(duplicateNative, 3, 0, 1, 0, 10);
    duplicate.Select(1);
    int duplicateCount = 3;
    Check(duplicate.Deposit(duplicateState, duplicateNative, duplicateCount,
                            0, 1, 10, false, false) == Result::Ok,
          "duplicate scroll records are independently movable");
    Check(duplicateState.heroes[0] == Backpack{{1, 4}}
              && duplicateNative[0] == ArtifactRecord{1, 4}
              && duplicateNative[1] == ArtifactRecord{1, 5},
          "duplicate scroll and subtype are preserved exactly");
}

void CheckDepositGuards() {
    State state;
    NativeBackpack native = NativeWithCount(3);
    int count = 3;
    Flow flow(native, count, 0, 41, 0, 12);
    flow.Select(1);
    const State stateBefore = state;
    const NativeBackpack nativeBefore = native;
    const int countBefore = count;
    const auto unchanged = [&]() {
        Check(SameState(stateBefore, state) && native == nativeBefore
                  && count == countBefore,
              "failed operation leaves state/native/count/epoch unchanged");
    };

    Check(flow.Deposit(state, native, count, 1, 41, 12, false, false)
              == Result::InvalidHero,
          "cross-hero is rejected");
    unchanged();
    Check(flow.Deposit(state, native, count, 0, 42, 12, false, false)
              == Result::InvalidOwner,
          "cross-owner is rejected");
    unchanged();
    Check(flow.Deposit(state, native, count, 0, 41, 13, false, false)
              == Result::InvalidToken,
          "cross-token is rejected");
    unchanged();
    Check(flow.Deposit(state, native, count, 0, 41, 12, true, false)
              == Result::Locked,
          "locked operation is rejected");
    unchanged();
    Check(flow.Deposit(state, native, count, 0, 41, 12, false, true)
              == Result::HeldArtifact,
          "held operation is rejected");
    unchanged();

    NativeBackpack changedSource = native;
    changedSource[0].subtype = 99;
    Check(flow.Deposit(state, changedSource, count, 0, 41, 12, false, false)
              == Result::StaleSelection,
          "third-party source change is rejected");
    unchanged();

    State changedEpoch = state;
    ++changedEpoch.mutationEpoch;
    const State changedEpochBefore = changedEpoch;
    Check(flow.Deposit(changedEpoch, native, count, 0, 41, 12, false, false)
              == Result::StaleWarehouse,
          "third-party epoch change is rejected");
    Check(SameState(changedEpochBefore, changedEpoch),
          "epoch rejection does not mutate state argument");

    Flow noSelection(native, count, 0, 41, 0, 12);
    Check(noSelection.Deposit(state, native, count, 0, 41, 12, false, false)
              == Result::NoSelection,
          "missing selection is rejected");

    NativeBackpack invalid = EmptyNative();
    invalid[0] = {144, 0};
    Check(!Flow(invalid, 1, 0, 41, 0, 12).IsValid(),
          "artifact id above 143 is invalid");
    invalid[0] = {1, 70};
    Check(!Flow(invalid, 1, 0, 41, 0, 12).IsValid(),
          "scroll subtype above 69 is invalid");
    invalid[0] = {-2, 0};
    Check(!Flow(invalid, 1, 0, 41, 0, 12).IsValid(),
          "negative non-empty game id is invalid");

    NativeBackpack emptyOnly = EmptyNative();
    emptyOnly[0] = {-1, 69};
    Check(warehouseui::ValidateNative(emptyOnly, 0) == Result::Ok,
          "only artifactId == -1 denotes an empty slot");
    Flow emptyFlow(emptyOnly, 0, 0, 41, 0, 12);
    State emptyState;
    NativeBackpack emptyNative = emptyOnly;
    int emptyCount = 0;
    emptyFlow.Select(0);
    Check(emptyFlow.Deposit(emptyState, emptyNative, emptyCount, 0, 41, 12,
                            false, false) == Result::InvalidIndex,
          "selecting an empty slot is not depositable");
}

void CheckRetrieveAndCapacity() {
    State state;
    state.heroes[5] = {{1, 0}, {1, 0}, {77, 3}, {78, 4}};
    NativeBackpack native = EmptyNative();
    native[2] = {20, 2};
    native[4] = {21, 3};
    native[63] = {22, 4};
    int count = 3;
    Flow flow(native, count, 5, 8, 0, 20);
    Check(flow.Retrieve(2, state, native, count, 5, 8, 20, false, false)
              == Result::Ok,
          "retrieve succeeds");
    Check(count == 4 && state.mutationEpoch == 1
              && state.heroes[5] == Backpack{{1, 0}, {1, 0}, {78, 4}},
          "retrieve removes exactly one complete warehouse pair");
    Check(native[0] == ArtifactRecord{77, 3}
              && native[2] == ArtifactRecord{20, 2}
              && native[4] == ArtifactRecord{21, 3}
              && native[63] == ArtifactRecord{22, 4},
          "retrieve fills first hole and preserves complete pairs");

    State fullState;
    fullState.heroes[0] = {{99, 7}};
    NativeBackpack fullNative = NativeWithCount(64);
    const State fullBefore = fullState;
    const NativeBackpack fullNativeBefore = fullNative;
    int fullCount = 64;
    Flow full(fullNative, fullCount, 0, 2, 0, 21);
    Check(full.Retrieve(0, fullState, fullNative, fullCount, 0, 2, 21,
                        false, false) == Result::CapacityReached,
          "full 64-slot pack rejects retrieve");
    Check(SameState(fullBefore, fullState) && fullNative == fullNativeBefore
              && fullCount == 64,
          "full rejection is atomic");

    for (std::size_t size : {std::size_t(64), std::size_t(65), std::size_t(1000)}) {
        State many;
        many.heroes[1].reserve(size);
        for (std::size_t i = 0; i < size; ++i)
            many.heroes[1].push_back({static_cast<std::int32_t>(i % 143),
                                      static_cast<std::int32_t>(i % 70)});
        NativeBackpack one = EmptyNative();
        one[0] = {0, -1};
        int oneCount = 1;
        Flow manyFlow(one, oneCount, 1, 3, 0,
                      static_cast<std::uint64_t>(30 + size));
        manyFlow.Select(0);
        Check(manyFlow.Deposit(many, one, oneCount, 1, 3,
                               static_cast<std::uint64_t>(30 + size), false, false)
                  == Result::Ok && many.heroes[1].size() == size + 1,
              "64/65/1000 warehouse vectors accept records");
    }

    State maxEpoch;
    maxEpoch.heroes[0] = {{5, 1}};
    NativeBackpack empty = EmptyNative();
    int emptyCount = 0;
    maxEpoch.mutationEpoch = (std::numeric_limits<std::uint64_t>::max)();
    Flow maxFlow(empty, emptyCount, 0, 4, maxEpoch.mutationEpoch, 23);
    const State maxBefore = maxEpoch;
    Check(maxFlow.Retrieve(0, maxEpoch, empty, emptyCount, 0, 4, 23,
                           false, false) == Result::EpochExhausted
              && SameState(maxBefore, maxEpoch),
          "epoch max refuses retrieval without wrap");
}

void CheckRetrieveAllBatch() {
    State state;
    state.heroes[5] = {{1, 0}, {1, 0}, {77, 3}, {78, 4}, {79, 5}};
    NativeBackpack native = EmptyNative();
    native[1] = {20, 2};
    native[3] = {21, 3};
    native[7] = {22, 4};
    int count = 3;
    Flow flow(native, count, 5, 8, state.mutationEpoch, 20);
    std::vector<char> include = {0, 0, 1, 1, 1};
    std::size_t moved = 99;
    Check(flow.RetrieveAll(state, native, count, 5, 8, 20, false, false, include,
                           moved) == Result::Ok,
          "retrieve all succeeds");
    Check(moved == 3 && count == 6 && state.mutationEpoch == 1,
          "retrieve all reports every selected move and bumps epoch once");
    Check(state.heroes[5] == Backpack{{1, 0}, {1, 0}},
          "retrieve all removes selected entries in warehouse order");
    Check(native[0] == ArtifactRecord{77, 3}
              && native[1] == ArtifactRecord{20, 2}
              && native[2] == ArtifactRecord{78, 4}
              && native[3] == ArtifactRecord{21, 3}
              && native[4] == ArtifactRecord{79, 5}
              && native[7] == ArtifactRecord{22, 4}
              && native[5] == ArtifactRecord{-1, -1},
          "retrieve all fills native holes in slot order with complete pairs");
    std::vector<char> nextRetrieve(2, 0);
    nextRetrieve[1] = 1;
    Check(flow.RetrieveAll(state, native, count, 5, 8, 20, false, false,
                           nextRetrieve, moved) == Result::Ok && moved == 1,
          "retrieve all refreshes its snapshot for a second operation");
    Check(state.heroes[5] == Backpack{{1, 0}}
              && native[5] == ArtifactRecord{1, 0}
              && count == 7 && state.mutationEpoch == 2,
          "refreshed retrieve snapshot commits the next pair atomically");

    // Capacity is partial: only the first matching entries fit, the rest stay.
    State partialState;
    partialState.heroes[6] = {{30, 0}, {31, 1}, {32, 2}, {33, 3}, {34, 4}};
    NativeBackpack partial = NativeWithCount(64);
    partial[1] = {-1, 123};
    partial[63] = {-1, 456};
    const NativeBackpack partialBefore = partial;
    int partialCount = 62;
    Flow partialFlow(partial, partialCount, 6, 9, partialState.mutationEpoch, 21);
    std::vector<char> partialFilter(5, 1);
    Check(partialFlow.RetrieveAll(partialState, partial, partialCount, 6, 9, 21,
                                  false, false, partialFilter, moved) == Result::Ok,
          "retrieve all partially fills remaining capacity");
    Check(moved == 2 && partialCount == 64 && partialState.mutationEpoch == 1,
          "partial retrieve moves exactly available capacity");
    Check(partialState.heroes[6] == Backpack{{32, 2}, {33, 3}, {34, 4}},
          "partial retrieve retains unmatched tail in original order");
    Check(partial[1] == ArtifactRecord{30, 0}
              && partial[63] == ArtifactRecord{31, 1}
              && partial[0] == partialBefore[0]
              && partial[2] == partialBefore[2]
              && partial[62] == partialBefore[62],
          "partial retrieve preserves untouched native pairs");

    // A matching filter on a full native pack is a hard capacity result and atomic.
    State fullState;
    fullState.heroes[0] = {{100, 2}, {101, 3}};
    NativeBackpack fullNative = NativeWithCount(64);
    int fullCount = 64;
    Flow fullFlow(fullNative, fullCount, 0, 1, fullState.mutationEpoch, 22);
    std::vector<char> fullFilter(2, 1);
    const State fullBefore = fullState;
    const NativeBackpack fullNativeBefore = fullNative;
    Check(fullFlow.RetrieveAll(fullState, fullNative, fullCount, 0, 1, 22, false,
                               false, fullFilter, moved) == Result::CapacityReached,
          "full pack rejects retrieve all with any hit");
    Check(moved == 0 && SameState(fullBefore, fullState)
              && fullNative == fullNativeBefore && fullCount == 64,
          "full retrieve-all rejection is atomic");

    // No hit is Ok and does not require epoch advancement.
    State noHitState;
    noHitState.heroes[2] = {{40, 0}, {41, 1}};
    NativeBackpack noHitNative = EmptyNative();
    noHitNative[0] = {50, 5};
    int noHitCount = 1;
    Flow noHitFlow(noHitNative, noHitCount, 2, 4, noHitState.mutationEpoch, 23);
    std::vector<char> noHit(2, 0);
    const State noHitBefore = noHitState;
    const NativeBackpack noHitNativeBefore = noHitNative;
    Check(noHitFlow.RetrieveAll(noHitState, noHitNative, noHitCount, 2, 4, 23, false,
                                false, noHit, moved) == Result::Ok && moved == 0,
          "no-hit retrieve all is ok");
    Check(SameState(noHitBefore, noHitState)
              && noHitNative == noHitNativeBefore && noHitCount == 1,
          "no-hit retrieve all changes nothing");

    // Length, held/locked, cross identity and stale source all fail unchanged.
    std::vector<char> validFilter(2, 1);
    std::vector<char> shortFilter(1, 1);
    const State guardBefore = noHitState;
    const NativeBackpack guardNativeBefore = noHitNative;
    const int guardCountBefore = noHitCount;
    Check(noHitFlow.RetrieveAll(noHitState, noHitNative, noHitCount, 2, 4, 23, false,
                                false, shortFilter, moved) == Result::InvalidIndex,
          "retrieve-all filter length mismatch is invalid index");
    Check(noHitFlow.RetrieveAll(noHitState, noHitNative, noHitCount, 3, 4, 23, false,
                                false, validFilter, moved) == Result::InvalidHero,
          "retrieve all cross hero is rejected");
    Check(noHitFlow.RetrieveAll(noHitState, noHitNative, noHitCount, 2, 5, 23, false,
                                false, validFilter, moved) == Result::InvalidOwner,
          "retrieve all cross owner is rejected");
    Check(noHitFlow.RetrieveAll(noHitState, noHitNative, noHitCount, 2, 4, 24, false,
                                false, validFilter, moved) == Result::InvalidToken,
          "retrieve all cross token is rejected");
    Check(noHitFlow.RetrieveAll(noHitState, noHitNative, noHitCount, 2, 4, 23, true,
                                false, validFilter, moved) == Result::Locked,
          "retrieve all locked is rejected");
    Check(noHitFlow.RetrieveAll(noHitState, noHitNative, noHitCount, 2, 4, 23, false,
                                true, validFilter, moved) == Result::HeldArtifact,
          "retrieve all held is rejected");
    State staleState = noHitState;
    staleState.heroes[2][0].subtype = 9;
    ++staleState.mutationEpoch;
    const State staleBefore = staleState;
    Check(noHitFlow.RetrieveAll(staleState, noHitNative, noHitCount, 2, 4, 23, false,
                                false, validFilter, moved) == Result::StaleWarehouse,
          "retrieve all detects warehouse epoch snapshot drift");
    Check(SameState(staleBefore, staleState),
          "retrieve all epoch failure preserves changed warehouse");
    NativeBackpack staleNative = noHitNative;
    staleNative[63].subtype = 99;
    const NativeBackpack staleNativeBefore = staleNative;
    Check(noHitFlow.RetrieveAll(noHitState, staleNative, noHitCount, 2, 4, 23, false,
                                false, validFilter, moved) == Result::StaleSelection,
          "retrieve all checks complete native snapshot including empty subtype");
    Check(staleNative == staleNativeBefore,
          "retrieve all stale native failure preserves source argument");
    Check(SameState(guardBefore, noHitState)
              && noHitNative == guardNativeBefore && noHitCount == guardCountBefore,
          "retrieve-all guard failures are atomic");

    State corruptState;
    corruptState.heroes[0] = {{144, 0}};
    NativeBackpack corruptNative = EmptyNative();
    int corruptCount = 0;
    Flow corruptFlow(corruptNative, corruptCount, 0, 1, 0, 30);
    std::vector<char> corruptFilter(1, 1);
    const State corruptBefore = corruptState;
    Check(corruptFlow.RetrieveAll(corruptState, corruptNative, corruptCount, 0, 1, 30,
                                  false, false, corruptFilter, moved)
              == Result::InvalidRecord,
          "invalid warehouse record is rejected");
    Check(SameState(corruptBefore, corruptState) && moved == 0,
          "invalid warehouse record is atomic");
}

void CheckCatalogFilteringAndScrolling() {
    using warehouseui::BaseTab;
    using warehouseui::IsPositionTab;
    using warehouseui::MakeTabWindow;
    using warehouseui::kRegularTab;
    using warehouseui::kTabScrollArtifactId;
    using warehouseui::kSlotSpellbook;
    using warehouseui::TabKind;

    Check(kTabScrollArtifactId == 1 && kRegularTab == 0,
          "scroll id and regular tab constants");
    Check(warehouseui::TabMatches(TabKind::Regular, kTabScrollArtifactId, -1)
              && warehouseui::TabMatches(TabKind::Regular, 0, -1)
              && warehouseui::TabMatches(TabKind::Regular, 143, 5),
          "regular tab is the all-entries gate");
    Check(BaseTab(kTabScrollArtifactId, 5) == TabKind::Scroll,
          "scroll wins over wearable slot");
    Check(!warehouseui::TabMatches(TabKind::Equipment, kTabScrollArtifactId, 5)
              && warehouseui::TabMatches(TabKind::Equipment, 40, 3)
              && !warehouseui::TabMatches(TabKind::Equipment, 41, -2)
              && !warehouseui::TabMatches(TabKind::Equipment, 42, -1),
          "equipment excludes scrolls and slot<0");
    Check(!warehouseui::TabMatches(TabKind::Scroll, 40, -1)
              && !warehouseui::TabMatches(TabKind::Head, 40, -1)
              && !warehouseui::TabMatches(TabKind::Regular, 40, 13)
              && !warehouseui::TabMatches(TabKind::Regular, 40, 14)
              && !warehouseui::TabMatches(TabKind::Regular, 40, 15)
              && !warehouseui::TabMatches(TabKind::Regular, 40, 16)
              && !warehouseui::TabMatches(TabKind::Regular, 40, 17),
          "machine and spellbook slots never appear in warehouse tabs");

    const auto window0 = MakeTabWindow(0, warehouseui::kTabCount, 6);
    Check(window0.begin == 0 && window0.visible == 6 && window0.canPrev == false
              && window0.canNext,
          "tab window starts at regular and scrolls right");
    const auto windowEnd = MakeTabWindow(99, warehouseui::kTabCount, 6);
    Check(windowEnd.begin == warehouseui::kTabCount - 6
              && windowEnd.canNext == false && windowEnd.canPrev,
          "tab offset clamps to the final six");
    const auto wide = MakeTabWindow(0, warehouseui::kTabCount, 99);
    Check(wide.visible == warehouseui::kTabCount && !wide.canPrev && !wide.canNext,
          "window wider than tab list shows all without arrows");
    const auto none = MakeTabWindow(0, warehouseui::kTabCount, 0);
    Check(none.visible == 0 && !none.canPrev && !none.canNext,
          "zero slot window is closed");
    const auto four = MakeTabWindow(8, warehouseui::kTabCount, 4);
    Check(four.begin == 8 && four.visible == 4 && four.canPrev && !four.canNext,
          "production four-tab window reaches the end");
    Check(warehouseui::TabOffsetShift(0, -1, 12, 4) == 0
              && warehouseui::TabOffsetShift(0, 1, 12, 4) == 1
              && warehouseui::TabOffsetShift(8, 1, 12, 4) == 8
              && warehouseui::TabOffsetShift(8, -1, 12, 4) == 7,
          "production tab arrows shift once and clamp both ends");
    Check(warehouseui::TabOffsetShift((std::numeric_limits<std::size_t>::max)(),
                                     1, 12, 4) == 8
              && warehouseui::TabOffsetShift(8, (std::numeric_limits<int>::max)(), 12, 4) == 8
              && warehouseui::TabOffsetShift(8, (std::numeric_limits<int>::min)(), 12, 4) == 0
              && warehouseui::TabOffsetShift(8, 1, 12, 0) == 0,
          "tab shift rejects wraparound and handles zero visible slots");
    const TabKind expectedSlots[] = {
        TabKind::Head, TabKind::Shoulders, TabKind::Neck, TabKind::Weapon,
        TabKind::Shield, TabKind::Torso, TabKind::Ring, TabKind::Ring,
        TabKind::Feet, TabKind::Misc, TabKind::Misc, TabKind::Misc,
        TabKind::Misc, TabKind::Machine, TabKind::Machine, TabKind::Machine,
        TabKind::Machine, TabKind::Spellbook, TabKind::Misc };
    for (int slot = 0; slot < 19; ++slot)
        Check(warehouseui::SlotTab(slot) == expectedSlots[slot],
              "all nineteen original wearable positions map to the correct tab");
    Check(warehouseui::TabMatches(TabKind::Misc, 40, 9)
              && warehouseui::TabMatches(TabKind::Misc, 40, 12)
              && warehouseui::TabMatches(TabKind::Misc, 40, 18)
              && warehouseui::TabMatches(TabKind::Equipment, 40, 9)
              && !warehouseui::TabMatches(TabKind::Misc, 40, 13)
              && !warehouseui::TabMatches(TabKind::Misc, 40, 17),
          "misc slots feed both misc and equipment; machine/spellbook never match");
    Check(IsPositionTab(TabKind::Head) && !IsPositionTab(TabKind::Equipment)
              && !IsPositionTab(TabKind::Regular) && !IsPositionTab(TabKind::Scroll),
          "position tab classification");
}

void CheckRound6CallbackAndIcons() {
    int nativeCalls = 0;
    const int result = warehouseui::CallNativeOnce([&] {
        ++nativeCalls;
        return 7;
    });
    Check(result == 7 && nativeCalls == 1,
          "move refresh calls native callback exactly once");
    const auto empty = warehouseui::MakeIconView(-1, 9, true);
    const auto artifact = warehouseui::MakeIconView(42, 9, true);
    const auto scroll = warehouseui::MakeIconView(1, 9, true);
    const auto ordinaryScroll = warehouseui::MakeIconView(1, 9, false);
    Check(!empty.visible && artifact.visible && artifact.kind == 0
              && artifact.frame == 42,
          "empty and ordinary artifacts map to artifact.def correctly");
    Check(scroll.visible && scroll.kind == 1 && scroll.frame == 9
              && ordinaryScroll.kind == 0 && ordinaryScroll.frame == 1,
          "scrolls select def.SpScrArt only when enabled");
    Check(warehouseui::ConsumeWarehouseDown(30201)
              && warehouseui::ConsumeWarehouseDown(30400)
              && warehouseui::ConsumeWarehouseDown(30403)
              && !warehouseui::ConsumeWarehouseDown(1000)
              && warehouseui::ExecutesWarehouseAction(0xD)
              && !warehouseui::ExecutesWarehouseAction(0xC),
          "warehouse and pager controls consume DOWN and commit one CLICK");
    Check(warehouseui::kPagerButtonCount == 4
              && warehouseui::kPagerButtonWidth == 36
              && warehouseui::kPagerButtonHeight == 22
              && warehouseui::kBackpackGridOriginDY == 50,
          "round-7 pager geometry and backpack grid origin constants");
}

// round-8：原生宝物表 position 是 1-based（0 = 无装备位置），必须先归一化再分类；
// 走查用户实测的三个关键点：头部(1→0)、胸甲(6→5)、戒指(7/8→6/7)、杂物5(19→18)。
void CheckRound8SlotBaseAndSorting() {
    using warehouseui::NormalizeArtifactTableSlot;
    using warehouseui::SlotTab;
    using warehouseui::TabKind;
    Check(NormalizeArtifactTableSlot(0) == warehouseui::kSlotNone
              && NormalizeArtifactTableSlot(-1) == warehouseui::kSlotNone
              && NormalizeArtifactTableSlot(1) == 0
              && NormalizeArtifactTableSlot(6) == 5
              && NormalizeArtifactTableSlot(7) == 6
              && NormalizeArtifactTableSlot(8) == 7
              && NormalizeArtifactTableSlot(19) == 18
              && NormalizeArtifactTableSlot(23) == 23,
          "artifact table slot is decoded as 1-based with 0 meaning no position");
    Check(SlotTab(NormalizeArtifactTableSlot(1)) == TabKind::Head
              && SlotTab(NormalizeArtifactTableSlot(2)) == TabKind::Shoulders
              && SlotTab(NormalizeArtifactTableSlot(3)) == TabKind::Neck
              && SlotTab(NormalizeArtifactTableSlot(6)) == TabKind::Torso
              && SlotTab(NormalizeArtifactTableSlot(7)) == TabKind::Ring
              && SlotTab(NormalizeArtifactTableSlot(8)) == TabKind::Ring
              && SlotTab(NormalizeArtifactTableSlot(19)) == TabKind::Misc,
          "head/cloak/neck/torso/ring/misc5 land on the tabs the user expects");

    // 相同道具必须连续（展示层）。
    State state;
    state.heroes[4] = {{42, 0}, {7, 0}, {42, 0}, {1, 9}, {7, 0}, {1, 3}, {42, 0}};
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < state.heroes[4].size(); ++i) order.push_back(i);
    warehouseui::SortRecordIndicesByIdentity(order, state.heroes[4]);
    // 排序键是 (宝物ID, 子类型)：1/9 与 1/3 同 ID，子类型 3 在前；同键保持原有先后。
    Check(order.size() == 7 && order[0] == 5 && order[1] == 3
              && order[2] == 1 && order[3] == 4 && order[4] == 0
              && order[5] == 2 && order[6] == 6,
          "display order groups identical items and keeps original order inside a group");

    // 存储层整理：直接重排记录本体，空记录排最后，同组保持稳定。
    warehouse::Backpack storage = {{42, 0}, {-1, -1}, {7, 5}, {42, 0}, {7, 1}, {3, 0}};
    warehouseui::SortRecordsByIdentity(storage);
    Check(storage.size() == 6
              && storage[0] == warehouse::ArtifactRecord{3, 0}
              && storage[1] == warehouse::ArtifactRecord{7, 1}
              && storage[2] == warehouse::ArtifactRecord{7, 5}
              && storage[3] == warehouse::ArtifactRecord{42, 0}
              && storage[4] == warehouse::ArtifactRecord{42, 0}
              && storage[5].artifactId < 0,
          "organize sorts the stored records themselves and pushes empty records last");
    // 幂等：再整理一次结果不变。
    const warehouse::Backpack once = storage;
    warehouseui::SortRecordsByIdentity(storage);
    Check(storage == once, "organize is idempotent");
}

void CheckDepositAllBatch() {
    State state;
    state.heroes[2] = {{90, 1}};
    NativeBackpack native = EmptyNative();
    native[0] = {1, 7};
    native[1] = {-1, 55};
    native[2] = {42, 9};
    native[3] = {1, 7};
    native[4] = {-1, 56};
    native[10] = {1, 69};
    int count = 4;
    Flow flow(native, count, 2, 6, state.mutationEpoch, 44);

    std::vector<char> include(64, 0);
    include[0] = 1;
    include[3] = 1;
    include[10] = 1;
    std::size_t moved = 99;
    Check(flow.DepositAll(state, native, count, 2, 6, 44, false, false, include,
                          moved) == Result::Ok,
          "deposit all succeeds");
    Check(moved == 3 && count == 1 && state.mutationEpoch == 1,
          "deposit all reports moved count and bumps epoch once");
    Check(state.heroes[2] == Backpack{{90, 1}, {1, 7}, {1, 7}, {1, 69}},
          "deposit all preserves duplicate scrolls and warehouse append order");
    Check(native[0] == ArtifactRecord{42, 9}
              && native[1] == ArtifactRecord{-1, 55}
              && native[2] == ArtifactRecord{-1, 56}
              && native[3] == ArtifactRecord{-1, -1}
              && native[61] == ArtifactRecord{-1, -1},
          "deposit all compacts and keeps all existing empty subtypes in order");

    // Reuse the same Flow, proving its complete snapshot was refreshed.
    std::vector<char> remaining(64, 0);
    remaining[0] = 1;
    Check(flow.DepositAll(state, native, count, 2, 6, 44, false, false,
                          remaining, moved) == Result::Ok,
          "second deposit all uses refreshed snapshot");
    Check(moved == 1 && count == 0 && state.mutationEpoch == 2
              && state.heroes[2].back() == ArtifactRecord{42, 9},
          "refreshed snapshot accepts the next batch");
    Check(native[0] == ArtifactRecord{-1, 55}
              && native[1] == ArtifactRecord{-1, 56},
          "existing empty subtype remains after second batch");

    // No match at all: Ok, moved 0, no epoch, no change.
    State idleState;
    idleState.heroes[7] = {{5, 5}};
    NativeBackpack idle = EmptyNative();
    idle[0] = {42, 0};
    idle[64 - 1] = {43, 1};
    int idleCount = 2;
    Flow idleFlow(idle, idleCount, 7, 5, idleState.mutationEpoch, 77);
    std::vector<char> none(64, 0);
    std::vector<char> wrongSlots(64, 0);
    wrongSlots[30] = 1;
    const State idleBefore = idleState;
    const NativeBackpack idleNativeBefore = idle;
    const int idleCountBefore = idleCount;
    std::size_t idleMoved = 99;
    Check(idleFlow.DepositAll(idleState, idle, idleCount, 7, 5, 77, false, false,
                              none, idleMoved) == Result::Ok && idleMoved == 0,
          "all-excluded deposit all is ok with zero moves");
    Check(idleFlow.DepositAll(idleState, idle, idleCount, 7, 5, 77, false, false,
                              wrongSlots, idleMoved) == Result::Ok && idleMoved == 0,
          "hole-only selection moves nothing");
    Check(SameState(idleBefore, idleState) && idle == idleNativeBefore
              && idleCount == idleCountBefore,
          "zero-move deposit all changes nothing");

    // Filter length mismatch.
    std::vector<char> shortFilter(63, 0);
    std::vector<char> longFilter(65, 0);
    std::vector<char> emptyFilter;
    std::size_t badMoved = 99;
    Check(idleFlow.DepositAll(idleState, idle, idleCount, 7, 5, 77, false, false,
                              shortFilter, badMoved) == Result::InvalidIndex
              && idleFlow.DepositAll(idleState, idle, idleCount, 7, 5, 77, false,
                                     false, longFilter, badMoved) == Result::InvalidIndex
              && idleFlow.DepositAll(idleState, idle, idleCount, 7, 5, 77, false,
                                     false, emptyFilter, badMoved) == Result::InvalidIndex,
          "filter length must equal 64");
    Check(SameState(idleBefore, idleState) && idle == idleNativeBefore
              && idleCount == idleCountBefore,
          "filter length failure changes nothing");

    // Guard failures, including stale snapshot after a successful batch.
    State guardState;
    guardState.heroes[4] = {{8, 8}};
    NativeBackpack guardNative = EmptyNative();
    guardNative[0] = {10, 0};
    guardNative[1] = {11, 1};
    guardNative[2] = {12, 2};
    int guardCount = 3;
    Flow guard(guardNative, guardCount, 4, 9, guardState.mutationEpoch, 31);
    std::vector<char> guardFilter(64, 0);
    guardFilter[0] = 1;
    guardFilter[1] = 1;
    guardFilter[2] = 1;
    const State guardBefore = guardState;
    const NativeBackpack guardNativeBefore = guardNative;
    const int guardCountBefore = guardCount;
    std::size_t guardMoved = 99;
    Check(guard.DepositAll(guardState, guardNative, guardCount, 5, 9, 31, false,
                           false, guardFilter, guardMoved) == Result::InvalidHero,
          "deposit all cross-hero is rejected");
    Check(guard.DepositAll(guardState, guardNative, guardCount, 4, 10, 31, false,
                           false, guardFilter, guardMoved) == Result::InvalidOwner,
          "deposit all cross-owner is rejected");
    Check(guard.DepositAll(guardState, guardNative, guardCount, 4, 9, 32, false,
                           false, guardFilter, guardMoved) == Result::InvalidToken,
          "deposit all cross-token is rejected");
    Check(guard.DepositAll(guardState, guardNative, guardCount, 4, 9, 31, true,
                           false, guardFilter, guardMoved) == Result::Locked,
          "deposit all locked is rejected");
    Check(guard.DepositAll(guardState, guardNative, guardCount, 4, 9, 31, false,
                           true, guardFilter, guardMoved) == Result::HeldArtifact,
          "deposit all held is rejected");
    NativeBackpack drifted = guardNativeBefore;
    drifted[1].subtype = 77;
    Check(guard.DepositAll(guardState, drifted, guardCount, 4, 9, 31, false, false,
                           guardFilter, guardMoved) == Result::StaleSelection,
          "deposit all source drift is rejected");
    State driftedEpoch = guardBefore;
    ++driftedEpoch.mutationEpoch;
    const State driftedEpochBefore = driftedEpoch;
    Check(guard.DepositAll(driftedEpoch, guardNative, guardCount, 4, 9, 31, false,
                           false, guardFilter, guardMoved) == Result::StaleWarehouse,
          "deposit all epoch drift is rejected");
    Check(SameState(driftedEpochBefore, driftedEpoch),
          "deposit all stale epoch does not mutate state");
    Check(SameState(guardBefore, guardState)
              && guardNative == guardNativeBefore
              && guardCount == guardCountBefore && guardMoved == 0,
          "deposit all guard failures are atomic");

    // Full 64 selected: entire pack compacts out, epoch advances once.
    State fullState;
    fullState.heroes[0] = {{1, 1}};
    NativeBackpack full = NativeWithCount(64);
    int fullCount = 64;
    std::vector<char> fullFilter(64, 1);
    std::size_t fullMoved = 0;
    Flow fullFlow(full, fullCount, 0, 1, fullState.mutationEpoch, 51);
    Check(fullFlow.DepositAll(fullState, full, fullCount, 0, 1, 51, false, false,
                              fullFilter, fullMoved) == Result::Ok,
          "deposit all accepts a full 64 selection");
    Check(fullMoved == 64 && fullCount == 0 && fullState.mutationEpoch == 1
              && fullState.heroes[0].size() == 65
              && fullState.heroes[0].back() == ArtifactRecord{73, 63},
          "full batch appends every occupied pair in slot order");
    bool anyLeft = false;
    for (const auto& slot : full)
        if (slot.artifactId != -1) anyLeft = true;
    Check(!anyLeft, "full batch leaves no occupied native slot");

    // Exhausted epoch refuses either nonempty transaction, without wrapping.
    State exhausted;
    exhausted.mutationEpoch = (std::numeric_limits<std::uint64_t>::max)();
    exhausted.heroes[1] = {{30, 1}, {31, 2}};
    NativeBackpack epochNative = EmptyNative();
    int epochCount = 0;
    const State epochBefore = exhausted;
    const NativeBackpack epochNativeBefore = epochNative;
    Flow epochFlow(epochNative, epochCount, 1, 2, exhausted.mutationEpoch, 62);
    std::vector<char> retrieveFilter(2, 1);
    Check(epochFlow.RetrieveAll(exhausted, epochNative, epochCount, 1, 2, 62,
                                false, false, retrieveFilter, moved)
              == Result::EpochExhausted && moved == 0,
          "exhausted epoch refuses batch retrieval");
    Check(SameState(epochBefore, exhausted) && epochNative == epochNativeBefore
              && epochCount == 0 && epochFlow.WarehouseEpoch() == exhausted.mutationEpoch,
          "exhausted retrieval preserves state/native/count/snapshot");
    NativeBackpack depNative = EmptyNative();
    depNative[0] = {40, (std::numeric_limits<std::int32_t>::min)()};
    depNative[1] = {41, (std::numeric_limits<std::int32_t>::max)()};
    const NativeBackpack depNativeBefore = depNative;
    int depCount = 2;
    Flow depFlow(depNative, depCount, 1, 2, exhausted.mutationEpoch, 63);
    std::vector<char> depFilter(64, 1);
    Check(depFlow.DepositAll(exhausted, depNative, depCount, 1, 2, 63, false, false,
                             depFilter, moved) == Result::EpochExhausted && moved == 0,
          "exhausted epoch refuses batch deposit");
    Check(SameState(epochBefore, exhausted) && depNative == depNativeBefore
              && depCount == 2 && depFlow.Native() == depNativeBefore,
          "exhausted deposit preserves state/native/count/snapshot");
}

} // namespace

void RunWarehouseUiFlowTests_() {
    g_checks = 0;
    g_failures = 0;
    CheckLayoutAndPaging();
    CheckDepositAndCompaction();
    CheckDepositGuards();
    CheckRetrieveAndCapacity();
    CheckRetrieveAllBatch();
    CheckDepositAllBatch();
    CheckRound6CallbackAndIcons();
    CheckCatalogFilteringAndScrolling();
    CheckRound8SlotBaseAndSorting();

    // 置顶仓库层的点击过滤契约（纯函数，离线可回归）：
    using warehouseui::IsNativeBackpackId;
    using warehouseui::IsWarehouseLeftGesture;
    using warehouseui::SuppressPinnedClose;
    Check(IsWarehouseLeftGesture(0x200, 0xC) && IsWarehouseLeftGesture(0x200, 0xD)
              && !IsWarehouseLeftGesture(0x200, 0xE) && !IsWarehouseLeftGesture(0x200, 0xA)
              && !IsWarehouseLeftGesture(2, 1),
          "left gesture only covers 0x200/C and 0x200/D");
    Check(IsNativeBackpackId(1000) && IsNativeBackpackId(1063)
              && IsNativeBackpackId(2000) && IsNativeBackpackId(2063)
              && !IsNativeBackpackId(999) && !IsNativeBackpackId(1064)
              && !IsNativeBackpackId(30000) && !IsNativeBackpackId(30200),
          "native backpack ids cover 1000..1063 and 2000..2063 only");
    Check(!SuppressPinnedClose(false, 0x200, 0xC, 777)
              && !SuppressPinnedClose(false, 0x200, 0xD, 0),
          "collapsed state never suppresses native close");
    Check(SuppressPinnedClose(true, 0x200, 0xD, 0)
              && SuppressPinnedClose(true, 0x200, 0xC, 1005)
              && SuppressPinnedClose(true, 0x200, 0xC, 2005)
              && SuppressPinnedClose(true, 0x200, 0xC, 30004)
              && SuppressPinnedClose(true, 0x200, 0xC, 30200)
              && SuppressPinnedClose(true, 0x200, 0xC, 777)
              && !SuppressPinnedClose(true, 0x8, 0, 0)
              && !SuppressPinnedClose(true, 0x200, 0xA, 777)
              && !SuppressPinnedClose(true, 0x200, 0xE, 777)
              && !SuppressPinnedClose(true, 2, 1, 0),
          "expanded mode neutralizes both left gestures for every id "
          "(empty-cell presses used to close) while EndDialog/right-click/Esc still pass through");
    // round-8：展开态左键按下全部由仓库接管；未展开时不得吞原生按下。
    Check(warehouseui::ConsumeExpandedLeftDown(true, true)
              && !warehouseui::ConsumeExpandedLeftDown(true, false)
              && !warehouseui::ConsumeExpandedLeftDown(false, true),
          "expanded mode consumes every left-button down, collapsed mode does not");
    // round-8：空槽没有原生命中项，点坐标→槽位必须与原生 8x8 网格一致。
    {
        const int originX = 18, originY = 50;
        Check(warehouseui::GridSlotFromPoint(originX, originY, originX, originY,
                  46, 8, 8, 64) == 0
                  && warehouseui::GridSlotFromPoint(originX + 45, originY + 45, originX,
                      originY, 46, 8, 8, 64) == 0
                  && warehouseui::GridSlotFromPoint(originX + 46, originY, originX,
                      originY, 46, 8, 8, 64) == 1
                  && warehouseui::GridSlotFromPoint(originX, originY + 46, originX,
                      originY, 46, 8, 8, 64) == 8
                  && warehouseui::GridSlotFromPoint(originX + 7 * 46, originY + 7 * 46,
                      originX, originY, 46, 8, 8, 64) == 63
                  && warehouseui::GridSlotFromPoint(originX - 1, originY, originX,
                      originY, 46, 8, 8, 64) == -1
                  && warehouseui::GridSlotFromPoint(originX, originY - 1, originX,
                      originY, 46, 8, 8, 64) == -1
                  && warehouseui::GridSlotFromPoint(originX + 8 * 46, originY, originX,
                      originY, 46, 8, 8, 64) == -1
                  && warehouseui::GridSlotFromPoint(originX, originY + 8 * 46, originX,
                      originY, 46, 8, 8, 64) == -1,
              "backpack point maps onto the native 8x8 grid and rejects the outside");
    }
    // round-8：整理按钮纳入仓库控件集（按下同样只中和，动作只在 CLICK 执行）。
    Check(warehouseui::IsWarehouseControlId(30052)
              && warehouseui::ConsumeWarehouseDown(30052)
              && !warehouseui::IsWarehouseControlId(30005),
          "organize button id 30052 joins the warehouse control set");
    // round-8.1（实机回归）：打开按钮在背包面板矩形内，几何兜底必须让位于已知控件。
    Check(!warehouseui::UsesGeometryFallback(30200)
              && !warehouseui::UsesGeometryFallback(30060)
              && !warehouseui::UsesGeometryFallback(30052)
              && !warehouseui::UsesGeometryFallback(30400)
              && !warehouseui::UsesGeometryFallback(30100)
              && warehouseui::UsesGeometryFallback(1005)
              && warehouseui::UsesGeometryFallback(2005)
              && warehouseui::UsesGeometryFallback(0)
              && warehouseui::UsesGeometryFallback(-1),
          "geometry fallback yields to known warehouse control ids "
          "(the open button lives inside the backpack rect)");

    if (g_failures != 0) {
        std::fprintf(stderr, "warehouse UI flow: %d/%d checks failed\n",
                     g_failures, g_checks);
        std::exit(1);
    }
    std::printf("PASS warehouse UI flow: %d checks\n", g_checks);
}
