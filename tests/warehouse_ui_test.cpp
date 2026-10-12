// Standalone UI lifecycle gate tests: no Heroes3 process, no patcher, no game UI.
// These tests deliberately exercise only version-state transitions and the
// session state machine; attaching real backpack controls is a separate phase
// and must pass the same state-machine gates first.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <new>
#include <vector>

#include "../modules/WarehouseUi.hpp"

static int g_checks = 0;
static void Check(bool ok, const char* message)
{
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static std::size_t CountFingerprints_()
{
    std::size_t count = 0;
    for (const auto& entry : warehouseui::kHsodFingerprints_) {
        Check(entry.label && *entry.label, "fingerprint has label");
        Check(entry.bytes && entry.size > 0, "fingerprint has bytes");
        Check(entry.imageRva < 0x200000u, "fingerprint is RVA not preferred VA");
        ++count;
    }
    return count;
}

static void CheckFingerprintBytes_()
{
    std::vector<std::uint8_t> image(0x400000u, 0);
    for (const auto& entry : warehouseui::kHsodFingerprints_) {
        Check(entry.imageRva + entry.size <= image.size(), "synthetic fingerprint fits image");
        std::memcpy(image.data() + entry.imageRva, entry.bytes, entry.size);
    }
    Check(warehouseui::MatchesAllFingerprints(image.data(), image.size()),
        "all synthetic HD_SOD fingerprints match");
    image[warehouseui::kHsodFingerprints_[1].imageRva] ^= 0x01;
    Check(!warehouseui::MatchesAllFingerprints(image.data(), image.size()),
        "one changed HD_SOD byte rejects the gate");
    image[warehouseui::kHsodFingerprints_[1].imageRva] ^= 0x01;
    Check(warehouseui::MatchesAllFingerprints(image.data(), image.size()),
        "restored HD_SOD bytes pass the gate");
    const std::uint8_t shortImage[1] = {};
    Check(!warehouseui::MatchesFingerprint(shortImage, sizeof(shortImage),
        {"out-of-range", static_cast<std::uint32_t>(image.size() - 1), warehouseui::kHsodCtorSig_, 2}),
        "out-of-range fingerprint rejects safely");
}

static void CheckStateMachine_()
{
    using namespace warehouseui;
    UiSessionStore_ store;
    store.Reset();
    Check(store.token() == 0 && store.state() == UiState::Inactive_, "idle state");

    const std::uint64_t first = store.BeginAttach();
    Check(first == 1 && store.state() == UiState::Attaching_, "first attach token");
    Check(store.Activate(first), "activate first session");
    Check(store.state() == UiState::Active_, "active state");

    Check(store.SetPendingSelection(first), "record pending selection");
    Check(store.pendingSelection(), "pending flag set");
    Check(store.ConsumePendingSelection(first), "consume pending selection");
    Check(!store.pendingSelection(), "pending flag cleared");

    // A second open without a close invalidates the earlier token; the old
    // token must never appear valid again even if its pointer reused.
    const std::uint64_t second = store.BeginAttach();
    Check(second == 2, "second attach token increments");
    Check(!store.IsValidFor(first), "previous token invalidated by reopen");
    Check(store.Activate(second), "activate second session");
    Check(store.state() == UiState::Active_, "second session active");

    Check(!store.BeginClose(first, CloseReason::Esc_), "stale close ignored");
    Check(store.state() == UiState::Active_, "stale close does not change state");
    Check(store.BeginClose(second, CloseReason::Esc_), "close active session");
    Check(store.state() == UiState::Closing_, "closing state");
    Check(store.closeReason() == CloseReason::Esc_, "close reason preserved");
    Check(store.FinishClose(second), "finish close");
    Check(store.state() == UiState::Inactive_, "return to inactive after close");

    const std::uint64_t third = store.BeginAttach();
    Check(!store.Activate(second), "old token cannot activate new session");
    Check(store.Activate(third), "new session activates");
    store.Invalidate(CloseReason::VersionInvalid_);
    Check(store.state() == UiState::Invalidated_, "invalidate on version failure");
    Check(store.closeReason() == CloseReason::VersionInvalid_, "invalidate reason preserved");
    Check(!store.IsValidFor(third), "invalidated token cannot update");
    Check(!store.SetPendingSelection(third), "invalidated session rejects pending selection");
    Check(!store.ConsumePendingSelection(third), "invalidated session rejects consume");
    Check(!store.FinishClose(third), "invalidated session cannot finish close");
    store.Reset();
    Check(store.state() == UiState::Inactive_ && store.token() == 0, "reset clears token");
}

int RunWarehouseUiTests_()
{
    const std::size_t fingerprints = CountFingerprints_();
    Check(fingerprints == warehouseui::kHsodFingerprintCount_,
        "all supported HD_SOD fingerprints registered");
    CheckStateMachine_();
    CheckFingerprintBytes_();
    std::printf("PASS warehouse UI state machine and %zu fingerprint gates (%d checks)\n",
        fingerprints, g_checks);
    return 0;
}
