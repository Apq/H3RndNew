#pragma once

#include "WarehouseCodec.hpp"
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Pure transaction policy between the codec and a future game/file adapter.
// This header performs no file I/O, hooks, path construction, or game-memory access.
//
// Boundary ownership (deliberate, per design doc 6.2/6.4):
// - Token replay / double post-hook notification: this layer validates the
//   token shape only; one-shot consumption and per-operation uniqueness are
//   enforced by the adapter context that issues tokens.
// - TOCTOU: if the file is swapped for a different internally consistent file
//   after the native read, the attachment's own prefix hash stays valid and
//   no codec check can detect it. Binding bytes to the observed native file
//   object is the adapter's responsibility; this layer only adjudicates the
//   evidence it receives.
// - NoAttachment is ambiguous (legacy save vs lost attachment): this layer
//   never clears or replaces State for that outcome.
namespace warehouse::store {

enum class Status {
    Ok,
    InvalidToken,
    PathIdentityMissing,
    NativeOperationFailed,
    StreamCloseFailed,
    BoundaryUntrusted,
    AttachmentUnsafe,
    NoAttachmentLegacyUnknown,
    AttachmentInvalid,
    InventoryReplaceFailed,
    AllocationFailure
};

struct Observation {
    // Opaque unique token assigned by the adapter to this exact operation.
    std::uint64_t operationToken = 0;
    // Identity captured from the stream actually opened by the native call.
    // Never derive this from the caller's save-name argument.
    std::uint64_t pathIdentity = 0;
    bool pathIdentityKnown = false;
    bool nativeSucceeded = false;
    bool streamClosedSuccessfully = false;
    // Must be supplied by an independent, verified native-stream boundary reader.
    // Never trust nativeEnd copied from an attachment header/footer.
    std::optional<std::size_t> nativeEnd;
};

struct SavePlan {
    Status status = Status::Ok;
    codec::Error codecError = codec::Error::None;
    std::vector<std::uint8_t> newFile;
};

struct LoadPlan {
    Status status = Status::Ok;
    codec::Error codecError = codec::Error::None;
    Result replaceResult = Result::Ok;
    // Caller should disable warehouse writes after an invalid/unsafe load until
    // a successful load or new-game lifecycle event explicitly resets policy.
    bool lockAdvised = false;
};

inline Status ValidateObservation(const Observation& observation,
                                  std::size_t fileBytes) noexcept {
    if (observation.operationToken == 0) return Status::InvalidToken;
    if (!observation.pathIdentityKnown || observation.pathIdentity == 0)
        return Status::PathIdentityMissing;
    if (!observation.nativeSucceeded) return Status::NativeOperationFailed;
    if (!observation.streamClosedSuccessfully) return Status::StreamCloseFailed;
    if (!observation.nativeEnd || *observation.nativeEnd == 0
        || *observation.nativeEnd > fileBytes)
        return Status::BoundaryUntrusted;
    return Status::Ok;
}

// Prepare a complete replacement byte vector. The input file and inventory are
// const and remain unchanged on every path. The adapter must atomically or
// otherwise safely commit newFile to the same captured path identity.
inline SavePlan PlanSave(std::span<const std::uint8_t> file,
                         const Observation& observation,
                         const Inventory& inventories,
                         codec::Limits limits = {}) noexcept {
    SavePlan plan;
    plan.status = ValidateObservation(observation, file.size());
    if (plan.status != Status::Ok) return plan;

    try {
        std::vector<std::uint8_t> candidate(file.begin(), file.end());
        if (!codec::StoreAttachment(candidate, *observation.nativeEnd,
                                    inventories, plan.codecError, limits)) {
            plan.status = plan.codecError == codec::Error::AllocationFailure
                ? Status::AllocationFailure : Status::AttachmentUnsafe;
            return plan;
        }
        plan.newFile.swap(candidate);
        return plan;
    } catch (const std::bad_alloc&) {
        plan.status = Status::AllocationFailure;
    } catch (const std::length_error&) {
        plan.status = Status::AllocationFailure;
    } catch (...) {
        plan.status = Status::AttachmentUnsafe;
    }
    plan.newFile.clear();
    return plan;
}

// A missing attachment has two possible meanings (legacy save or lost/cut
// attachment). This policy never clears/replaces State for that ambiguous case.
// The adapter/application must make an explicit, user-visible legacy decision.
inline LoadPlan ApplyLoad(std::span<const std::uint8_t> file,
                          const Observation& observation,
                          State& state,
                          codec::Limits limits = {}) noexcept {
    LoadPlan plan;
    plan.status = ValidateObservation(observation, file.size());
    if (plan.status != Status::Ok) return plan;

    Inventory decoded;
    const auto result = codec::Decode(file, *observation.nativeEnd, decoded, limits);
    if (result.status == codec::DecodeStatus::NoAttachment) {
        plan.status = Status::NoAttachmentLegacyUnknown;
        return plan;
    }
    if (result.status == codec::DecodeStatus::Invalid) {
        plan.status = result.error == codec::Error::AllocationFailure
            ? Status::AllocationFailure : Status::AttachmentInvalid;
        plan.codecError = result.error;
        plan.lockAdvised = true;
        return plan;
    }

    plan.replaceResult = ReplaceInventories(state, std::move(decoded));
    if (plan.replaceResult != Result::Ok) {
        plan.status = Status::InventoryReplaceFailed;
        plan.lockAdvised = true;
        return plan;
    }
    plan.status = Status::Ok;
    return plan;
}

} // namespace warehouse::store
