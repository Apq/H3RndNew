#pragma once
#include <stdint.h>

namespace bankmax {
constexpr int kRatioScale = 1000;
struct ControlsLayout {
    static constexpr int titleWidth = 90, labelWidth = 60, sliderWidth = 100, percentWidth = 47;
    int checkX, sliderX, percentX;
    ControlsLayout(int titleX) : checkX(titleX + titleWidth),
        sliderX(checkX + 32 + 3 + labelWidth + 3), percentX(sliderX + sliderWidth + 3) {}
    bool Fits(int dialogWidth, int textHeight) const {
        return percentX + percentWidth <= dialogWidth - 6 && textHeight <= 24;
    }
};
inline int ClampRatio(int ratio) { return ratio < 0 ? 0 : ratio > kRatioScale ? kRatioScale : ratio; }
inline int RatioFromPixel(int pixel, int width) {
    if (width <= 1) return 0;
    if (pixel <= 0) return 0;
    if (pixel >= width - 1) return 1000;
    return (pixel * 1000 + (width - 1) / 2) / (width - 1);
}
enum class SliderEvent { Down, Move, Release, Click, Other };
enum class SliderAction { Pass, Update, Consume };
struct SliderGesture {
    bool dragging = false;
    bool swallowClick = false;
    void Reset() { dragging = false; swallowClick = false; }
    SliderAction Handle(SliderEvent event, bool inside, bool leftButtonDown = true) {
        if (event == SliderEvent::Down) {
            Reset();
            if (!inside) return SliderAction::Pass;
            dragging = swallowClick = true;
            return SliderAction::Update;
        }
        if (event == SliderEvent::Move && dragging) {
            // HD may omit the separate release/click message after a consumed
            // press. Never update a hover using stale drag state.
            if (!leftButtonDown) { dragging = false; return SliderAction::Consume; }
            return SliderAction::Update;
        }
        if (event == SliderEvent::Release && dragging) {
            dragging = false;
            return SliderAction::Consume;
        }
        if (event == SliderEvent::Click && swallowClick) {
            Reset();
            return SliderAction::Consume;
        }
        return SliderAction::Pass;
    }
};
inline bool SuccessfulNewGame(bool newGame, unsigned result) { return newGame && (result & 255u) == 1u; }
inline int ResourceLimit(int type) {
    return type < 0 || type > 6 ? 0 : (type == 0 || type == 2 || type == 6 ? 10 : 6);
}
inline uint32_t ChestSetup(uint32_t old) {
    // Clear artifact id, artifact flag and bonus, retain unrelated upper bits.
    return (old & ~0x7FFFu) | (4u << 11);
}
inline uint32_t ResourceSetup(uint32_t old, int type) {
    unsigned limit = static_cast<unsigned>(ResourceLimit(type));
    unsigned value = old & 0x7FFFFu;
    // Explicit map-author/RMG amounts above the ordinary upper bound remain intact.
    return limit && value < limit ? (old & ~0x7FFFFu) | limit : old;
}
inline uint32_t CampfireSetup(uint32_t old) {
    return (old & 15u) | ((old >> 4) < 6u ? (6u << 4) : (old & 0xFFFFFFF0u));
}
template<class Bank, class State>
inline void CopyRewardFields(Bank& bank, const State& state) {
    bank.resources = state.resources;
    bank.creatureRewardType = state.creatureRewardType;
    bank.creatureRewardCount = state.creatureRewardCount;
    // Never assign guardians or the entire bank/state.
}
// Strict class selection: disabled and map-banned artifacts never enter the pool.
// A depleted class restarts from map allowances; no fallback to a lower class.
template<class Artifact, class Roll>
int PickArtifact(const Artifact* info, const signed char* banned,
                 signed char* used, int mask, Roll roll) {
    int candidates[144];
    int count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        count = 0;
        for (int id = 0; id < 144; ++id) {
            if (!info[id].disabled && (static_cast<int>(info[id].type) & mask)
                && !banned[id] && !used[id]) candidates[count++] = id;
        }
        if (count) break;
        for (int id = 0; id < 144; ++id)
            if (!info[id].disabled && (static_cast<int>(info[id].type) & mask))
                used[id] = banned[id];
    }
    if (!count) return -1;
    int index = roll(count);
    if (index < 0 || index >= count) return -1;
    int id = candidates[index];
    used[id] = 1;
    return id;
}
}
