#include "../modules/WarehouseCore.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <malloc.h>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

// Test executable ONLY (Windows CRT); link this translation unit exactly once.
// Do not link another replacement new/delete shim, nor include it in the DLL.
// Disabled by default; only the arming thread is affected. Prepare fixtures and
// snapshots first, arm only around the tested call, disarm before Check/logging.
// Do not start background work while armed; malloc/native heaps are not injected.
thread_local std::size_t g_failAfter = (std::numeric_limits<std::size_t>::max)();
thread_local std::size_t g_forcedFailures = 0;

void* Allocate_(std::size_t size) {
    if (g_failAfter == 0) {
        ++g_forcedFailures;
        throw std::bad_alloc();
    }
    if (g_failAfter != (std::numeric_limits<std::size_t>::max)()) --g_failAfter;
    void* result = std::malloc(size == 0 ? 1 : size);
    if (result == nullptr) throw std::bad_alloc();
    return result;
}

void* AllocateAligned_(std::size_t size, std::size_t alignment) {
    if (g_failAfter == 0) {
        ++g_forcedFailures;
        throw std::bad_alloc();
    }
    if (g_failAfter != (std::numeric_limits<std::size_t>::max)()) --g_failAfter;
    void* result = _aligned_malloc(size == 0 ? 1 : size, alignment);
    if (result == nullptr) throw std::bad_alloc();
    return result;
}

} // namespace

