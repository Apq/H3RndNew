#include "../modules/WarehouseStore.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

extern "C" void WarehouseTestArmAllocationFailure_(std::size_t) noexcept;
extern "C" std::size_t WarehouseTestDisarmAllocationFailure_() noexcept;

namespace {

using namespace warehouse;
using namespace warehouse::store;
using Bytes = std::vector<std::uint8_t>;

int checks = 0;

void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL warehouse store: %s\n", label);
        std::exit(1);
    }
}

Bytes Text(const char* s) { return Bytes(s, s + std::strlen(s)); }
const Bytes prefix = Text("native-store-fixture-prefix");

Inventory Stocks(char tag) {
    Inventory stocks;
    stocks[0] = {{1, 7}, {1, 8}, {42, -1}};
    stocks[9] = {{static_cast<std::int32_t>(tag), 4}};
    stocks[155] = {{143, -1}};
    return stocks;
}

Observation GoodObservation(std::size_t nativeEnd) {
    Observation observation;
    observation.operationToken = 0x1D2C3B4A5E6F7081ull;
    observation.pathIdentity = 0xABCD1234EF560907ull;
    observation.pathIdentityKnown = true;
    observation.nativeSucceeded = true;
    observation.streamClosedSuccessfully = true;
    observation.nativeEnd = nativeEnd;
    return observation;
}

void GateTests() {
    const auto file = codec::Encode(prefix, prefix.size(), Stocks('a'));
    State state;
    state.heroes[3] = {{5, 6}};
    const auto sentinel = state.heroes;
    const auto frozen = file;

    // token zero
    {
        auto obs = GoodObservation(prefix.size());
        obs.operationToken = 0;
        SavePlan plan;
        // direct call path
        Inventory inv = Stocks('b');
        std::vector<std::uint8_t> bytes(frozen.begin(), frozen.end());
        // PlanSave
        plan = PlanSave(bytes, obs, inv);
        Check(plan.status == Status::InvalidToken && plan.newFile.empty(),
              "save rejects zero token");
        LoadPlan load = ApplyLoad(bytes, obs, state, {});
        Check(load.status == Status::InvalidToken && state.heroes == sentinel,
              "load rejects zero token");
    }
    // path identity missing
    {
        auto obs = GoodObservation(prefix.size());
        obs.pathIdentityKnown = false;
        std::vector<std::uint8_t> bytes(frozen.begin(), frozen.end());
        auto plan = PlanSave(bytes, obs, Stocks('b'));
        Check(plan.status == Status::PathIdentityMissing && plan.newFile.empty(),
              "save rejects missing path identity");
        auto load = ApplyLoad(bytes, obs, state, {});
        Check(load.status == Status::PathIdentityMissing && state.heroes == sentinel,
              "load rejects missing path identity");
    }
    // path identity zero even when claimed known
    {
        auto obs = GoodObservation(prefix.size());
        obs.pathIdentity = 0;
        std::vector<std::uint8_t> bytes(frozen.begin(), frozen.end());
        auto plan = PlanSave(bytes, obs, Stocks('b'));
        Check(plan.status == Status::PathIdentityMissing, "zero path identity rejected");
    }
    // native operation failed
    {
        auto obs = GoodObservation(prefix.size());
        obs.nativeSucceeded = false;
        std::vector<std::uint8_t> bytes(frozen.begin(), frozen.end());
        auto plan = PlanSave(bytes, obs, Stocks('b'));
        Check(plan.status == Status::NativeOperationFailed && plan.newFile.empty(),
              "save rejects failed native save");
        auto load = ApplyLoad(bytes, obs, state, {});
        Check(load.status == Status::NativeOperationFailed && state.heroes == sentinel,
              "load keeps old inventory when native load failed");
    }
    // stream close failed
    {
        auto obs = GoodObservation(prefix.size());
        obs.streamClosedSuccessfully = false;
        std::vector<std::uint8_t> bytes(frozen.begin(), frozen.end());
        auto plan = PlanSave(bytes, obs, Stocks('b'));
        Check(plan.status == Status::StreamCloseFailed && plan.newFile.empty(),
              "save rejects unverified close");
        auto load = ApplyLoad(bytes, obs, state, {});
        Check(load.status == Status::StreamCloseFailed, "load rejects unverified close");
    }
    // nativeEnd absent / zero / beyond file
    {
        std::vector<std::uint8_t> bytes(frozen.begin(), frozen.end());
        auto obs = GoodObservation(prefix.size());
        obs.nativeEnd.reset();
        auto plan = PlanSave(bytes, obs, Stocks('b'));
        Check(plan.status == Status::BoundaryUntrusted && plan.newFile.empty(),
              "save rejects absent nativeEnd");
        auto load = ApplyLoad(bytes, obs, state, {});
        Check(load.status == Status::BoundaryUntrusted && state.heroes == sentinel,
              "load rejects absent nativeEnd");

        obs = GoodObservation(0);
        plan = PlanSave(bytes, obs, Stocks('b'));
        Check(plan.status == Status::BoundaryUntrusted, "zero nativeEnd rejected");

        obs = GoodObservation(bytes.size() + 1);
        plan = PlanSave(bytes, obs, Stocks('b'));
        Check(plan.status == Status::BoundaryUntrusted, "nativeEnd beyond file rejected");
    }
    Check(file == frozen, "gate checks never modify input file");
}

