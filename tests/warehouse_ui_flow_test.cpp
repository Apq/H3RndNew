#include "../modules/WarehouseUiFlow.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

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
    for (int width : {640, 800, 1920}) {
        const auto layout = warehouseui::MakeLayout(width, 480, 8, -20);
        Check(layout.fits, "640/800/1920 layout fits");
        Check(layout.backpack.height == 459 && layout.warehouse.height == 459,
              "backpack and warehouse heights are 459");
        Check(layout.backpack.y == 0, "negative y clamps to zero");
        Check(layout.backpack.x >= 0 && layout.warehouse.x >= 0
                  && layout.warehouse.Right() <= width,
              "layout remains inside screen");
        Check(!layout.backpack.Overlaps(layout.warehouse),
              "backpack and warehouse never overlap");
        Check(layout.warehouse.width >= 160 && layout.columns >= 3
                  && layout.page_size == 24,
              "panel width and fixed 3x8 page are valid");
    }

    const auto right = warehouseui::MakeLayout(800, 480, 8, 999);
    Check(right.fits && right.side == LayoutSide::Right
              && right.backpack.y == 21,
          "right layout and positive y clamp");

    const auto left = warehouseui::MakeLayout(800, 480, 300, 20);
    Check(left.fits && left.side == LayoutSide::Left,
          "left fallback is preferred when right is too narrow");
    Check(left.warehouse.Right() + 8 <= left.backpack.x,
          "left panel keeps eight pixel gap");

    const auto moved = warehouseui::MakeLayout(640, 480, 500, 500);
    Check(moved.fits && moved.backpack.x == 8
              && moved.side == LayoutSide::Right,
          "640 fallback moves backpack to x=8 and uses right panel");
    Check(!warehouseui::MakeLayout(580, 480, 8, 0).fits,
          "width below 581 fails closed");
    Check(!warehouseui::MakeLayout(640, 458, 8, 0).fits,
          "height below 459 fails closed");
    Check(warehouseui::MakeLayout(640, 480, -100, 0).fits,
          "off-screen preferred position uses safe fallback");

    const std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
    Check(warehouseui::PageCount(0) == 0
              && warehouseui::PageCount(24) == 1
              && warehouseui::PageCount(25) == 2,
          "page boundaries 0/24/25");
    Check(warehouseui::PageBegin(25, 1) == 24
              && warehouseui::PageItemCount(25, 1) == 1,
          "page begin and item count");
    Check(warehouseui::PageCount(maximum)
              == maximum / 24 + (maximum % 24 == 0 ? 0u : 1u),
          "page count is size_t overflow safe");
    const auto last = warehouseui::MakePageView(
        maximum, warehouseui::PageCount(maximum) - 1);
    Check(last.begin <= maximum && last.count > 0 && last.count <= 24,
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

} // namespace

void RunWarehouseUiFlowTests_() {
    g_checks = 0;
    g_failures = 0;
    CheckLayoutAndPaging();
    CheckDepositAndCompaction();
    CheckDepositGuards();
    CheckRetrieveAndCapacity();
    if (g_failures != 0) {
        std::fprintf(stderr, "warehouse UI flow: %d/%d checks failed\n",
                     g_failures, g_checks);
        std::exit(1);
    }
    std::printf("PASS warehouse UI flow: %d checks\n", g_checks);
}
