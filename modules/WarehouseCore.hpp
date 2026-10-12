#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

// Pure value model only: no game pointers, hooks, native calls or I/O.
namespace warehouse {

constexpr std::size_t kHeroCount = 156;
constexpr std::size_t kBackpackCapacity = 64;

struct ArtifactRecord {
    std::int32_t artifactId = -1;
    std::int32_t subtype = -1;

    bool operator==(const ArtifactRecord& other) const noexcept {
        return artifactId == other.artifactId && subtype == other.subtype;
    }
};

using Backpack = std::vector<ArtifactRecord>;
using Inventory = std::array<Backpack, kHeroCount>;
using NativeBackpack = std::array<ArtifactRecord, kBackpackCapacity>;

struct State {
    Inventory heroes;
    std::uint64_t mutationEpoch = 0;
};

// Ephemeral selection, never serialized. ownerId is an opaque observed owner
// token supplied by the adapter, not a player permission decision by this model.
// State and Backpack must outlive the selection. Object identity + complete
// snapshot prevents replay against another equal-valued model/backpack.
struct PendingSelection {
    std::int32_t heroId = -1;
    std::size_t backpackIndex = 0;
    ArtifactRecord record;
    std::uint64_t mutationEpoch = 0;
    std::int32_t ownerId = -1;
    const State* stateIdentity = nullptr;
    const Backpack* backpackIdentity = nullptr;
    NativeBackpack backpackSnapshot = {};
    std::size_t backpackSize = 0;
    bool active = false;
};

enum class Result {
    Ok,
    InvalidHero,
    InvalidBackpack,
    InvalidIndex,
    StaleSelection,
    CapacityReached,
    AllocationFailure,
    PredicateFailure,
    InvalidRecord,
    AliasedBackpack,
    SizeOverflow,
    EpochExhausted,
    AliasedInventory
};

struct RetrieveSummary {
    std::size_t moved = 0;
    std::size_t rejected = 0;
};

inline bool IsValidHeroId(std::int32_t heroId) noexcept {
    return heroId >= 0 && static_cast<std::size_t>(heroId) < kHeroCount;
}

// The pure model has no artifact table: all nonnegative int32 IDs are opaque.
// subtype is ALWAYS preserved, including non-scroll values and INT32_MIN/MAX.
inline bool IsValidRecord(const ArtifactRecord& record) noexcept {
    return record.artifactId >= 0;
}

inline bool HasValidRecords(const Backpack& records) noexcept {
    for (const auto& record : records)
        if (!IsValidRecord(record)) return false;
    return true;
}

namespace detail {
inline bool CanGrowRecordCount(std::size_t current, std::size_t added,
                               std::size_t maximum) noexcept {
    return current <= maximum && added <= maximum - current;
}

inline bool CanAdvanceEpoch(const State& state) noexcept {
    return state.mutationEpoch < (std::numeric_limits<std::uint64_t>::max)();
}

inline Result ValidateBackpack(const State& state, const Backpack& backpack) noexcept {
    // Reject ANY hero-vector alias, not just the requested hero. Do this even
    // for empty/full/no-op transactions, before reading or mutating contents.
    for (const auto& records : state.heroes)
        if (&records == &backpack) return Result::AliasedBackpack;
    if (backpack.size() > kBackpackCapacity) return Result::InvalidBackpack;
    if (!HasValidRecords(backpack)) return Result::InvalidRecord;
    return Result::Ok;
}
} // namespace detail

// Import an already copied 64-slot value array, never original game memory.
// Only artifactId == -1 denotes an empty native slot; other negative IDs are
// corrupt. Validate the declared occupied count before allocating. On failure
// output is untouched; on success retain occupied slot order and exact pairs.
inline Result ImportNativeBackpack(const NativeBackpack& slots,
                                    std::int32_t declaredCount,
                                    Backpack& output) noexcept {
    if (declaredCount < 0 || declaredCount > static_cast<std::int32_t>(kBackpackCapacity))
        return Result::InvalidBackpack;
    std::size_t occupied = 0;
    for (const auto& record : slots) {
        if (record.artifactId < -1) return Result::InvalidRecord;
        if (record.artifactId >= 0) ++occupied;
    }
    if (occupied != static_cast<std::size_t>(declaredCount)) return Result::InvalidBackpack;
    try {
        Backpack candidate;
        candidate.reserve(occupied);
        for (const auto& record : slots)
            if (record.artifactId >= 0) candidate.push_back(record);
        output.swap(candidate);
        return Result::Ok;
    } catch (const std::bad_alloc&) {
        return Result::AllocationFailure;
    } catch (const std::length_error&) {
        return Result::AllocationFailure;
    }
}

inline void CancelSelection(PendingSelection& selection) noexcept {
    selection = {};
}

// No legacy record-only overload: callers must explicitly bind a current
// hero AND owner and provide the actual source model. Failed selection clears
// any previous selection. Taking/cancelling a selection never mutates state.
inline Result SelectBackpackItem(const State& state, const Backpack& backpack,
                                 std::int32_t heroId, std::int32_t ownerId,
                                 std::size_t backpackIndex,
                                 PendingSelection& selection) noexcept {
    CancelSelection(selection);
    if (!IsValidHeroId(heroId)) return Result::InvalidHero;
    const Result valid = detail::ValidateBackpack(state, backpack);
    if (valid != Result::Ok) return valid;
    if (backpackIndex >= backpack.size()) return Result::InvalidIndex;
    selection.heroId = heroId;
    selection.ownerId = ownerId;
    selection.backpackIndex = backpackIndex;
    selection.record = backpack[backpackIndex];
    selection.mutationEpoch = state.mutationEpoch;
    selection.stateIdentity = &state;
    selection.backpackIdentity = &backpack;
    selection.backpackSize = backpack.size();
    for (std::size_t i = 0; i < backpack.size(); ++i)
        selection.backpackSnapshot[i] = backpack[i];
    selection.active = true;
    return Result::Ok;
}

inline Result MoveOneToWarehouse(State& state, Backpack& backpack,
                                 std::int32_t currentHeroId,
                                 std::int32_t currentOwnerId,
                                 const PendingSelection& selection) noexcept {
    if (!IsValidHeroId(currentHeroId)) return Result::InvalidHero;
    const Result valid = detail::ValidateBackpack(state, backpack);
    if (valid != Result::Ok) return valid;
    if (!selection.active || !IsValidHeroId(selection.heroId)
        || selection.heroId != currentHeroId || selection.ownerId != currentOwnerId
        || selection.stateIdentity != &state || selection.backpackIdentity != &backpack
        || selection.mutationEpoch != state.mutationEpoch
        || selection.backpackSize != backpack.size()) return Result::StaleSelection;
    if (selection.backpackIndex >= backpack.size()) return Result::InvalidIndex;
    if (!(backpack[selection.backpackIndex] == selection.record)) return Result::StaleSelection;
    for (std::size_t i = 0; i < backpack.size(); ++i)
        if (!(backpack[i] == selection.backpackSnapshot[i])) return Result::StaleSelection;

    auto& target = state.heroes[static_cast<std::size_t>(currentHeroId)];
    if (!HasValidRecords(target)) return Result::InvalidRecord;
    if (!detail::CanAdvanceEpoch(state)) return Result::EpochExhausted;
    if (!detail::CanGrowRecordCount(target.size(), 1, target.max_size()))
        return Result::SizeOverflow;
    try {
        // Prepare all allocating work privately. Commit below cannot throw for
        // this trivial record type and the default vector allocator.
        Backpack targetAfter;
        targetAfter.reserve(target.size() + 1);
        targetAfter.insert(targetAfter.end(), target.begin(), target.end());
        targetAfter.push_back(selection.record);
        target.swap(targetAfter);
        backpack.erase(backpack.begin() + static_cast<std::ptrdiff_t>(selection.backpackIndex));
        ++state.mutationEpoch;
        return Result::Ok;
    } catch (const std::bad_alloc&) {
        return Result::AllocationFailure;
    } catch (const std::length_error&) {
        return Result::AllocationFailure;
    }
}

inline Result MoveAllToWarehouse(State& state, Backpack& backpack,
                                 std::int32_t heroId) noexcept {
    if (!IsValidHeroId(heroId)) return Result::InvalidHero;
    const Result valid = detail::ValidateBackpack(state, backpack);
    if (valid != Result::Ok) return valid;
    auto& target = state.heroes[static_cast<std::size_t>(heroId)];
    if (!HasValidRecords(target)) return Result::InvalidRecord;
    if (backpack.empty()) return Result::Ok;
    if (!detail::CanAdvanceEpoch(state)) return Result::EpochExhausted;
    if (!detail::CanGrowRecordCount(target.size(), backpack.size(), target.max_size()))
        return Result::SizeOverflow;
    try {
        Backpack targetAfter;
        targetAfter.reserve(target.size() + backpack.size());
        targetAfter.insert(targetAfter.end(), target.begin(), target.end());
        targetAfter.insert(targetAfter.end(), backpack.begin(), backpack.end());
        target.swap(targetAfter);
        backpack.clear();
        ++state.mutationEpoch;
        return Result::Ok;
    } catch (const std::bad_alloc&) {
        return Result::AllocationFailure;
    } catch (const std::length_error&) {
        return Result::AllocationFailure;
    }
}

// canAccept MUST be pure, deterministic logic over a const record: never call
// native AddArtifact, mutate either model, re-enter Core, or perform external
// side effects. Core cannot undo callback side effects. This function is a
// model transaction, NOT a native-game commit adapter. Any exception from its
// invocation is PredicateFailure, including bad_alloc/length_error.
// Calls stop immediately when full; rejected counts only evaluated false items.
// Every failure zeroes summary and preserves both model vectors and epoch.
// All access to State/Backpack must be serialized by the caller.
template<class CanAccept>
Result RetrieveUpToCapacity(State& state, Backpack& backpack, std::int32_t heroId,
                            CanAccept&& canAccept, RetrieveSummary& summary) noexcept {
    summary = {};
    if (!IsValidHeroId(heroId)) return Result::InvalidHero;
    const Result valid = detail::ValidateBackpack(state, backpack);
    if (valid != Result::Ok) return valid;
    auto& source = state.heroes[static_cast<std::size_t>(heroId)];
    if (!HasValidRecords(source)) return Result::InvalidRecord;
    if (source.empty() || backpack.size() == kBackpackCapacity) return Result::Ok;
    if (!detail::CanAdvanceEpoch(state)) return Result::EpochExhausted;

    RetrieveSummary preparedSummary;
    try {
        Backpack backpackAfter;
        Backpack warehouseAfter;
        backpackAfter.reserve(kBackpackCapacity);
        backpackAfter.insert(backpackAfter.end(), backpack.begin(), backpack.end());
        warehouseAfter.reserve(source.size());
        for (const auto& record : source) {
            if (backpackAfter.size() < kBackpackCapacity) {
                bool accepted = false;
                try {
                    accepted = canAccept(record);
                } catch (...) {
                    return Result::PredicateFailure;
                }
                if (accepted) {
                    backpackAfter.push_back(record);
                    ++preparedSummary.moved;
                    continue;
                }
                ++preparedSummary.rejected;
            }
            warehouseAfter.push_back(record);
        }
        if (preparedSummary.moved != 0) {
            backpack.swap(backpackAfter);
            source.swap(warehouseAfter);
            ++state.mutationEpoch;
        }
        summary = preparedSummary;
        return Result::Ok;
    } catch (const std::bad_alloc&) {
        summary = {};
        return Result::AllocationFailure;
    } catch (const std::length_error&) {
        summary = {};
        return Result::AllocationFailure;
    }
}

inline Result RetrieveUpToCapacity(State& state, Backpack& backpack,
                                   std::int32_t heroId,
                                   RetrieveSummary& summary) noexcept {
    return RetrieveUpToCapacity(state, backpack, heroId,
                                [](const ArtifactRecord&) noexcept { return true; }, summary);
}

// The integration MUST advance the epoch for external logical changes (native
// backpack edits, popup close/reopen, owner/lifecycle changes, etc.). Equal-value
// ABA mutations cannot be inferred from a value snapshot. Never reset/wrap an
// exhausted epoch on a live State; discard old selections and create a new State.
inline Result InvalidateSelections(State& state) noexcept {
    if (!detail::CanAdvanceEpoch(state)) return Result::EpochExhausted;
    ++state.mutationEpoch;
    return Result::Ok;
}

inline Result ClearForNewGame(State& state) noexcept {
    if (!detail::CanAdvanceEpoch(state)) return Result::EpochExhausted;
    for (auto& records : state.heroes) records.clear();
    ++state.mutationEpoch;
    return Result::Ok;
}

inline Result ReplaceInventories(State& state, Inventory&& inventories) noexcept {
    if (&inventories == &state.heroes) return Result::AliasedInventory;
    for (const auto& records : inventories)
        if (!HasValidRecords(records)) return Result::InvalidRecord;
    if (!detail::CanAdvanceEpoch(state)) return Result::EpochExhausted;
    state.heroes = std::move(inventories);
    ++state.mutationEpoch;
    return Result::Ok;
}

} // namespace warehouse