void SaveTests() {
    const auto oldStocks = Stocks('a');
    auto newStocks = Stocks('b');
    auto file = codec::Encode(prefix, prefix.size(), oldStocks);
    const auto frozen = file;

    // Replace an own EOF block (new snapshot has one extra record).
    newStocks[9].push_back({21, 5});
    auto plan = PlanSave(file, GoodObservation(prefix.size()), newStocks);
    Check(plan.status == Status::Ok
          && plan.newFile.size() == file.size() + 8,
          "save plans EOF replacement");
    Check(file == frozen, "PlanSave never mutates input");
    Inventory decoded;
    Check(codec::Decode(plan.newFile, prefix.size(), decoded).status
              == codec::DecodeStatus::Ok
          && decoded == newStocks,
          "planned file decodes to new inventory");
    Check(std::equal(prefix.begin(), prefix.end(), plan.newFile.begin()),
          "planned file preserves prefix");

    // Append first block to a plain prefix.
    plan = PlanSave(prefix, GoodObservation(prefix.size()), newStocks);
    Check(plan.status == Status::Ok
          && codec::Decode(plan.newFile, prefix.size(), decoded).status
                 == codec::DecodeStatus::Ok
          && decoded == newStocks,
          "save plans first append on legacy file");

    // Foreign suffix after own block refuses rewrite.
    auto suffixed = codec::Encode(prefix, prefix.size(), oldStocks);
    const auto foreign = Text("OTHER_PLUGIN_TAIL");
    suffixed.insert(suffixed.end(), foreign.begin(), foreign.end());
    plan = PlanSave(suffixed, GoodObservation(prefix.size()), newStocks);
    Check(plan.status == Status::AttachmentUnsafe
          && plan.codecError == codec::Error::ForeignSuffix
          && plan.newFile.empty(),
          "foreign suffix is never moved or truncated");

    // Allocation failure leaves no partial plan.
    WarehouseTestArmAllocationFailure_(0);
    plan = PlanSave(file, GoodObservation(prefix.size()), newStocks);
    const auto failures = WarehouseTestDisarmAllocationFailure_();
    Check(plan.status == Status::AllocationFailure && plan.newFile.empty()
          && failures != 0 && file == frozen,
          "save allocation failure is atomic");
}

void LoadTests() {
    const auto stocks = Stocks('a');
    const auto file = codec::Encode(prefix, prefix.size(), stocks);

    // Valid attachment replaces the whole inventory.
    State state;
    state.heroes[3] = {{5, 6}};
    state.mutationEpoch = 41;
    auto plan = ApplyLoad(file, GoodObservation(prefix.size()), state, {});
    Check(plan.status == Status::Ok && !plan.lockAdvised
          && state.heroes == stocks && state.mutationEpoch == 42,
          "valid load replaces inventory and advances epoch");

    // No attachment never clears state.
    const auto sentinel = state.heroes;
    plan = ApplyLoad(prefix, GoodObservation(prefix.size()), state, {});
    Check(plan.status == Status::NoAttachmentLegacyUnknown
          && !plan.lockAdvised && state.heroes == sentinel,
          "ambiguous legacy save never clears inventory");

    // Valid block followed by an opaque third-party suffix still loads.
    auto suffixedLoad = codec::Encode(prefix, prefix.size(), stocks);
    const auto tail = Text("TAIL_PLUGIN_BYTES_9Z");
    suffixedLoad.insert(suffixedLoad.end(), tail.begin(), tail.end());
    plan = ApplyLoad(suffixedLoad, GoodObservation(prefix.size()), state, {});
    Check(plan.status == Status::Ok && state.heroes == stocks,
          "opaque third-party suffix does not block reading our block");

    // Damaged attachment locks and keeps state.
    auto damaged = file;
    damaged[damaged.size() - 2] ^= 0x20;
    plan = ApplyLoad(damaged, GoodObservation(prefix.size()), state, {});
    Check(plan.status == Status::AttachmentInvalid
          && plan.codecError == codec::Error::BadFooterCrc
          && plan.lockAdvised && state.heroes == sentinel,
          "damaged attachment locks writes and keeps inventory");

    // Codec-legal bits still fail Core semantics through ReplaceInventories.
    Inventory hostile;
    hostile[2] = {{-7, 0}};
    const auto hostileFile = codec::Encode(prefix, prefix.size(), hostile);
    plan = ApplyLoad(hostileFile, GoodObservation(prefix.size()), state, {});
    Check(plan.status == Status::InventoryReplaceFailed
          && plan.replaceResult == Result::InvalidRecord
          && plan.lockAdvised && state.heroes == sentinel,
          "negative artifact ids never reach state");

    // Epoch exhaustion surfaces without touching state.
    State exhausted;
    exhausted.heroes[0] = {{9, 1}};
    exhausted.mutationEpoch = UINT64_MAX;
    plan = ApplyLoad(file, GoodObservation(prefix.size()), exhausted, {});
    Check(plan.status == Status::InventoryReplaceFailed
          && plan.replaceResult == Result::EpochExhausted
          && exhausted.heroes[0].size() == 1
          && exhausted.mutationEpoch == UINT64_MAX,
          "epoch exhaustion keeps previous inventory");

    // Decode allocation failure keeps state.
    Inventory big;
    big[0].assign(4096, {17, 3});
    const auto bigFile = codec::Encode(prefix, prefix.size(), big);
    State target;
    target.heroes[1] = {{3, 4}};
    const auto before = target.heroes;
    WarehouseTestArmAllocationFailure_(0);
    plan = ApplyLoad(bigFile, GoodObservation(prefix.size()), target, {});
    const auto decodeFailures = WarehouseTestDisarmAllocationFailure_();
    Check((plan.status == Status::AllocationFailure
           || plan.status == Status::AttachmentInvalid)
          && plan.lockAdvised && decodeFailures != 0 && target.heroes == before,
          "decode allocation failure keeps inventory");
}

} // namespace

void RunWarehouseStoreTests_() {
    GateTests();
    SaveTests();
    LoadTests();
    std::printf("PASS warehouse store: %d checks\n", checks);
}
