#pragma once

#include "EmbeddedChucKEngine.h"

#include <vector>

namespace ChucKScoreScript
{
enum class CommandId
{
    none = 0,
    scoreClear = 1,
    scoreComplete = 998,
    play = 2,
    stop = 3,
    tempo = 4,
    meter = 5,
    phase = 6,
    stateAdd = 10,
    stateRemove = 11,
    stateDuration = 12,
    stateTail = 13,
    stateName = 14,
    stateSelect = 15,
    stateConnect = 16,
    stateDisconnect = 17,
    stateClearConnections = 18,
    trackAdd = 20,
    trackRemove = 21,
    trackName = 22,
    trackLanguage = 23,
    trackGain = 24,
    trackSync = 25,
    trackTempo = 26,
    trackMeter = 27,
    trackPhase = 28,
    trackCode = 29,
    trackTemplate = 30,
    trackClear = 31,
    trackMute = 32,
    trackSolo = 33,
    trackType = 34,
    mixer = 40
};

struct Program
{
    juce::String source;
};

constexpr int argumentCount = 6;

std::vector<EmbeddedChucKEngine::ParameterBinding> getParameterBindings();
juce::String buildPrelude();
Program buildProgram (const juce::String& script);
} // namespace ChucKScoreScript
