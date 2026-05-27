#include <WeldChucKEngine.h>

#include <array>
#include <cmath>
#include <iostream>

namespace
{
double bufferEnergy (const juce::AudioBuffer<float>& buffer)
{
    double energy = 0.0;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto value = buffer.getSample (channel, sample);

            if (! std::isfinite (value))
                return -1.0;

            energy += static_cast<double> (value) * static_cast<double> (value);
        }

    return energy;
}
}

int main()
{
    juce::ScopedNoDenormals noDenormals;

    EmbeddedChucKEngine engine;
    constexpr int blockSize = 128;

    if (! engine.prepare (48000.0, blockSize, 0, 2))
    {
        std::cerr << engine.getLastError() << '\n';
        return 1;
    }

    if (engine.getParameterIndex ("hostFreq") < 0
        || ! engine.setParameterValue ("hostFreq", 440.0f)
        || ! engine.setParameterValue ("hostGain", 0.10f))
    {
        std::cerr << "parameter access failed\n";
        return 2;
    }

    juce::AudioBuffer<float> input (0, blockSize);
    juce::AudioBuffer<float> output (2, blockSize);
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || engine.getRenderExceptionCount() != 0)
    {
        std::cerr << "render failed\n";
        return 3;
    }

    EmbeddedLanguageEngine languageEngine (EmbeddedLanguageEngine::Language::chuck);
    if (! languageEngine.prepare (48000.0, blockSize, 0, 2)
        || ! languageEngine.setParameterValue ("hostFreq", 330.0f)
        || ! languageEngine.setParameterValue ("hostGain", 0.08f))
    {
        std::cerr << "language host ChucK backend failed\n";
        return 4;
    }

    output.clear();
    languageEngine.process (input, output);
    if (bufferEnergy (output) <= 0.0 || languageEngine.getRenderExceptionCount() != 0)
    {
        std::cerr << "language host render failed\n";
        return 5;
    }

    const std::array unavailableLanguages
    {
        EmbeddedLanguageEngine::Language::faust,
        EmbeddedLanguageEngine::Language::csound,
        EmbeddedLanguageEngine::Language::supercollider,
        EmbeddedLanguageEngine::Language::rtcmix
    };

    for (const auto language : unavailableLanguages)
    {
        if (EmbeddedLanguageEngine::isLanguageBuiltIn (language))
            continue;

        EmbeddedLanguageEngine unavailableEngine (language);
        if (unavailableEngine.prepare (48000.0, blockSize, 0, 2) || unavailableEngine.isReady())
        {
            std::cerr << "unavailable backend prepared unexpectedly\n";
            return 6;
        }

        output.clear();
        output.setSample (0, 0, 1.0f);
        unavailableEngine.process (input, output);

        if (bufferEnergy (output) != 0.0 || unavailableEngine.getSilentProcessCount() == 0)
        {
            std::cerr << "unavailable backend did not fail silent\n";
            return 7;
        }

        if (unavailableEngine.getLastError().isEmpty())
        {
            std::cerr << "unavailable backend did not report an error\n";
            return 8;
        }
    }

    if (EmbeddedLanguageEngine::isLanguageBuiltIn (EmbeddedLanguageEngine::Language::csound))
    {
        EmbeddedLanguageEngine csoundEngine (EmbeddedLanguageEngine::Language::csound);
        if (! csoundEngine.prepare (48000.0, blockSize, 0, 2))
        {
            if (! csoundEngine.getLastError().containsIgnoreCase ("Could not load the embedded Csound runtime library"))
            {
                std::cerr << csoundEngine.getLastError() << '\n';
                return 90;
            }

            output.clear();
            output.setSample (0, 0, 1.0f);
            csoundEngine.process (input, output);

            if (bufferEnergy (output) != 0.0 || csoundEngine.getSilentProcessCount() == 0)
            {
                std::cerr << "Csound missing-runtime path did not fail silent\n";
                return 91;
            }
        }
        else
        {
            if (csoundEngine.getParameterIndex ("hostFreq") < 0
                || ! csoundEngine.setParameterValue ("hostFreq", 330.0f)
                || ! csoundEngine.setParameterValue ("hostGain", 0.06f)
                || ! csoundEngine.setParameterValue ("hostBlend", 0.4f))
            {
                std::cerr << "Csound parameter access failed\n";
                return 92;
            }

            double csoundEnergy = 0.0;
            for (int block = 0; block < 8; ++block)
            {
                output.clear();
                csoundEngine.process (input, output);
                csoundEnergy += bufferEnergy (output);
            }

            if (csoundEnergy <= 0.0 || csoundEngine.getRenderExceptionCount() != 0)
            {
                std::cerr << "Csound render failed: energy=" << csoundEnergy
                          << " renderExceptions=" << csoundEngine.getRenderExceptionCount()
                          << " renderedBlocks=" << csoundEngine.getRenderedBlockCount()
                          << " silentBlocks=" << csoundEngine.getSilentProcessCount()
                          << " internalErrors=" << csoundEngine.getInternalErrorCount()
                          << " lastError=" << csoundEngine.getLastError() << '\n';
                return 93;
            }

            const auto csoundOrchestra = juce::String (R"csound(
giWeldTestSine ftgen 2, 0, 4096, 10, 1

instr 1
    kfreq chnget "hostFreq"
    kgain chnget "hostGain"
    kblend chnget "hostBlend"
    aleft oscili kgain, kfreq, giWeldTestSine
    aright oscili kgain * (0.25 + kblend), kfreq * 1.5, giWeldTestSine
    outs aleft, aright
endin
)csound");

            if (! csoundEngine.loadProgram (csoundOrchestra, EmbeddedChucKEngine::getDefaultParameterBindings()))
            {
                std::cerr << csoundEngine.getLastError() << '\n';
                return 94;
            }

            csoundEnergy = 0.0;
            for (int block = 0; block < 8; ++block)
            {
                output.clear();
                csoundEngine.process (input, output);
                csoundEnergy += bufferEnergy (output);
            }

            if (csoundEnergy <= 0.0 || csoundEngine.getProgramLoadSuccessCount() < 2)
            {
                std::cerr << "Csound orchestra load failed to render\n";
                return 95;
            }
        }
    }

    if (EmbeddedLanguageEngine::isLanguageBuiltIn (EmbeddedLanguageEngine::Language::rtcmix))
    {
        EmbeddedLanguageEngine rtcmixEngine (EmbeddedLanguageEngine::Language::rtcmix);
        if (! rtcmixEngine.prepare (48000.0, blockSize, 0, 2))
        {
            if (! rtcmixEngine.getLastError().containsIgnoreCase ("Could not load the embedded RTcmix runtime library"))
            {
                std::cerr << rtcmixEngine.getLastError() << '\n';
                return 9;
            }

            output.clear();
            output.setSample (0, 0, 1.0f);
            rtcmixEngine.process (input, output);

            if (bufferEnergy (output) != 0.0 || rtcmixEngine.getSilentProcessCount() == 0)
            {
                std::cerr << "RTcmix missing-runtime path did not fail silent\n";
                return 9;
            }

            return 0;
        }

        if (rtcmixEngine.getParameterIndex ("hostFreq") < 0
            || ! rtcmixEngine.setParameterValue ("hostFreq", 275.0f)
            || ! rtcmixEngine.setParameterValue ("hostGain", 0.07f)
            || ! rtcmixEngine.setParameterValue ("hostBlend", 0.5f))
        {
            std::cerr << "RTcmix parameter access failed\n";
            return 10;
        }

        double rtcmixEnergy = 0.0;
        for (int block = 0; block < 8; ++block)
        {
            output.clear();
            rtcmixEngine.process (input, output);
            rtcmixEnergy += bufferEnergy (output);
        }

        if (rtcmixEnergy <= 0.0 || rtcmixEngine.getRenderExceptionCount() != 0)
        {
            std::cerr << "RTcmix render failed: energy=" << rtcmixEnergy
                      << " renderExceptions=" << rtcmixEngine.getRenderExceptionCount()
                      << " renderedBlocks=" << rtcmixEngine.getRenderedBlockCount()
                      << " silentBlocks=" << rtcmixEngine.getSilentProcessCount()
                      << " internalErrors=" << rtcmixEngine.getInternalErrorCount()
                      << " lastError=" << rtcmixEngine.getLastError() << '\n';
            return 11;
        }

        const auto inletScore = juce::String (R"rtcmix(
bus_config("WAVETABLE", "out 0")
freq = makeconnection("inlet", 1, 330)
gain = makeconnection("inlet", 2, 0.05)
pan = makeconnection("inlet", 3, 0.5)
wave = maketable("wave", 1024, 1, 0.5, 0.25)
WAVETABLE(0, 120, gain * 32767.0, freq, pan, wave)
)rtcmix");

        if (! rtcmixEngine.loadProgram (inletScore, EmbeddedChucKEngine::getDefaultParameterBindings()))
        {
            std::cerr << rtcmixEngine.getLastError() << '\n';
            return 12;
        }

        rtcmixEnergy = 0.0;
        for (int block = 0; block < 8; ++block)
        {
            output.clear();
            rtcmixEngine.process (input, output);
            rtcmixEnergy += bufferEnergy (output);
        }

        if (rtcmixEnergy <= 0.0 || rtcmixEngine.getProgramLoadSuccessCount() < 2)
        {
            std::cerr << "RTcmix score load failed to render\n";
            return 13;
        }
    }

    if (EmbeddedLanguageEngine::isLanguageBuiltIn (EmbeddedLanguageEngine::Language::supercollider))
    {
        EmbeddedLanguageEngine superColliderEngine (EmbeddedLanguageEngine::Language::supercollider);
        if (! superColliderEngine.prepare (48000.0, blockSize, 0, 2))
        {
            if (! superColliderEngine.getLastError().containsIgnoreCase ("Could not load the embedded SuperCollider"))
            {
                std::cerr << superColliderEngine.getLastError() << '\n';
                return 14;
            }

            output.clear();
            output.setSample (0, 0, 1.0f);
            superColliderEngine.process (input, output);

            if (bufferEnergy (output) != 0.0 || superColliderEngine.getSilentProcessCount() == 0)
            {
                std::cerr << "SuperCollider missing-runtime path did not fail silent\n";
                return 14;
            }

            return 0;
        }

        if (superColliderEngine.getParameterIndex ("hostFreq") < 0
            || ! superColliderEngine.setParameterValue ("hostFreq", 220.0f)
            || ! superColliderEngine.setParameterValue ("hostGain", 0.05f)
            || ! superColliderEngine.setParameterValue ("hostBlend", 0.25f))
        {
            std::cerr << "SuperCollider parameter access failed\n";
            return 15;
        }

        output.clear();
        superColliderEngine.process (input, output);

        if (bufferEnergy (output) != 0.0
            || superColliderEngine.getRenderExceptionCount() != 0
            || superColliderEngine.getRenderedBlockCount() == 0)
        {
            std::cerr << "SuperCollider host render path failed: energy=" << bufferEnergy (output)
                      << " renderExceptions=" << superColliderEngine.getRenderExceptionCount()
                      << " renderedBlocks=" << superColliderEngine.getRenderedBlockCount()
                      << " lastError=" << superColliderEngine.getLastError() << '\n';
            return 16;
        }

        if (! superColliderEngine.loadProgram ("{ |freq = 440, gain = 0.1| SinOsc.ar(freq) * gain }"))
        {
            std::cerr << "SuperCollider source compile failed: "
                      << superColliderEngine.getLastError() << '\n';
            return 17;
        }

        double superColliderEnergy = 0.0;
        for (int block = 0; block < 8; ++block)
        {
            output.clear();
            superColliderEngine.process (input, output);
            superColliderEnergy += bufferEnergy (output);
        }

        if (superColliderEnergy <= 0.0 || superColliderEngine.getRenderExceptionCount() != 0)
        {
            std::cerr << "SuperCollider compiled source failed to render: energy=" << superColliderEnergy
                      << " renderExceptions=" << superColliderEngine.getRenderExceptionCount()
                      << " renderedBlocks=" << superColliderEngine.getRenderedBlockCount()
                      << " lastError=" << superColliderEngine.getLastError() << '\n';
            return 18;
        }
    }

    {
        EmbeddedPerformanceEngine performance;
        if (! performance.prepare (48000.0, blockSize, 0, 2))
        {
            std::cerr << "Performance prepare failed: " << performance.getLastError() << '\n';
            return 19;
        }

        if (! performance.setTempoMap ({ { 0.0, 60.0 }, { 0.010, 120.0 } })
            || ! performance.setTimeSignatureMap ({ { 0.0, 4, 4 }, { 0.018, 3, 8 } })
            || ! performance.setPhaseRotationMap ({ { 0.0, 0.0 }, { 0.018, 0.25 } }))
        {
            std::cerr << "Performance musical-time map failed: " << performance.getLastError() << '\n';
            return 20;
        }

        const auto performanceChucK = juce::String (R"chuck(
SinOsc osc => Gain amp => dac;

while (true)
{
    hostFreq => osc.freq;
    hostGain * hostStateGain => amp.gain;
    1::samp => now;
}
)chuck");

        const std::vector<EmbeddedPerformanceEngine::State> states
        {
            { "chuck-a",
              EmbeddedLanguageEngine::Language::chuck,
              performanceChucK,
              EmbeddedChucKEngine::getDefaultParameterBindings(),
              0.010,
              0.010,
              {} },

            { "chuck-b",
              EmbeddedLanguageEngine::Language::chuck,
              performanceChucK,
              EmbeddedChucKEngine::getDefaultParameterBindings(),
              0.008,
              0.010,
              {} },

            { "chuck-c",
              EmbeddedLanguageEngine::Language::chuck,
              performanceChucK,
              EmbeddedChucKEngine::getDefaultParameterBindings(),
              0.010,
              0.005,
              {} }
        };

        if (! performance.loadSequence (states) || ! performance.start())
        {
            std::cerr << "Performance sequence failed: " << performance.getLastError() << '\n';
            return 21;
        }

        auto sawFirstTailOverlap = false;
        auto sawSecondTailOverlap = false;
        double performanceEnergy = 0.0;

        for (int block = 0; block < 30; ++block)
        {
            output.clear();
            performance.process (input, output);
            performanceEnergy += bufferEnergy (output);

            if (performance.getCurrentStateIndex() == 1 && performance.getActiveStateCount() >= 2)
                sawFirstTailOverlap = true;

            if (performance.getCurrentStateIndex() == 2 && performance.getActiveStateCount() >= 2)
                sawSecondTailOverlap = true;
        }

        if (performanceEnergy <= 0.0
            || ! sawFirstTailOverlap
            || ! sawSecondTailOverlap
            || performance.isPlaying()
            || performance.getRenderExceptionCount() != 0)
        {
            std::cerr << "Performance sequence did not preserve state tails: energy="
                      << performanceEnergy
                      << " firstOverlap=" << sawFirstTailOverlap
                      << " secondOverlap=" << sawSecondTailOverlap
                      << " playing=" << performance.isPlaying()
                      << " renderExceptions=" << performance.getRenderExceptionCount()
                      << " lastError=" << performance.getLastError() << '\n';
            return 22;
        }
    }

    {
        const auto multitrackChucK = juce::String (R"chuck(
SinOsc osc => Gain amp => dac;

while (true)
{
    hostFreq => osc.freq;
    hostGain * hostTrackGain * hostStateGain => amp.gain;
    1::samp => now;
}
)chuck");

        EmbeddedPerformanceEngine singleTrack;
        EmbeddedPerformanceEngine multiTrack;

        if (! singleTrack.prepare (48000.0, blockSize, 0, 2)
            || ! multiTrack.prepare (48000.0, blockSize, 0, 2))
        {
            std::cerr << "Multitrack performance prepare failed\n";
            return 23;
        }

        EmbeddedPerformanceEngine::State singleState;
        singleState.name = "single";
        singleState.durationBeats = 0.050;
        singleState.tailBeats = 0.0;
        singleState.tracks.push_back ({ "single-a",
                                        EmbeddedLanguageEngine::Language::chuck,
                                        multitrackChucK,
                                        EmbeddedChucKEngine::getDefaultParameterBindings(),
                                        1.0f,
                                        true,
                                        120.0,
                                        4,
                                        4,
                                        0.0,
                                        {} });

        EmbeddedPerformanceEngine::State multiState;
        multiState.name = "multi";
        multiState.durationBeats = 0.050;
        multiState.tailBeats = 0.0;
        multiState.tracks.push_back ({ "multi-a",
                                       EmbeddedLanguageEngine::Language::chuck,
                                       multitrackChucK,
                                       EmbeddedChucKEngine::getDefaultParameterBindings(),
                                       1.0f,
                                       true,
                                       120.0,
                                       4,
                                       4,
                                       0.0,
                                       {} });
        multiState.tracks.push_back ({ "multi-b",
                                       EmbeddedLanguageEngine::Language::chuck,
                                       multitrackChucK,
                                       EmbeddedChucKEngine::getDefaultParameterBindings(),
                                       1.0f,
                                       true,
                                       120.0,
                                       4,
                                       4,
                                       0.0,
                                       {} });

        if (! singleTrack.loadSequence ({ singleState })
            || ! multiTrack.loadSequence ({ multiState })
            || ! singleTrack.setTrackParameterValue (0, 0, "hostFreq", 220.0f)
            || ! multiTrack.setTrackParameterValue (0, 0, "hostFreq", 220.0f)
            || ! multiTrack.setTrackParameterValue (0, 1, "hostFreq", 440.0f)
            || ! singleTrack.start()
            || ! multiTrack.start())
        {
            std::cerr << "Multitrack performance setup failed: single="
                      << singleTrack.getLastError()
                      << " multi=" << multiTrack.getLastError() << '\n';
            return 24;
        }

        output.clear();
        singleTrack.process (input, output);
        const auto singleEnergy = bufferEnergy (output);

        output.clear();
        multiTrack.process (input, output);
        const auto multiEnergy = bufferEnergy (output);

        if (singleEnergy <= 0.0
            || multiEnergy <= singleEnergy * 1.20
            || multiTrack.getRenderExceptionCount() != 0)
        {
            std::cerr << "Multitrack performance did not render multiple tracks: singleEnergy="
                      << singleEnergy
                      << " multiEnergy=" << multiEnergy
                      << " renderExceptions=" << multiTrack.getRenderExceptionCount()
                      << " lastError=" << multiTrack.getLastError() << '\n';
            return 25;
        }

        if (! multiTrack.setTrackGain (0, 0, 0.0f)
            || ! multiTrack.setTrackGain (0, 1, 0.0f))
        {
            std::cerr << "Multitrack runtime gain update failed: "
                      << multiTrack.getLastError() << '\n';
            return 26;
        }

        output.clear();
        multiTrack.process (input, output);
        const auto mutedEnergy = bufferEnergy (output);

        if (mutedEnergy > 1.0e-8 || multiTrack.getRenderExceptionCount() != 0)
        {
            std::cerr << "Multitrack runtime gain update did not mute tracks: energy="
                      << mutedEnergy
                      << " renderExceptions=" << multiTrack.getRenderExceptionCount()
                      << " lastError=" << multiTrack.getLastError() << '\n';
            return 27;
        }
    }

    return 0;
}
