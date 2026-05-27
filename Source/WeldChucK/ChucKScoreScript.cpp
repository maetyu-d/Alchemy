#include "ChucKScoreScript.h"

namespace ChucKScoreScript
{
std::vector<EmbeddedChucKEngine::ParameterBinding> getParameterBindings()
{
    std::vector<EmbeddedChucKEngine::ParameterBinding> bindings;
    bindings.reserve (1 + argumentCount);
    bindings.push_back ({ "weldScoreCommand", 0.0f, 0.0f, 999.0f });

    for (int i = 0; i < argumentCount; ++i)
        bindings.push_back ({ "weldScoreArg" + juce::String (i), 0.0f, -1000000.0f, 1000000.0f });

    return bindings;
}

juce::String buildPrelude()
{
    return R"chuck(
global string weldScoreText0;
global string weldScoreText1;

120.0 => float weldLocalTempo;
4.0 => float weldMeterNumerator;
4.0 => float weldMeterDenominator;
dur beat;
dur bar;

fun void weldUpdateTime()
{
    (60.0 / Math.max(1.0, weldLocalTempo))::second => beat;
    beat * (weldMeterNumerator * (4.0 / Math.max(1.0, weldMeterDenominator))) => bar;
}

weldUpdateTime();

fun void weldSend(float command, float a0, float a1, float a2, float a3, float a4, float a5)
{
    a0 => weldScoreArg0;
    a1 => weldScoreArg1;
    a2 => weldScoreArg2;
    a3 => weldScoreArg3;
    a4 => weldScoreArg4;
    a5 => weldScoreArg5;
    command => weldScoreCommand;

    while (weldScoreCommand != 0.0)
        1::samp => now;
}

class ScoreApi
{
    fun void clear() { weldSend(1, 0, 0, 0, 0, 0, 0); }
}

class StateApi
{
    fun void add(string name, float durationBeats, float tailBeats)
    {
        name => weldScoreText0;
        weldSend(10, 0, durationBeats, tailBeats, 0, 0, 0);
    }

    fun void remove(float stateIndex) { weldSend(11, stateIndex, 0, 0, 0, 0, 0); }
    fun void duration(float stateIndex, float durationBeats) { weldSend(12, stateIndex, durationBeats, 0, 0, 0, 0); }
    fun void tail(float stateIndex, float tailBeats) { weldSend(13, stateIndex, tailBeats, 0, 0, 0, 0); }

    fun void name(float stateIndex, string name)
    {
        name => weldScoreText0;
        weldSend(14, stateIndex, 0, 0, 0, 0, 0);
    }

    fun void select(float stateIndex) { weldSend(15, stateIndex, 0, 0, 0, 0, 0); }
}

class TrackApi
{
    fun void add(float stateIndex, string name, string language)
    {
        name => weldScoreText0;
        language => weldScoreText1;
        weldSend(20, stateIndex, 0, 0, 0, 0, 0);
    }

    fun void remove(float stateIndex, float trackIndex) { weldSend(21, stateIndex, trackIndex, 0, 0, 0, 0); }

    fun void name(float stateIndex, float trackIndex, string name)
    {
        name => weldScoreText0;
        weldSend(22, stateIndex, trackIndex, 0, 0, 0, 0);
    }

    fun void language(float stateIndex, float trackIndex, string language)
    {
        language => weldScoreText0;
        weldSend(23, stateIndex, trackIndex, 0, 0, 0, 0);
    }

    fun void gain(float stateIndex, float trackIndex, float gain) { weldSend(24, stateIndex, trackIndex, gain, 0, 0, 0); }
    fun void sync(float stateIndex, float trackIndex, float isSynced) { weldSend(25, stateIndex, trackIndex, isSynced, 0, 0, 0); }
    fun void tempo(float stateIndex, float trackIndex, float bpm) { weldSend(26, stateIndex, trackIndex, bpm, 0, 0, 0); }
    fun void meter(float stateIndex, float trackIndex, float numerator, float denominator) { weldSend(27, stateIndex, trackIndex, numerator, denominator, 0, 0); }
    fun void phase(float stateIndex, float trackIndex, float beats) { weldSend(28, stateIndex, trackIndex, beats, 0, 0, 0); }

    fun void code(float stateIndex, float trackIndex, string code)
    {
        code => weldScoreText0;
        weldSend(29, stateIndex, trackIndex, 0, 0, 0, 0);
    }

    fun void template(float stateIndex, float trackIndex) { weldSend(30, stateIndex, trackIndex, 0, 0, 0, 0); }
    fun void clear(float stateIndex, float trackIndex) { weldSend(31, stateIndex, trackIndex, 0, 0, 0, 0); }
    fun void mute(float stateIndex, float trackIndex, float isMuted) { weldSend(32, stateIndex, trackIndex, isMuted, 0, 0, 0); }
    fun void solo(float stateIndex, float trackIndex, float isSoloed) { weldSend(33, stateIndex, trackIndex, isSoloed, 0, 0, 0); }
}

ScoreApi score;
StateApi state;
TrackApi track;

fun void play() { weldSend(2, 0, 0, 0, 0, 0, 0); }
fun void stop() { weldSend(3, 0, 0, 0, 0, 0, 0); }
fun void tempo(float bpm)
{
    Math.max(1.0, bpm) => weldLocalTempo;
    weldUpdateTime();
    weldSend(4, bpm, 0, 0, 0, 0, 0);
}
fun void meter(float numerator, float denominator)
{
    Math.max(1.0, numerator) => weldMeterNumerator;
    Math.max(1.0, denominator) => weldMeterDenominator;
    weldUpdateTime();
    weldSend(5, numerator, denominator, 0, 0, 0, 0);
}
fun void phase(float beats) { weldSend(6, beats, 0, 0, 0, 0, 0); }
fun void mixer(float visible) { weldSend(40, visible, 0, 0, 0, 0, 0); }
)chuck";
}

Program buildProgram (const juce::String& script)
{
    Program program;
    program.source = buildPrelude() + "\n" + script + "\nweldSend(998, 0, 0, 0, 0, 0, 0);\n";
    return program;
}
} // namespace ChucKScoreScript
