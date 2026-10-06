#pragma once

// Windows Interlocked APIs are supplied by the including translation unit.
enum ScenarioContext_ : LONG {
    kScenarioUnknown_ = 0,
    kScenarioNewGame_,
    kScenarioLoad_,
    kScenarioSave_
};

static volatile LONG g_scenario_context_ = kScenarioUnknown_;

static ScenarioContext_ ClassifyScenarioContext_(bool loading, bool saving)
{
    if (loading) return kScenarioLoad_;
    if (saving) return kScenarioSave_;
    return kScenarioNewGame_;
}

static LONG ObserveScenarioContext_(ScenarioContext_ context)
{
    return InterlockedExchange(&g_scenario_context_, context);
}

static ScenarioContext_ ConsumeScenarioContext_()
{
    return static_cast<ScenarioContext_>(
        InterlockedExchange(&g_scenario_context_, kScenarioUnknown_));
}

static bool AllowsNewGameWindow_(ScenarioContext_ context)
{
    return context == kScenarioNewGame_;
}