void* operator new(std::size_t size) { return Allocate_(size); }
void* operator new[](std::size_t size) { return Allocate_(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return Allocate_(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return Allocate_(size); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    return AllocateAligned_(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return AllocateAligned_(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try { return AllocateAligned_(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try { return AllocateAligned_(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    _aligned_free(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    _aligned_free(p);
}

extern "C" void WarehouseTestArmAllocationFailure_(std::size_t successfulAllocations) noexcept {
    g_failAfter = successfulAllocations;
    g_forcedFailures = 0;
}

extern "C" std::size_t WarehouseTestDisarmAllocationFailure_() noexcept {
    const std::size_t failures = g_forcedFailures;
    g_failAfter = (std::numeric_limits<std::size_t>::max)();
    g_forcedFailures = 0;
    return failures;
}

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL warehouse core: %s\n", label);
    }
}

using warehouse::ArtifactRecord;
using warehouse::Backpack;
using warehouse::Inventory;
using warehouse::NativeBackpack;
using warehouse::PendingSelection;
using warehouse::Result;
using warehouse::RetrieveSummary;
using warehouse::State;

bool SameInventory(const Inventory& left, const Inventory& right) {
    for (std::size_t hero = 0; hero < warehouse::kHeroCount; ++hero)
        if (left[hero] != right[hero]) return false;
    return true;
}

bool SameState(const State& left, const State& right) {
    return left.mutationEpoch == right.mutationEpoch
        && SameInventory(left.heroes, right.heroes);
}

struct Snapshot {
    State state;
    Backpack backpack;
};

Snapshot TakeSnapshot(const State& state, const Backpack& backpack) {
    return {state, backpack};
}

bool SameSnapshot(const Snapshot& snapshot, const State& state,
                  const Backpack& backpack) {
    return SameState(snapshot.state, state) && snapshot.backpack == backpack;
}

Backpack MakeBackpack(std::size_t count, std::int32_t first = 10) {
    Backpack result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        result.push_back({static_cast<std::int32_t>(first + static_cast<std::int32_t>(i)),
                          static_cast<std::int32_t>(i)});
    return result;
}

std::vector<ArtifactRecord> Join(const Backpack& left, const Backpack& right) {
    std::vector<ArtifactRecord> result;
    result.reserve(left.size() + right.size());
    result.insert(result.end(), left.begin(), left.end());
    result.insert(result.end(), right.begin(), right.end());
    return result;
}

bool SameMultiset(Backpack left, Backpack right) {
    std::sort(left.begin(), left.end(), [](const ArtifactRecord& a,
                                           const ArtifactRecord& b) {
        return a.artifactId < b.artifactId
            || (a.artifactId == b.artifactId && a.subtype < b.subtype);
    });
    std::sort(right.begin(), right.end(), [](const ArtifactRecord& a,
                                             const ArtifactRecord& b) {
        return a.artifactId < b.artifactId
            || (a.artifactId == b.artifactId && a.subtype < b.subtype);
    });
    return left == right;
}

void TestConstantsAndRecordRules() {
    Check(warehouse::kHeroCount == 156 && warehouse::kBackpackCapacity == 64,
          "fixed 156 heroes and 64 backpack capacity");
    Check(!warehouse::IsValidHeroId(-1) && warehouse::IsValidHeroId(0)
          && warehouse::IsValidHeroId(155) && !warehouse::IsValidHeroId(156),
          "hero ids -1, 0, 155 and 156");
    Check(warehouse::detail::CanGrowRecordCount(0, 64, 64)
          && warehouse::detail::CanGrowRecordCount(63, 1, 64)
          && !warehouse::detail::CanGrowRecordCount(64, 1, 64)
          && !warehouse::detail::CanGrowRecordCount((std::numeric_limits<std::size_t>::max)(), 1,
                                                     (std::numeric_limits<std::size_t>::max)()),
          "checked count growth and overflow");
    Check(warehouse::IsValidRecord({0, (std::numeric_limits<std::int32_t>::min)()})
          && warehouse::IsValidRecord({155, (std::numeric_limits<std::int32_t>::max)()})
          && !warehouse::IsValidRecord({-1, 7}),
          "negative compact artifact ids rejected, subtype opaque");
    Check(ArtifactRecord{42, (std::numeric_limits<std::int32_t>::min)()}
          != ArtifactRecord{42, (std::numeric_limits<std::int32_t>::max)()},
          "full int32 pair comparison keeps subtype");
}

void TestNativeImport() {
    NativeBackpack slots{};
    Backpack output = {{901, 77}};
    Check(warehouse::ImportNativeBackpack(slots, 0, output) == Result::Ok
          && output.empty(), "native empty slots import as empty compact backpack");

    for (std::size_t count : {std::size_t(1), std::size_t(63), std::size_t(64)}) {
        slots = {};
        for (std::size_t i = 0; i < count; ++i)
            slots[i] = {static_cast<std::int32_t>(1000 + i),
                        i == 0 ? (std::numeric_limits<std::int32_t>::min)()
                                : (i == 1 ? (std::numeric_limits<std::int32_t>::max)()
                                           : static_cast<std::int32_t>(i))};
        output = {{777, 8}};
        const Result result = warehouse::ImportNativeBackpack(
            slots, static_cast<std::int32_t>(count), output);
        Check(result == Result::Ok && output.size() == count,
              "native declared count 1/63/64 accepted");
        for (std::size_t i = 0; i < count; ++i)
            Check(output.size() > i && output[i] == slots[i],
                  "native order and complete pair preserved");
    }

    slots = {};
    slots[0] = {55, 9};
    output = {{901, 77}};
    const Backpack before = output;
    Check(warehouse::ImportNativeBackpack(slots, 0, output) == Result::InvalidBackpack
          && output == before, "occupied count mismatch rejected without mutation");
    Check(warehouse::ImportNativeBackpack(slots, 65, output) == Result::InvalidBackpack
          && output == before, "declared count 65 rejected without mutation");
    Check(warehouse::ImportNativeBackpack(slots, -1, output) == Result::InvalidBackpack
          && output == before, "negative declared count rejected without mutation");

    slots = {};
    slots[0] = {-2, 123};
    Check(warehouse::ImportNativeBackpack(slots, 0, output) == Result::InvalidRecord
          && output == before, "negative native id below empty sentinel rejected");

    slots = {};
    slots[0] = {-1, (std::numeric_limits<std::int32_t>::max)()};
    slots[1] = {12, (std::numeric_limits<std::int32_t>::min)()};
    output = before;
    Check(warehouse::ImportNativeBackpack(slots, 1, output) == Result::Ok
          && output == Backpack{{12, (std::numeric_limits<std::int32_t>::min)()}},
          "only exact -1 is empty and empty subtype is filtered");

    slots = {};
    slots[0] = {33, (std::numeric_limits<std::int32_t>::max)()};
    output = before;
    const Snapshot outputSnapshot{{}, output};
    WarehouseTestArmAllocationFailure_(0);
    const Result allocation = warehouse::ImportNativeBackpack(slots, 1, output);
    const std::size_t forced = WarehouseTestDisarmAllocationFailure_();
    Check(allocation == Result::AllocationFailure && forced != 0
          && output == outputSnapshot.backpack,
          "native import bad_alloc rolls output back");
}

void TestMoveAll() {
    for (std::size_t count : {std::size_t(0), std::size_t(1), std::size_t(63), std::size_t(64)}) {
        State state;
        Backpack backpack = MakeBackpack(count, 200);
        const Backpack original = backpack;
        const std::uint64_t epoch = state.mutationEpoch;
        Check(warehouse::MoveAllToWarehouse(state, backpack, 155) == Result::Ok,
              "move-all accepts 0/1/63/64 slots");
        Check(backpack.empty() && state.heroes[155] == original
              && state.mutationEpoch == (count == 0 ? epoch : epoch + 1),
              "move-all preserves order, subtype and empty no-op epoch");
    }

    State state;
    Backpack duplicate = {{9, 1}, {9, 1}, {9, 2}, {9, 2},
                          {10, (std::numeric_limits<std::int32_t>::min)()}};
    const Backpack duplicateBefore = duplicate;
    Check(warehouse::MoveAllToWarehouse(state, duplicate, 0) == Result::Ok
          && duplicate.empty() && state.heroes[0] == duplicateBefore,
          "duplicate records and arbitrary subtypes are never deduplicated");

    State invalidState;
    Backpack over = MakeBackpack(65);
    const Snapshot overSnapshot = TakeSnapshot(invalidState, over);
    Check(warehouse::MoveAllToWarehouse(invalidState, over, 0) == Result::InvalidBackpack
          && SameSnapshot(overSnapshot, invalidState, over),
          "65-slot compact backpack rejected unchanged");

    Backpack badId = {{-1, 4}};
    const Snapshot badIdSnapshot = TakeSnapshot(invalidState, badId);
    Check(warehouse::MoveAllToWarehouse(invalidState, badId, 0) == Result::InvalidRecord
          && SameSnapshot(badIdSnapshot, invalidState, badId),
          "negative compact id rejected unchanged");
    Check(warehouse::MoveAllToWarehouse(invalidState, badId, -1) == Result::InvalidHero
          && warehouse::MoveAllToWarehouse(invalidState, badId, 156) == Result::InvalidHero,
          "move-all rejects hero -1 and 156");

    State aliasState;
    aliasState.heroes[0] = {{1, 2}};
    const Snapshot aliasSnapshot = TakeSnapshot(aliasState, aliasState.heroes[0]);
    Check(warehouse::MoveAllToWarehouse(aliasState, aliasState.heroes[0], 0)
          == Result::AliasedBackpack
          && SameSnapshot(aliasSnapshot, aliasState, aliasState.heroes[0]),
          "backpack aliasing state.heroes is rejected");

    State allocationState;
    Backpack allocationPack = {{77, 8}};
    const Snapshot allocationSnapshot = TakeSnapshot(allocationState, allocationPack);
    WarehouseTestArmAllocationFailure_(0);
    const Result allocation = warehouse::MoveAllToWarehouse(allocationState, allocationPack, 0);
    const std::size_t forced = WarehouseTestDisarmAllocationFailure_();
    Check(allocation == Result::AllocationFailure && forced != 0
          && SameSnapshot(allocationSnapshot, allocationState, allocationPack),
          "move-all bad_alloc rolls both sides back");

    State overflowState;
    overflowState.mutationEpoch = (std::numeric_limits<std::uint64_t>::max)();
    Backpack one = {{8, 3}};
    const Snapshot overflowSnapshot = TakeSnapshot(overflowState, one);
    Check(warehouse::MoveAllToWarehouse(overflowState, one, 0) == Result::EpochExhausted
          && SameSnapshot(overflowSnapshot, overflowState, one),
          "epoch overflow rejects mutation unchanged");
}

void TestSelection() {
    State state;
    Backpack backpack = {{8, 3}, {8, 4}, {9, 5}};
    PendingSelection selection;
    const std::uint64_t epoch = state.mutationEpoch;
    Check(warehouse::SelectBackpackItem(state, backpack, 0, 41, 1, selection) == Result::Ok
          && selection.active && selection.record == ArtifactRecord{8, 4}
          && state.mutationEpoch == epoch,
          "selection binds complete pair, hero, owner and epoch");
    const Snapshot beforeWrongOwner = TakeSnapshot(state, backpack);
    Check(warehouse::MoveOneToWarehouse(state, backpack, 0, 42, selection)
          == Result::StaleSelection
          && SameSnapshot(beforeWrongOwner, state, backpack),
          "owner mismatch rejects selected item");
    Check(warehouse::MoveOneToWarehouse(state, backpack, 1, 41, selection)
          == Result::StaleSelection
          && SameSnapshot(beforeWrongOwner, state, backpack),
          "cross-hero current selection is rejected");

    Check(warehouse::MoveOneToWarehouse(state, backpack, 0, 41, selection) == Result::Ok
          && backpack == Backpack{{8, 3}, {9, 5}}
          && state.heroes[0] == Backpack{{8, 4}},
          "valid selection moves one exact entry");
    const Snapshot afterMove = TakeSnapshot(state, backpack);
    Check(warehouse::MoveOneToWarehouse(state, backpack, 0, 41, selection)
          == Result::StaleSelection
          && SameSnapshot(afterMove, state, backpack),
          "selection cannot be replayed after epoch mutation");

    State staleState;
    Backpack stalePack = {{11, 1}, {12, 2}};
    PendingSelection stale;
    Check(warehouse::SelectBackpackItem(staleState, stalePack, 155, 7, 0, stale) == Result::Ok,
          "selection for hero 155 succeeds");
    Check(warehouse::InvalidateSelections(staleState) == Result::Ok,
          "external lifecycle invalidation advances epoch");
    const Snapshot staleSnapshot = TakeSnapshot(staleState, stalePack);
    Check(warehouse::MoveOneToWarehouse(staleState, stalePack, 155, 7, stale)
          == Result::StaleSelection
          && SameSnapshot(staleSnapshot, staleState, stalePack),
          "cancel/reopen epoch makes selection stale");

    State otherState;
    Backpack otherPack = stalePack;
    PendingSelection otherSelection;
    Check(warehouse::SelectBackpackItem(otherState, otherPack, 0, 7, 0, otherSelection)
          == Result::Ok, "selection source binds state identity");
    const Snapshot otherSnapshot = TakeSnapshot(otherState, otherPack);
    Check(warehouse::MoveOneToWarehouse(otherState, stalePack, 0, 7, otherSelection)
          == Result::StaleSelection,
          "equal-valued different backpack cannot consume selection");
    Check(SameSnapshot(otherSnapshot, otherState, otherPack),
          "different backpack rejection leaves source unchanged");

    PendingSelection changed;
    Backpack changedPack = {{20, 1}, {21, 2}};
    State changedState;
    Check(warehouse::SelectBackpackItem(changedState, changedPack, 0, 7, 0, changed)
          == Result::Ok, "selection snapshot created");
    changedPack[1].subtype = 99;
    const Snapshot changedSnapshot = TakeSnapshot(changedState, changedPack);
    Check(warehouse::MoveOneToWarehouse(changedState, changedPack, 0, 7, changed)
          == Result::StaleSelection
          && SameSnapshot(changedSnapshot, changedState, changedPack),
          "same-size changed entry invalidates complete backpack snapshot");
    warehouse::CancelSelection(changed);
    Check(!changed.active && warehouse::MoveOneToWarehouse(changedState, changedPack, 0, 7, changed)
          == Result::StaleSelection, "cancelled selection cannot be used");

    State aliasState;
    PendingSelection aliasSelection;
    aliasState.heroes[0] = {{1, 2}};
    Check(warehouse::SelectBackpackItem(aliasState, aliasState.heroes[0], 0, 1, 0,
                                        aliasSelection) == Result::AliasedBackpack,
          "selection rejects backpack alias with state.heroes");

    State allocationState;
    Backpack allocationPack = {{31, 4}};
    PendingSelection allocationSelection;
    Check(warehouse::SelectBackpackItem(allocationState, allocationPack, 0, 1, 0,
                                        allocationSelection) == Result::Ok,
          "selection prepared for allocation rollback");
    const Snapshot allocationSnapshot = TakeSnapshot(allocationState, allocationPack);
    WarehouseTestArmAllocationFailure_(0);
    const Result allocation = warehouse::MoveOneToWarehouse(
        allocationState, allocationPack, 0, 1, allocationSelection);
    const std::size_t forced = WarehouseTestDisarmAllocationFailure_();
    Check(allocation == Result::AllocationFailure && forced != 0
          && SameSnapshot(allocationSnapshot, allocationState, allocationPack),
          "single move bad_alloc rolls both sides back");

    State maxEpoch;
    maxEpoch.mutationEpoch = (std::numeric_limits<std::uint64_t>::max)();
    Backpack maxPack = {{1, 1}};
    PendingSelection maxSelection;
    Check(warehouse::SelectBackpackItem(maxEpoch, maxPack, 0, 1, 0, maxSelection)
          == Result::Ok, "selection can observe max epoch without mutation");
    const Snapshot maxSnapshot = TakeSnapshot(maxEpoch, maxPack);
    Check(warehouse::MoveOneToWarehouse(maxEpoch, maxPack, 0, 1, maxSelection)
          == Result::EpochExhausted
          && SameSnapshot(maxSnapshot, maxEpoch, maxPack),
          "single move refuses epoch wrap");
}

// Predicate counters below are test-only observation probes. Production
// predicates must not mutate model/native/external state (Core contract).
void TestRetrieve() {
    for (std::size_t initialCount : {std::size_t(0), std::size_t(1),
                                     std::size_t(63), std::size_t(64)}) {
        State boundaryState;
        const std::size_t additions = warehouse::kBackpackCapacity - initialCount;
        Backpack boundaryPack = MakeBackpack(initialCount, 1000);
        boundaryState.heroes[0] = MakeBackpack(additions, 2000);
        const Backpack allBefore = Join(boundaryState.heroes[0], boundaryPack);
        RetrieveSummary boundarySummary{1, 1};
        std::size_t calls = 0;
        const Result boundaryResult = warehouse::RetrieveUpToCapacity(
            boundaryState, boundaryPack, 0,
            [&calls](const ArtifactRecord&) noexcept { ++calls; return true; },
            boundarySummary);
        Check(boundaryResult == Result::Ok && boundaryPack.size() == 64
              && boundarySummary.moved == additions && boundarySummary.rejected == 0
              && calls == additions && boundaryState.heroes[0].empty(),
              "retrieve fills remaining capacity from 0/1/63/64");
        Check(SameMultiset(allBefore, boundaryPack),
              "boundary retrieval conserves the full record multiset");
    }

    State state;
    state.heroes[155] = {{10, 1}, {11, 7}, {12, (std::numeric_limits<std::int32_t>::min)()},
                         {13, (std::numeric_limits<std::int32_t>::max)()}, {14, 123}};
    Backpack backpack = MakeBackpack(62, 500);
    const Backpack beforeWarehouse = state.heroes[155];
    const Backpack beforeBackpack = backpack;
    int predicateCalls = 0;
    RetrieveSummary summary{99, 88};
    const Result result = warehouse::RetrieveUpToCapacity(
        state, backpack, 155,
        [&predicateCalls](const ArtifactRecord& record) noexcept {
            ++predicateCalls;
            return record.artifactId == 12 || record.artifactId == 13;
        }, summary);
    Check(result == Result::Ok && summary.moved == 2 && summary.rejected == 2
          && predicateCalls == 4 && backpack.size() == 64,
          "retrieve skips rejected records, moves accepted items and stops at 64 slots");
    Check(state.heroes[155] == Backpack{{10, 1}, {11, 7}, {14, 123}}
          && backpack == [&]() { Backpack expected = beforeBackpack;
                                 expected.push_back({12, (std::numeric_limits<std::int32_t>::min)()});
                                 expected.push_back({13, (std::numeric_limits<std::int32_t>::max)()});
                                 return expected; }(),
          "retrieve preserves complete pairs and source order");
    Check(SameMultiset(Join(beforeWarehouse, beforeBackpack),
                       Join(state.heroes[155], backpack)),
          "retrieve conserves full record multiset");

    State fullState;
    fullState.heroes[0] = {{1, 1}};
    Backpack fullPack = MakeBackpack(64);
    const Snapshot fullSnapshot = TakeSnapshot(fullState, fullPack);
    bool calledWhenFull = false;
    RetrieveSummary fullSummary{4, 5};
    const Result fullResult = warehouse::RetrieveUpToCapacity(
        fullState, fullPack, 0,
        [&calledWhenFull](const ArtifactRecord&) -> bool {
            calledWhenFull = true;
            throw std::runtime_error("must not be called when full");
        }, fullSummary);
    Check(fullResult == Result::Ok && !calledWhenFull && fullSummary.moved == 0
          && fullSummary.rejected == 0 && SameSnapshot(fullSnapshot, fullState, fullPack),
          "full backpack never invokes predicate and remains unchanged");

    State throwingState;
    throwingState.heroes[0] = {{20, 1}, {21, 2}, {22, 3}};
    Backpack throwingPack = MakeBackpack(62, 700);
    const Snapshot throwingSnapshot = TakeSnapshot(throwingState, throwingPack);
    RetrieveSummary throwingSummary{7, 8};
    int callsBeforeThrow = 0;
    const Result throwingResult = warehouse::RetrieveUpToCapacity(
        throwingState, throwingPack, 0,
        [&callsBeforeThrow](const ArtifactRecord& record) {
            ++callsBeforeThrow;
            if (record.artifactId == 22) throw std::runtime_error("predicate failure");
            return record.artifactId == 20;
        }, throwingSummary);
    Check(throwingResult == Result::PredicateFailure && callsBeforeThrow == 3
          && throwingSummary.moved == 0 && throwingSummary.rejected == 0
          && SameSnapshot(throwingSnapshot, throwingState, throwingPack),
          "mid-predicate exception zeroes summary and rolls both sides back");

    RetrieveSummary badAllocPredicateSummary{2, 3};
    const Snapshot badAllocPredicateSnapshot = TakeSnapshot(throwingState, throwingPack);
    const Result badAllocPredicate = warehouse::RetrieveUpToCapacity(
        throwingState, throwingPack, 0,
        [](const ArtifactRecord&) -> bool { throw std::bad_alloc(); },
        badAllocPredicateSummary);
    Check(badAllocPredicate == Result::PredicateFailure
          && badAllocPredicateSummary.moved == 0 && badAllocPredicateSummary.rejected == 0
          && SameSnapshot(badAllocPredicateSnapshot, throwingState, throwingPack),
          "predicate bad_alloc is contained as failure with rollback");

    RetrieveSummary lengthPredicateSummary{2, 3};
    const Snapshot lengthPredicateSnapshot = TakeSnapshot(throwingState, throwingPack);
    const Result lengthPredicate = warehouse::RetrieveUpToCapacity(
        throwingState, throwingPack, 0,
        [](const ArtifactRecord&) -> bool { throw std::length_error("predicate failure"); },
        lengthPredicateSummary);
    Check(lengthPredicate == Result::PredicateFailure
          && lengthPredicateSummary.moved == 0 && lengthPredicateSummary.rejected == 0
          && SameSnapshot(lengthPredicateSnapshot, throwingState, throwingPack),
          "predicate length_error is contained with rollback");

    State allocationState;
    allocationState.heroes[0] = {{31, 1}, {32, 2}};
    Backpack allocationPack = MakeBackpack(1, 800);
    const Snapshot allocationSnapshot = TakeSnapshot(allocationState, allocationPack);
    RetrieveSummary allocationSummary{9, 9};
    WarehouseTestArmAllocationFailure_(0);
    const Result allocation = warehouse::RetrieveUpToCapacity(
        allocationState, allocationPack, 0,
        [](const ArtifactRecord&) noexcept { return true; }, allocationSummary);
    const std::size_t forced = WarehouseTestDisarmAllocationFailure_();
    Check(allocation == Result::AllocationFailure && forced != 0
          && allocationSummary.moved == 0 && allocationSummary.rejected == 0
          && SameSnapshot(allocationSnapshot, allocationState, allocationPack),
          "retrieve bad_alloc before predicate rolls back");

    State secondAllocationState;
    secondAllocationState.heroes[0] = {{41, 1}};
    Backpack secondAllocationPack = MakeBackpack(1, 900);
    const Snapshot secondAllocationSnapshot = TakeSnapshot(secondAllocationState, secondAllocationPack);
    RetrieveSummary secondAllocationSummary{9, 9};
    WarehouseTestArmAllocationFailure_(1);
    const Result secondAllocation = warehouse::RetrieveUpToCapacity(
        secondAllocationState, secondAllocationPack, 0,
        [](const ArtifactRecord&) noexcept { return true; }, secondAllocationSummary);
    const std::size_t secondForced = WarehouseTestDisarmAllocationFailure_();
    Check(secondAllocation == Result::AllocationFailure && secondForced != 0
          && secondAllocationSummary.moved == 0 && secondAllocationSummary.rejected == 0
          && SameSnapshot(secondAllocationSnapshot, secondAllocationState, secondAllocationPack),
          "retrieve bad_alloc during preparation rolls back");

    State invalidState;
    invalidState.heroes[0] = {{1, 1}};
    Backpack over = MakeBackpack(65);
    RetrieveSummary invalidSummary{1, 1};
    const Snapshot invalidSnapshot = TakeSnapshot(invalidState, over);
    bool invalidCalled = false;
    Check(warehouse::RetrieveUpToCapacity(
              invalidState, over, 0,
              [&invalidCalled](const ArtifactRecord&) {
                  invalidCalled = true;
                  return true;
              }, invalidSummary) == Result::InvalidBackpack
          && !invalidCalled && invalidSummary.moved == 0 && invalidSummary.rejected == 0
          && SameSnapshot(invalidSnapshot, invalidState, over),
          "65-slot retrieve input rejected before predicate");

    State aliasState;
    aliasState.heroes[0] = {{1, 1}};
    RetrieveSummary aliasSummary{1, 1};
    Check(warehouse::RetrieveUpToCapacity(aliasState, aliasState.heroes[0], 0,
                                          aliasSummary) == Result::AliasedBackpack
          && aliasSummary.moved == 0 && aliasSummary.rejected == 0,
          "retrieve rejects backpack alias with state.heroes");
}

void TestGuardMatrixAndConservation() {
    State state;
    for (std::size_t hero = 0; hero < warehouse::kHeroCount; ++hero)
        state.heroes[hero] = {{static_cast<std::int32_t>(hero), 123}};
    Backpack backpack = {{1, 7}, {1, 8}, {1, 7}, {42, 99},
                         {(std::numeric_limits<std::int32_t>::max)(), -123}};
    PendingSelection selected;
    Check(warehouse::SelectBackpackItem(state, backpack, 155, 7, 2, selected) == Result::Ok,
          "select one of duplicate scroll pairs");
    const Snapshot before = TakeSnapshot(state, backpack);
    const Backpack multisetBefore = Join(state.heroes[155], backpack);
    Check(warehouse::MoveOneToWarehouse(state, backpack, 155, 7, selected) == Result::Ok
          && SameMultiset(multisetBefore, Join(state.heroes[155], backpack)),
          "single duplicate move conserves complete pair multiset");
    Check(warehouse::MoveAllToWarehouse(state, backpack, 155) == Result::Ok
          && SameMultiset(multisetBefore, state.heroes[155]),
          "bulk append conserves scroll duplicate and arbitrary subtype multiset");
    RetrieveSummary summary;
    Check(warehouse::RetrieveUpToCapacity(state, backpack, 155, summary) == Result::Ok
          && SameMultiset(multisetBefore, backpack) && state.heroes[155].empty()
          && summary.moved == multisetBefore.size() && summary.rejected == 0,
          "default retrieve returns duplicate records unchanged");
    for (std::size_t hero = 0; hero < 155; ++hero)
        Check(state.heroes[hero] == before.state.heroes[hero],
              "transfers for hero 155 leave every other fixed hero untouched");

    for (std::int32_t invalidHero : {-1, 156}) {
        const Snapshot snap = TakeSnapshot(state, backpack);
        PendingSelection bad;
        bad.active = true;
        Check(warehouse::SelectBackpackItem(state, backpack, invalidHero, 7, 0, bad)
              == Result::InvalidHero && !bad.active,
              "invalid hero selection clears old pending selection");
        Check(warehouse::MoveOneToWarehouse(state, backpack, invalidHero, 7, bad)
              == Result::InvalidHero, "single move rejects invalid current hero");
        summary = {8, 9};
        Check(warehouse::RetrieveUpToCapacity(state, backpack, invalidHero, summary)
              == Result::InvalidHero && summary.moved == 0 && summary.rejected == 0,
              "invalid retrieve hero clears summary");
        Check(SameSnapshot(snap, state, backpack), "invalid hero operations never mutate");
    }

    PendingSelection pending;
    Check(warehouse::SelectBackpackItem(state, backpack, 0, 7, backpack.size(), pending)
          == Result::InvalidIndex && !pending.active, "one-past index is rejected");
    Check(warehouse::SelectBackpackItem(state, backpack, 0, 7,
          (std::numeric_limits<std::size_t>::max)(), pending) == Result::InvalidIndex,
          "max size_t index is rejected without addition");
    Check(warehouse::SelectBackpackItem(state, backpack, 0, 7, 0, pending) == Result::Ok,
          "selection for copied-state guard");
    State copiedState = state;
    const Snapshot copySnapshot = TakeSnapshot(copiedState, backpack);
    Check(warehouse::MoveOneToWarehouse(copiedState, backpack, 0, 7, pending)
          == Result::StaleSelection && SameSnapshot(copySnapshot, copiedState, backpack),
          "equal state clone cannot consume original state selection");

    for (std::size_t hero = 0; hero < warehouse::kHeroCount; ++hero) {
        const State aliasBefore = state;
        Check(warehouse::MoveAllToWarehouse(state, state.heroes[hero], 0)
              == Result::AliasedBackpack, "bulk move rejects any hero-vector alias");
        Check(warehouse::MoveOneToWarehouse(state, state.heroes[hero], 0, 7, pending)
              == Result::AliasedBackpack, "single move rejects any hero-vector alias");
        summary = {8, 9};
        Check(warehouse::RetrieveUpToCapacity(state, state.heroes[hero], 0, summary)
              == Result::AliasedBackpack && summary.moved == 0 && summary.rejected == 0,
              "retrieve rejects any hero-vector alias and zeroes summary");
        Check(SameState(aliasBefore, state), "all alias operations preserve entire state");
    }

    for (std::int32_t badId : {-1, -2, (std::numeric_limits<std::int32_t>::min)()}) {
        Backpack badPack = {{3, 9}, {badId, 9}};
        const Snapshot badSnapshot = TakeSnapshot(state, badPack);
        Check(warehouse::SelectBackpackItem(state, badPack, 0, 7, 0, pending)
              == Result::InvalidRecord, "selection checks all compact entries, not just selected");
        summary = {8, 9};
        Check(warehouse::RetrieveUpToCapacity(state, badPack, 0, summary)
              == Result::InvalidRecord && summary.moved == 0 && summary.rejected == 0,
              "negative compact input record rejects retrieve");
        Check(warehouse::MoveOneToWarehouse(state, badPack, 0, 7, pending)
              == Result::InvalidRecord && SameSnapshot(badSnapshot, state, badPack),
              "single move rejects invalid compact input unchanged");
    }

    State invalidWarehouse;
    invalidWarehouse.heroes[0] = {{-1, 3}};
    Backpack validPack = {{1, 2}};
    const Snapshot invalidSnapshot = TakeSnapshot(invalidWarehouse, validPack);
    Check(warehouse::SelectBackpackItem(invalidWarehouse, validPack, 0, 7, 0, pending)
          == Result::Ok, "selection observes backpack independently of invalid warehouse");
    Check(warehouse::MoveOneToWarehouse(invalidWarehouse, validPack, 0, 7, pending)
          == Result::InvalidRecord, "single move refuses invalid destination record");
    Check(warehouse::MoveAllToWarehouse(invalidWarehouse, validPack, 0)
          == Result::InvalidRecord, "bulk move refuses invalid destination record");
    summary = {8, 9};
    Check(warehouse::RetrieveUpToCapacity(invalidWarehouse, validPack, 0, summary)
          == Result::InvalidRecord && summary.moved == 0 && summary.rejected == 0
          && SameSnapshot(invalidSnapshot, invalidWarehouse, validPack),
          "invalid source record rejects retrieve unchanged");
}

void TestNoOpAndLifecycleGuards() {
    State state;
    Backpack backpack;
    RetrieveSummary summary{8, 9};
    bool called = false;
    Check(warehouse::RetrieveUpToCapacity(state, backpack, 0,
        [&called](const ArtifactRecord&) noexcept { called = true; return true; }, summary)
          == Result::Ok && !called && summary.moved == 0 && summary.rejected == 0
          && state.mutationEpoch == 0, "empty warehouse does not call predicate or advance epoch");
    state.heroes[0] = {{1, 7}, {1, 8}, {42, 55}};
    const Snapshot snap = TakeSnapshot(state, backpack);
    Check(warehouse::RetrieveUpToCapacity(state, backpack, 0,
          [](const ArtifactRecord&) noexcept { return false; }, summary) == Result::Ok
          && summary.moved == 0 && summary.rejected == 3 && SameSnapshot(snap, state, backpack),
          "reject-all is a nonmutating success");

    struct NoncopyablePredicate {
        NoncopyablePredicate() = default;
        NoncopyablePredicate(const NoncopyablePredicate&) = delete;
        bool operator()(const ArtifactRecord&) const noexcept { return false; }
    } predicate;
    Check(warehouse::RetrieveUpToCapacity(state, backpack, 0, predicate, summary) == Result::Ok
          && summary.rejected == 3 && SameSnapshot(snap, state, backpack),
          "predicate is never copied outside exception guard");

    state.mutationEpoch = (std::numeric_limits<std::uint64_t>::max)();
    const Snapshot maxSnapshot = TakeSnapshot(state, backpack);
    called = false;
    summary = {8, 9};
    Check(warehouse::RetrieveUpToCapacity(state, backpack, 0,
        [&called](const ArtifactRecord&) noexcept { called = true; return true; }, summary)
          == Result::EpochExhausted && !called && summary.moved == 0 && summary.rejected == 0
          && SameSnapshot(maxSnapshot, state, backpack), "exhausted epoch rejects retrieve before predicate");
    Inventory replacement;
    replacement[155] = {{7, 99}};
    const Inventory replacementBefore = replacement;
    Check(warehouse::ReplaceInventories(state, std::move(replacement)) == Result::EpochExhausted
          && SameInventory(replacementBefore, replacement) && SameSnapshot(maxSnapshot, state, backpack),
          "exhausted epoch replacement preserves both source and destination");
    Check(warehouse::ReplaceInventories(state, std::move(state.heroes)) == Result::AliasedInventory
          && SameSnapshot(maxSnapshot, state, backpack), "replacement rejects self move");

    NativeBackpack native{};
    native[3] = {1, 7};
    native[17] = {1, 7};
    native[63] = {42, (std::numeric_limits<std::int32_t>::max)()};
    const NativeBackpack nativeBefore = native;
    Check(warehouse::ImportNativeBackpack(native, 3, backpack) == Result::Ok
          && backpack == Backpack{{1, 7}, {1, 7}, {42, (std::numeric_limits<std::int32_t>::max)()}}
          && native == nativeBefore, "native holes filtered stably without modifying copied slot array");
    const Backpack imported = backpack;
    Check(warehouse::ImportNativeBackpack(native, 4, backpack) == Result::InvalidBackpack
          && backpack == imported && native == nativeBefore, "overstated occupancy leaves both adapter sides unchanged");
}

void TestLifecycleAndReplacement() {
    State state;
    state.heroes[0] = {{1, 2}};
    state.heroes[155] = {{155, 3}};
    const std::uint64_t oldEpoch = state.mutationEpoch;
    Check(warehouse::ClearForNewGame(state) == Result::Ok
          && state.heroes[0].empty() && state.heroes[155].empty()
          && state.mutationEpoch == oldEpoch + 1,
          "new game clears all 156 hero inventories and advances epoch");

    Inventory replacement;
    replacement[0] = {{7, (std::numeric_limits<std::int32_t>::min)()}};
    replacement[155] = {{8, (std::numeric_limits<std::int32_t>::max)()}};
    Check(warehouse::ReplaceInventories(state, std::move(replacement)) == Result::Ok
          && state.heroes[0] == Backpack{{7, (std::numeric_limits<std::int32_t>::min)()}}
          && state.heroes[155] == Backpack{{8, (std::numeric_limits<std::int32_t>::max)()}},
          "replacement preserves all pair fields");

    Inventory invalid;
    invalid[2] = {{-1, 9}};
    const State beforeInvalid = state;
    Check(warehouse::ReplaceInventories(state, std::move(invalid)) == Result::InvalidRecord
          && SameState(beforeInvalid, state),
          "replacement rejects invalid compact records unchanged");

    state.mutationEpoch = (std::numeric_limits<std::uint64_t>::max)();
    const State maxBefore = state;
    Check(warehouse::InvalidateSelections(state) == Result::EpochExhausted
          && SameState(maxBefore, state), "invalidation refuses epoch wrap");
    Check(warehouse::ClearForNewGame(state) == Result::EpochExhausted
          && SameState(maxBefore, state), "new game refuses epoch wrap");
}

} // namespace

extern "C" void WarehouseTestArmAllocationFailure_(std::size_t successfulAllocations) noexcept;
extern "C" std::size_t WarehouseTestDisarmAllocationFailure_() noexcept;

void RunWarehouseCoreTests_() {
    g_checks = 0;
    g_failures = 0;
    TestConstantsAndRecordRules();
    TestNativeImport();
    TestMoveAll();
    TestSelection();
    TestRetrieve();
    TestGuardMatrixAndConservation();
    TestNoOpAndLifecycleGuards();
    TestLifecycleAndReplacement();
    if (g_failures != 0) {
        std::fprintf(stderr, "warehouse core: %d/%d checks failed\n", g_failures, g_checks);
        std::exit(1);
    }
    std::printf("PASS warehouse core: %d checks\n", g_checks);
}
