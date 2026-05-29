#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <WeldChucKEngine.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <iostream>
#include <limits>
#include <thread>

class ChucKAudioCallback final : public juce::AudioIODeviceCallback
{
public:
    static constexpr int maxHostChannels = 16;
    static constexpr int safetyBlockSize = 8192;
    static constexpr int maximumHostBlockSize = EmbeddedChucKEngine::maximumBlockSizeLimit;
    static constexpr double fallbackSampleRate = 44100.0;
    static constexpr int engineInputChannels = 2;
    static constexpr int engineOutputChannels = 2;

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : fallbackSampleRate;
        const auto reportedBufferSize = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
        prepareForDevice (sampleRate, reportedBufferSize);
    }

    bool prepareForDevice (double sampleRate, int reportedBufferSize)
    {
        juce::ScopedNoDenormals noDenormals;
        deviceReady.store (false, std::memory_order_release);
        engine.release();
        resetScratchBuffers();

        try
        {
            if (sampleRate <= 0.0 || ! std::isfinite (sampleRate))
            {
                juce::Logger::writeToLog ("Refusing invalid audio sample rate: " + juce::String (sampleRate));
                rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
                return false;
            }

            const auto bufferSize = juce::jmax (reportedBufferSize, safetyBlockSize);

            if (bufferSize > maximumHostBlockSize)
            {
                juce::Logger::writeToLog ("Refusing unsupported audio block size: " + juce::String (bufferSize));
                rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
                return false;
            }

            scratchInput.setSize (maxHostChannels, bufferSize, false, false, true);
            scratchOutput.setSize (maxHostChannels, bufferSize, false, false, true);
            scratchInput.clear();
            scratchOutput.clear();

            const auto prepared = engine.prepare (sampleRate, bufferSize, engineInputChannels, engineOutputChannels);
            engine.setFrequency (220.0f);
            engine.setGain (0.14f);
            engine.setToneBlend (0.35f);

            if (! prepared)
            {
                juce::Logger::writeToLog ("Embedded ChucK prepare failed: " + engine.getLastError());
                resetScratchBuffers();
                rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
            }

            if (prepared)
                successfulPrepareCount.fetch_add (1, std::memory_order_relaxed);

            deviceReady.store (prepared, std::memory_order_release);
            return prepared;
        }
        catch (const std::exception& e)
        {
            juce::Logger::writeToLog (juce::String ("Audio prepare exception: ") + e.what());
            resetScratchBuffers();
            deviceReady.store (false, std::memory_order_release);
            rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
        catch (...)
        {
            juce::Logger::writeToLog ("Unknown audio prepare exception");
            resetScratchBuffers();
            deviceReady.store (false, std::memory_order_release);
            rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
    }

    void audioDeviceStopped() override
    {
        deviceReady.store (false, std::memory_order_release);
        engine.release();
        resetScratchBuffers();
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        try
        {
            handleAudioCallback (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
        }
        catch (...)
        {
            clearHostOutputs (outputChannelData, numOutputChannels, numSamples);
            callbackExceptionCount.fetch_add (1, std::memory_order_relaxed);
            rejectedCallbackCount.fetch_add (1, std::memory_order_relaxed);
        }
    }

private:
    void handleAudioCallback (const float* const* inputChannelData,
                              int numInputChannels,
                              float* const* outputChannelData,
                              int numOutputChannels,
                              int numSamples)
    {
        juce::ScopedNoDenormals noDenormals;
        clearHostOutputs (outputChannelData, numOutputChannels, numSamples);

        if (! deviceReady.load (std::memory_order_acquire)
            || outputChannelData == nullptr
            || numSamples <= 0
            || numSamples > scratchInput.getNumSamples()
            || numSamples > scratchOutput.getNumSamples()
            || numOutputChannels <= 0
            || numOutputChannels > scratchOutput.getNumChannels())
        {
            if (outputChannelData == nullptr)
                nullOutputCallbackCount.fetch_add (1, std::memory_order_relaxed);

            rejectedCallbackCount.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        scratchInput.clear (0, numSamples);
        for (int channel = 0; channel < juce::jmin (numInputChannels, scratchInput.getNumChannels()); ++channel)
            if (inputChannelData != nullptr && inputChannelData[channel] != nullptr)
                scratchInput.copyFrom (channel, 0, inputChannelData[channel], numSamples);

        juce::AudioBuffer<float> outputView (scratchOutput.getArrayOfWritePointers(),
                                             numOutputChannels,
                                             numSamples);
        outputView.clear();
        engine.process (scratchInput, outputView);

        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[channel],
                                                   scratchOutput.getReadPointer (channel),
                                                   numSamples);

        renderedCallbackCount.fetch_add (1, std::memory_order_relaxed);
    }

    void resetScratchBuffers()
    {
        scratchInput.setSize (0, 0);
        scratchOutput.setSize (0, 0);
    }

    static void clearHostOutputs (float* const* outputChannelData, int numOutputChannels, int numSamples)
    {
        if (outputChannelData == nullptr || numSamples <= 0)
            return;

        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[channel], numSamples);
    }

public:
    EmbeddedChucKEngine engine;
    juce::AudioBuffer<float> scratchInput;
    juce::AudioBuffer<float> scratchOutput;
    std::atomic<bool> deviceReady { false };
    std::atomic<uint64_t> successfulPrepareCount { 0 };
    std::atomic<uint64_t> rejectedPrepareCount { 0 };
    std::atomic<uint64_t> renderedCallbackCount { 0 };
    std::atomic<uint64_t> rejectedCallbackCount { 0 };
    std::atomic<uint64_t> nullOutputCallbackCount { 0 };
    std::atomic<uint64_t> callbackExceptionCount { 0 };
};

class PerformanceAudioCallback final : public juce::AudioIODeviceCallback
{
public:
    static constexpr int maxHostChannels = 16;
    static constexpr int safetyBlockSize = 8192;
    static constexpr int maximumHostBlockSize = EmbeddedChucKEngine::maximumBlockSizeLimit;
    static constexpr double fallbackSampleRate = 44100.0;
    static constexpr int engineInputChannels = 0;
    static constexpr int engineOutputChannels = 2;

    explicit PerformanceAudioCallback (bool quickDemoToUse = false)
        : quickDemo (quickDemoToUse)
    {
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : fallbackSampleRate;
        const auto reportedBufferSize = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
        prepareForDevice (sampleRate, reportedBufferSize);
    }

    bool prepareForDevice (double sampleRate, int reportedBufferSize)
    {
        juce::ScopedNoDenormals noDenormals;
        deviceReady.store (false, std::memory_order_release);
        performance.release();
        resetScratchBuffers();

        if (sampleRate <= 0.0 || ! std::isfinite (sampleRate))
        {
            juce::Logger::writeToLog ("Refusing invalid performance sample rate: " + juce::String (sampleRate));
            rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        const auto bufferSize = juce::jmax (reportedBufferSize, safetyBlockSize);
        if (bufferSize > maximumHostBlockSize)
        {
            juce::Logger::writeToLog ("Refusing unsupported performance block size: " + juce::String (bufferSize));
            rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        scratchInput.setSize (juce::jmax (1, engineInputChannels), bufferSize, false, false, true);
        scratchOutput.setSize (maxHostChannels, bufferSize, false, false, true);
        scratchInput.clear();
        scratchOutput.clear();

        if (! performance.prepare (sampleRate, bufferSize, engineInputChannels, engineOutputChannels))
        {
            juce::Logger::writeToLog ("Performance prepare failed: " + performance.getLastError());
            rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
            resetScratchBuffers();
            return false;
        }

        if (! configureDemoSequence())
        {
            rejectedPrepareCount.fetch_add (1, std::memory_order_relaxed);
            resetScratchBuffers();
            return false;
        }

        successfulPrepareCount.fetch_add (1, std::memory_order_relaxed);
        deviceReady.store (true, std::memory_order_release);
        return true;
    }

    void audioDeviceStopped() override
    {
        deviceReady.store (false, std::memory_order_release);
        performance.release();
        resetScratchBuffers();
    }

    void audioDeviceIOCallbackWithContext (const float* const*,
                                           int,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        try
        {
            handleAudioCallback (outputChannelData, numOutputChannels, numSamples);
        }
        catch (...)
        {
            clearHostOutputs (outputChannelData, numOutputChannels, numSamples);
            callbackExceptionCount.fetch_add (1, std::memory_order_relaxed);
            rejectedCallbackCount.fetch_add (1, std::memory_order_relaxed);
        }
    }

    bool hasFinished() const noexcept
    {
        return deviceReady.load (std::memory_order_acquire) && ! performance.isPlaying();
    }

private:
    bool configureDemoSequence()
    {
        constexpr auto demoTempo = 120.0;

        const auto chuckSource = juce::String (R"chuck(
SinOsc left => Gain leftGain => dac.left;
SinOsc right => Gain rightGain => dac.right;

while (true)
{
    Math.max(40.0, hostFreq) => float f;
    Math.max(0.0, Math.min(hostGain, 0.08)) => float g;

    f => left.freq;
    f * 1.005 => right.freq;
    g * hostStateGain => leftGain.gain;
    g * hostStateGain => rightGain.gain;

    1::samp => now;
}
)chuck");

        const auto superColliderSource = juce::String (R"supercollider(
{ |freq = 330, gain = 0.04, blend = 0.5, stateGate = 1, stateGain = 1|
    var amp = gain.min(0.05) * stateGain;
    var left = SinOsc.ar(freq) * amp;
    var right = SinOsc.ar(freq * 1.003) * amp;
    [left, right]
}
)supercollider");

        const auto rtcmixScore = juce::String (R"rtcmix(
bus_config("WAVETABLE", "out 0")
freq = makeconnection("inlet", 1, 180)
gain = makeconnection("inlet", 2, 0.02)
pan = makeconnection("inlet", 3, 0.55)
stategain = makeconnection("inlet", 5, 1.0)
wave = maketable("wave", 4096, "sine")
WAVETABLE(0, 3600, gain * stategain * 32767.0, freq, pan, wave)
)rtcmix");

        const auto firstDurationSeconds = quickDemo ? 6.0 : 30.0;
        const auto secondDurationSeconds = quickDemo ? 5.0 : 20.0;
        const auto thirdDurationSeconds = quickDemo ? 6.0 : 25.0;
        const auto firstTailSeconds = quickDemo ? 2.0 : 8.0;
        const auto secondTailSeconds = quickDemo ? 2.0 : 8.0;
        const auto thirdTailSeconds = quickDemo ? 2.0 : 10.0;

        std::vector<EmbeddedPerformanceEngine::State> states
        {
            { "state-1-chuck",
              EmbeddedLanguageEngine::Language::chuck,
              chuckSource,
              EmbeddedChucKEngine::getDefaultParameterBindings(),
              EmbeddedPerformanceEngine::secondsToBeats (firstDurationSeconds, demoTempo),
              EmbeddedPerformanceEngine::secondsToBeats (firstTailSeconds, demoTempo),
              {} },

            { "state-2-supercollider",
              EmbeddedLanguageEngine::Language::supercollider,
              superColliderSource,
              EmbeddedChucKEngine::getDefaultParameterBindings(),
              EmbeddedPerformanceEngine::secondsToBeats (secondDurationSeconds, demoTempo),
              EmbeddedPerformanceEngine::secondsToBeats (secondTailSeconds, demoTempo),
              {} },

            { "state-3-rtcmix",
              EmbeddedLanguageEngine::Language::rtcmix,
              rtcmixScore,
              EmbeddedChucKEngine::getDefaultParameterBindings(),
              EmbeddedPerformanceEngine::secondsToBeats (thirdDurationSeconds, demoTempo),
              EmbeddedPerformanceEngine::secondsToBeats (thirdTailSeconds, demoTempo),
              {} }
        };

        if (! performance.setTempoMap ({ { 0.0, demoTempo } })
            || ! performance.setTimeSignatureMap ({ { 0.0, 4, 4 }, { 32.0, 7, 8 }, { 76.0, 3, 4 } })
            || ! performance.setPhaseRotationMap ({ { 0.0, 0.0 }, { 32.0, 0.5 }, { 76.0, -0.25 } })
            || ! performance.loadSequence (states)
            || ! performance.setStateParameterValue (0, "hostFreq", 220.0f)
            || ! performance.setStateParameterValue (0, "hostGain", 0.04f)
            || ! performance.setStateParameterValue (0, "hostBlend", 0.50f)
            || ! performance.setStateParameterValue (1, "hostFreq", 330.0f)
            || ! performance.setStateParameterValue (1, "hostGain", 0.035f)
            || ! performance.setStateParameterValue (1, "hostBlend", 0.50f)
            || ! performance.setStateParameterValue (2, "hostFreq", 165.0f)
            || ! performance.setStateParameterValue (2, "hostGain", 0.02f)
            || ! performance.setStateParameterValue (2, "hostBlend", 0.50f)
            || ! performance.start())
        {
            juce::Logger::writeToLog ("Performance demo setup failed: " + performance.getLastError());
            return false;
        }

        return true;
    }

    void handleAudioCallback (float* const* outputChannelData, int numOutputChannels, int numSamples)
    {
        juce::ScopedNoDenormals noDenormals;
        clearHostOutputs (outputChannelData, numOutputChannels, numSamples);

        if (! deviceReady.load (std::memory_order_acquire)
            || outputChannelData == nullptr
            || numSamples <= 0
            || numSamples > scratchOutput.getNumSamples()
            || numOutputChannels <= 0
            || numOutputChannels > scratchOutput.getNumChannels())
        {
            rejectedCallbackCount.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        scratchInput.clear (0, numSamples);
        juce::AudioBuffer<float> outputView (scratchOutput.getArrayOfWritePointers(),
                                             numOutputChannels,
                                             numSamples);
        outputView.clear();
        performance.process (scratchInput, outputView);

        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[channel],
                                                   scratchOutput.getReadPointer (channel),
                                                   numSamples);

        renderedCallbackCount.fetch_add (1, std::memory_order_relaxed);
    }

    void resetScratchBuffers()
    {
        scratchInput.setSize (0, 0);
        scratchOutput.setSize (0, 0);
    }

    static void clearHostOutputs (float* const* outputChannelData, int numOutputChannels, int numSamples)
    {
        if (outputChannelData == nullptr || numSamples <= 0)
            return;

        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[channel], numSamples);
    }

public:
    EmbeddedPerformanceEngine performance;
    juce::AudioBuffer<float> scratchInput;
    juce::AudioBuffer<float> scratchOutput;
    std::atomic<bool> deviceReady { false };
    std::atomic<uint64_t> successfulPrepareCount { 0 };
    std::atomic<uint64_t> rejectedPrepareCount { 0 };
    std::atomic<uint64_t> renderedCallbackCount { 0 };
    std::atomic<uint64_t> rejectedCallbackCount { 0 };
    std::atomic<uint64_t> callbackExceptionCount { 0 };

private:
    bool quickDemo = false;
};

namespace
{
using ParameterBinding = EmbeddedChucKEngine::ParameterBinding;

double bufferEnergy (const juce::AudioBuffer<float>& buffer)
{
    double energy = 0.0;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sampleIndex = 0; sampleIndex < buffer.getNumSamples(); ++sampleIndex)
        {
            const auto sample = buffer.getSample (channel, sampleIndex);

            if (! std::isfinite (sample))
                return -1.0;

            energy += static_cast<double> (sample) * static_cast<double> (sample);
        }

    return energy;
}

bool bufferIsSilent (const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sampleIndex = 0; sampleIndex < buffer.getNumSamples(); ++sampleIndex)
        {
            const auto sample = buffer.getSample (channel, sampleIndex);
            if (! std::isfinite (sample) || sample != 0.0f)
                return false;
        }

    return true;
}

bool bufferIsFiniteAndWithin (const juce::AudioBuffer<float>& buffer, float absoluteLimit)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sampleIndex = 0; sampleIndex < buffer.getNumSamples(); ++sampleIndex)
        {
            const auto sample = buffer.getSample (channel, sampleIndex);
            if (! std::isfinite (sample) || std::abs (sample) > absoluteLimit)
                return false;
        }

    return true;
}

bool valuesAreClose (float a, float b, float tolerance = 0.00001f) noexcept
{
    return std::abs (a - b) <= tolerance;
}

uint32_t nextRandom (uint32_t& state) noexcept
{
    state = state * 1664525u + 1013904223u;
    return state;
}

float nextAudioSample (uint32_t& state) noexcept
{
    const auto unit = static_cast<float> ((nextRandom (state) >> 8) * (1.0 / 16777215.0));
    return (unit * 2.0f - 1.0f) * 0.35f;
}

const juce::String hostControlledProgram()
{
    return R"chuck(
TriOsc tri => Gain triGain => dac;

while (true)
{
    Math.max(30.0, hostFreq * 1.25) => tri.freq;
    Math.max(0.0, Math.min(hostGain, 0.35)) => triGain.gain;
    5::ms => now;
}
)chuck";
}

const juce::String fixedProgram()
{
    return R"chuck(
SinOsc fixed => dac;
330.0 => fixed.freq;
0.11 => fixed.gain;

while (true)
{
    5::ms => now;
}
)chuck";
}

const juce::String malformedProgram()
{
    return "this is not valid chuck code;";
}

const juce::String boundProgram()
{
    return R"chuck(
TriOsc osc => Gain out => dac;

while (true)
{
    Math.max(40.0, toneHz * ratio) => osc.freq;
    Math.max(0.0, Math.min(amp, 0.25)) => out.gain;
    5::ms => now;
}
)chuck";
}

const juce::String alternateBoundProgram()
{
    return R"chuck(
SawOsc osc => LPF filt => Gain out => dac;
0.7 => filt.Q;

while (true)
{
    Math.max(40.0, toneHz) => osc.freq;
    Math.max(120.0, toneHz * ratio) => filt.freq;
    Math.max(0.0, Math.min(amp, 0.25)) => out.gain;
    5::ms => now;
}
)chuck";
}

std::vector<ParameterBinding> boundProgramBindings()
{
    return
    {
        { "toneHz", 220.0f, 40.0f, 1200.0f },
        { "amp", 0.10f, 0.0f, 0.25f },
        { "ratio", 1.0f, 0.25f, 4.0f }
    };
}

int expectPrepareFailure (double sampleRate, int blockSize, int inputChannels, int outputChannels, const char* label)
{
    EmbeddedChucKEngine engine;

    if (engine.prepare (sampleRate, blockSize, inputChannels, outputChannels) || engine.isReady())
    {
        juce::Logger::writeToLog (juce::String ("Self-test failed: prepare should have failed for ") + label);
        return 1;
    }

    return 0;
}

int runSelfTest()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr int blockSize = 512;
    constexpr int renderBlocks = 32;

    if (expectPrepareFailure (0.0, blockSize, 2, 2, "invalid sample rate") != 0)
        return 2;

    if (expectPrepareFailure (48000.0, 0, 2, 2, "zero block size") != 0)
        return 3;

    if (expectPrepareFailure (48000.0, blockSize, -1, 2, "negative input channels") != 0)
        return 4;

    if (expectPrepareFailure (48000.0, blockSize, 2, 0, "zero output channels") != 0)
        return 5;

    {
        EmbeddedChucKEngine coldEngine;
        juce::AudioBuffer<float> coldInput (2, blockSize);
        juce::AudioBuffer<float> coldOutput (2, blockSize);
        coldOutput.clear();
        coldOutput.setSample (0, 0, 1.0f);
        coldEngine.process (coldInput, coldOutput);

        if (! bufferIsSilent (coldOutput) || coldEngine.getSilentProcessCount() == 0)
        {
            juce::Logger::writeToLog ("Self-test failed: cold engine did not fail silent");
            return 6;
        }
    }

    EmbeddedChucKEngine noInputEngine;
    if (! noInputEngine.prepare (48000.0, blockSize, 0, 2))
    {
        juce::Logger::writeToLog ("Self-test no-input prepare failed: " + noInputEngine.getLastError());
        return 7;
    }

    juce::AudioBuffer<float> noInput (0, blockSize);
    juce::AudioBuffer<float> noInputOutput (2, blockSize);
    noInputEngine.process (noInput, noInputOutput);

    if (bufferEnergy (noInputOutput) <= 0.0)
    {
        juce::Logger::writeToLog ("Self-test failed: no-input engine rendered silence");
        return 8;
    }

    EmbeddedChucKEngine shortInputEngine;
    if (! shortInputEngine.prepare (48000.0, blockSize, 2, 4))
    {
        juce::Logger::writeToLog ("Self-test short-input prepare failed: " + shortInputEngine.getLastError());
        return 9;
    }

    juce::AudioBuffer<float> shortInput (1, blockSize / 2);
    juce::AudioBuffer<float> wideOutput (4, blockSize);
    shortInput.clear();
    wideOutput.clear();
    shortInputEngine.process (shortInput, wideOutput);

    if (bufferEnergy (wideOutput) <= 0.0)
    {
        juce::Logger::writeToLog ("Self-test failed: short-input/wide-output render failed");
        return 10;
    }

    EmbeddedChucKEngine engine;
    if (! engine.prepare (48000.0, blockSize, 2, 2))
    {
        juce::Logger::writeToLog ("Self-test prepare failed: " + engine.getLastError());
        return 11;
    }

    engine.setFrequency (330.0f);
    engine.setGain (0.12f);
    engine.setToneBlend (0.5f);

    juce::AudioBuffer<float> input (2, blockSize);
    juce::AudioBuffer<float> output (2, blockSize);

    double energy = 0.0;
    float peak = 0.0f;

    for (int block = 0; block < renderBlocks; ++block)
    {
        input.clear();
        output.clear();

        if (block == 1)
            input.setSample (0, 0, std::numeric_limits<float>::infinity());

        engine.process (input, output);

        for (int channel = 0; channel < output.getNumChannels(); ++channel)
            for (int sampleIndex = 0; sampleIndex < output.getNumSamples(); ++sampleIndex)
            {
                const auto sample = output.getSample (channel, sampleIndex);

                if (! std::isfinite (sample))
                {
                    juce::Logger::writeToLog ("Self-test failed: non-finite output sample");
                    return 12;
                }

                peak = juce::jmax (peak, std::abs (sample));
                energy += static_cast<double> (sample) * static_cast<double> (sample);
            }
    }

    if (energy <= 0.0 || peak <= 0.0f)
    {
        juce::Logger::writeToLog ("Self-test failed: embedded ChucK rendered silence");
        return 13;
    }

    if (engine.getRenderedBlockCount() != static_cast<uint64_t> (renderBlocks)
        || engine.getRenderedFrameCount() != static_cast<uint64_t> (renderBlocks * blockSize))
    {
        juce::Logger::writeToLog ("Self-test failed: rendered block/frame counters are wrong");
        return 14;
    }

    const auto renderedBlocksBeforeOversized = engine.getRenderedBlockCount();
    juce::AudioBuffer<float> oversizedOutput (2, blockSize + 1);
    engine.process (input, oversizedOutput);

    if (engine.getOversizedBlockCount() == 0)
    {
        juce::Logger::writeToLog ("Self-test failed: oversized block was not counted");
        return 15;
    }

    if (engine.getRenderedBlockCount() != renderedBlocksBeforeOversized)
    {
        juce::Logger::writeToLog ("Self-test failed: oversized render was counted as rendered");
        return 16;
    }

    if (engine.getRenderExceptionCount() != 0
        || engine.getSilentProcessCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        juce::Logger::writeToLog ("Self-test failed: unexpected engine diagnostic count");
        return 17;
    }

    if (engine.getSanitisedSampleCount() == 0)
    {
        juce::Logger::writeToLog ("Self-test failed: non-finite input sanitising was not counted");
        return 18;
    }

    engine.release();
    output.clear();
    output.setSample (0, 0, 1.0f);
    engine.process (input, output);

    if (! bufferIsSilent (output) || engine.getSilentProcessCount() == 0)
    {
        juce::Logger::writeToLog ("Self-test failed: released engine did not fail silent");
        return 19;
    }

    juce::Logger::writeToLog ("Self-test passed: peak="
                              + juce::String (peak)
                              + " energy="
                              + juce::String (energy)
                              + " sanitisedSamples="
                              + juce::String (static_cast<juce::int64> (engine.getSanitisedSampleCount())));
    return 0;
}

int runStressTest()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr int iterations = 64;
    const int blockSizes[] = { 64, 127, 256, 511, 1024 };
    constexpr int numBlockSizes = 5;

    for (int i = 0; i < iterations; ++i)
    {
        const auto blockSize = blockSizes[i % numBlockSizes];
        EmbeddedChucKEngine engine;

        if (! engine.prepare (44100.0 + static_cast<double> (i % 3) * 2400.0, blockSize, i % 2, 2))
        {
            juce::Logger::writeToLog ("Stress-test prepare failed: " + engine.getLastError());
            return 20;
        }

        engine.setFrequency (110.0f + static_cast<float> (i * 7));
        engine.setGain (0.08f + static_cast<float> (i % 4) * 0.02f);
        engine.setToneBlend (static_cast<float> (i % 10) / 10.0f);

        juce::AudioBuffer<float> input (juce::jmax (1, i % 2), blockSize);
        juce::AudioBuffer<float> output (2, blockSize);

        for (int block = 0; block < 8; ++block)
        {
            input.clear();
            output.clear();
            engine.process (input, output);

            const auto energy = bufferEnergy (output);
            if (energy <= 0.0)
            {
                juce::Logger::writeToLog ("Stress-test failed: silent render");
                return 21;
            }

            if (engine.getRenderExceptionCount() != 0
                || engine.getSilentProcessCount() != 0
                || engine.getInternalErrorCount() != 0)
            {
                juce::Logger::writeToLog ("Stress-test failed: unexpected diagnostic count");
                return 22;
            }
        }

        engine.release();
        output.clear();
        output.setSample (0, 0, 1.0f);
        engine.process (input, output);

        if (! bufferIsSilent (output))
        {
            juce::Logger::writeToLog ("Stress-test failed: released engine emitted audio");
            return 23;
        }
    }

    juce::Logger::writeToLog ("Stress-test passed: iterations=" + juce::String (iterations));
    return 0;
}

int runFuzzTest()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr float expectedOutputLimit = 0.981f;
    constexpr int blocksPerConfig = 24;
    const int preparedBlockSizes[] = { 65, 128, 257, 512, 1024, 2048 };
    constexpr int numConfigs = 6;
    uint32_t randomState = 0xc0dec0deu;
    double accumulatedEnergy = 0.0;

    for (int config = 0; config < numConfigs; ++config)
    {
        const auto preparedBlockSize = preparedBlockSizes[config];
        const auto preparedInputs = config % 3;
        const auto preparedOutputs = 1 + (config % 4);

        EmbeddedChucKEngine engine;
        if (! engine.prepare (44100.0 + static_cast<double> (config) * 1102.5,
                              preparedBlockSize,
                              preparedInputs,
                              preparedOutputs))
        {
            juce::Logger::writeToLog ("Fuzz-test prepare failed: " + engine.getLastError());
            return 40;
        }

        for (int block = 0; block < blocksPerConfig; ++block)
        {
            const auto controlsBefore = engine.getSanitisedControlCount();

            if (block % 11 == 0)
            {
                engine.setFrequency (std::numeric_limits<float>::infinity());
                engine.setGain (std::numeric_limits<float>::quiet_NaN());
                engine.setToneBlend (-10.0f);

                if (engine.getSanitisedControlCount() < controlsBefore + 3)
                {
                    juce::Logger::writeToLog ("Fuzz-test failed: malformed control values were not counted");
                    return 46;
                }
            }
            else
            {
                engine.setFrequency (20.0f + static_cast<float> (nextRandom (randomState) % 6000u));
                engine.setGain (static_cast<float> (nextRandom (randomState) % 100u) / 160.0f);
                engine.setToneBlend (static_cast<float> (nextRandom (randomState) % 140u) / 100.0f - 0.2f);
            }

            const auto inputChannels = static_cast<int> (nextRandom (randomState) % 5u);
            const auto inputFrames = static_cast<int> (nextRandom (randomState) % static_cast<uint32_t> (preparedBlockSize + 37));
            const auto outputChannels = 1 + static_cast<int> (nextRandom (randomState) % 6u);
            const auto outputFrames = block % 17 == 0
                                        ? preparedBlockSize + 1
                                        : static_cast<int> (nextRandom (randomState) % static_cast<uint32_t> (preparedBlockSize + 1));

            juce::AudioBuffer<float> input (inputChannels, inputFrames);
            juce::AudioBuffer<float> output (outputChannels, outputFrames);

            for (int channel = 0; channel < input.getNumChannels(); ++channel)
                for (int sampleIndex = 0; sampleIndex < input.getNumSamples(); ++sampleIndex)
                    input.setSample (channel, sampleIndex, nextAudioSample (randomState));

            const auto injectedBadInput = preparedInputs > 0
                                          && inputChannels > 0
                                          && inputFrames > 0
                                          && outputFrames > 0
                                          && outputFrames <= preparedBlockSize
                                          && block % 5 == 0;

            if (injectedBadInput)
            {
                const auto selector = block % 3;
                input.setSample (0,
                                 0,
                                 selector == 0 ? std::numeric_limits<float>::infinity()
                                               : selector == 1 ? std::numeric_limits<float>::quiet_NaN()
                                                               : 42.0f);
            }

            output.clear();
            if (outputFrames > 0)
                output.setSample (0, 0, 1.0f);

            const auto sanitisedBefore = engine.getSanitisedSampleCount();
            const auto oversizedBefore = engine.getOversizedBlockCount();
            engine.process (input, output);

            if (outputFrames > preparedBlockSize)
            {
                if (engine.getOversizedBlockCount() == oversizedBefore || ! bufferIsSilent (output))
                {
                    juce::Logger::writeToLog ("Fuzz-test failed: oversized render did not fail silent");
                    return 41;
                }

                continue;
            }

            if (! bufferIsFiniteAndWithin (output, expectedOutputLimit))
            {
                juce::Logger::writeToLog ("Fuzz-test failed: non-finite or unclamped output sample");
                return 42;
            }

            if (injectedBadInput && engine.getSanitisedSampleCount() == sanitisedBefore)
            {
                juce::Logger::writeToLog ("Fuzz-test failed: malformed input sample was not sanitised");
                return 43;
            }

            if (engine.getRenderExceptionCount() != 0
                || engine.getSilentProcessCount() != 0
                || engine.getInternalErrorCount() != 0)
            {
                juce::Logger::writeToLog ("Fuzz-test failed: unexpected diagnostic count");
                return 44;
            }

            accumulatedEnergy += bufferEnergy (output);
        }
    }

    if (accumulatedEnergy <= 0.0)
    {
        juce::Logger::writeToLog ("Fuzz-test failed: all valid renders were silent");
        return 45;
    }

    juce::Logger::writeToLog ("Fuzz-test passed: configs=" + juce::String (numConfigs)
                              + " blocks=" + juce::String (numConfigs * blocksPerConfig));
    return 0;
}

int runProgramTest()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr int blockSize = 256;
    constexpr float expectedOutputLimit = 0.981f;

    {
        EmbeddedChucKEngine coldEngine;
        if (coldEngine.loadProgram (fixedProgram()) || coldEngine.isReady())
        {
            juce::Logger::writeToLog ("Program-test failed: cold program load should fail");
            return 70;
        }

        if (coldEngine.getProgramLoadFailureCount() == 0)
        {
            juce::Logger::writeToLog ("Program-test failed: cold program load failure was not counted");
            return 71;
        }
    }

    EmbeddedChucKEngine engine;
    if (! engine.prepare (48000.0, blockSize, 2, 2))
    {
        juce::Logger::writeToLog ("Program-test prepare failed: " + engine.getLastError());
        return 72;
    }

    if (engine.getCurrentProgram().isEmpty()
        || engine.getProgramLoadSuccessCount() != 1
        || engine.getProgramLoadFailureCount() != 0)
    {
        juce::Logger::writeToLog ("Program-test failed: initial program state is wrong");
        return 73;
    }

    juce::AudioBuffer<float> input (2, blockSize);
    juce::AudioBuffer<float> output (2, blockSize);
    input.clear();
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || ! bufferIsFiniteAndWithin (output, expectedOutputLimit))
    {
        juce::Logger::writeToLog ("Program-test failed: initial program render was invalid");
        return 74;
    }

    const auto programA = hostControlledProgram();
    if (! engine.loadProgram (programA)
        || ! engine.isReady()
        || engine.getCurrentProgram() != programA
        || engine.getProgramLoadSuccessCount() != 2)
    {
        juce::Logger::writeToLog ("Program-test failed: valid transactional load did not commit");
        return 75;
    }

    engine.setFrequency (550.0f);
    engine.setGain (0.10f);
    engine.setToneBlend (0.0f);
    input.clear();
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || ! bufferIsFiniteAndWithin (output, expectedOutputLimit))
    {
        juce::Logger::writeToLog ("Program-test failed: loaded program render was invalid");
        return 76;
    }

    const auto successesBeforeBadLoad = engine.getProgramLoadSuccessCount();
    const auto failuresBeforeBadLoad = engine.getProgramLoadFailureCount();
    if (engine.loadProgram (malformedProgram())
        || ! engine.isReady()
        || engine.getCurrentProgram() != programA
        || engine.getProgramLoadSuccessCount() != successesBeforeBadLoad
        || engine.getProgramLoadFailureCount() == failuresBeforeBadLoad)
    {
        juce::Logger::writeToLog ("Program-test failed: malformed program did not preserve last good program");
        return 77;
    }

    input.clear();
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || ! bufferIsFiniteAndWithin (output, expectedOutputLimit))
    {
        juce::Logger::writeToLog ("Program-test failed: last good program did not survive malformed load");
        return 78;
    }

    const auto programB = fixedProgram();
    if (! engine.loadProgram (programB)
        || engine.getCurrentProgram() != programB
        || engine.getProgramLoadSuccessCount() != successesBeforeBadLoad + 1)
    {
        juce::Logger::writeToLog ("Program-test failed: recovery load after malformed program did not commit");
        return 79;
    }

    input.clear();
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || ! bufferIsFiniteAndWithin (output, expectedOutputLimit))
    {
        juce::Logger::writeToLog ("Program-test failed: recovery program render was invalid");
        return 80;
    }

    std::atomic<bool> stopRequested { false };
    std::atomic<bool> failed { false };
    std::atomic<int> audibleBlocks { 0 };

    std::thread audioThread ([&]
    {
        juce::ScopedNoDenormals threadNoDenormals;
        juce::AudioBuffer<float> threadInput (2, blockSize);
        juce::AudioBuffer<float> threadOutput (2, blockSize);

        while (! stopRequested.load (std::memory_order_acquire))
        {
            threadInput.clear();
            threadOutput.clear();
            engine.process (threadInput, threadOutput);

            if (! bufferIsFiniteAndWithin (threadOutput, expectedOutputLimit))
                failed.store (true, std::memory_order_release);

            if (! bufferIsSilent (threadOutput))
                audibleBlocks.fetch_add (1, std::memory_order_relaxed);

            std::this_thread::yield();
        }
    });

    const juce::String validPrograms[] = { programA, programB, EmbeddedChucKEngine::getDefaultProgram() };

    for (int i = 0; i < 24 && ! failed.load (std::memory_order_acquire); ++i)
    {
        if (i % 6 == 5)
        {
            if (engine.loadProgram (malformedProgram()) || ! engine.isReady())
            {
                failed.store (true, std::memory_order_release);
                break;
            }
        }
        else if (! engine.loadProgram (validPrograms[i % 3]))
        {
            failed.store (true, std::memory_order_release);
            break;
        }
    }

    stopRequested.store (true, std::memory_order_release);
    audioThread.join();

    if (failed.load (std::memory_order_acquire))
    {
        juce::Logger::writeToLog ("Program-test failed: reload storm produced invalid output or bad state");
        return 81;
    }

    if (audibleBlocks.load (std::memory_order_relaxed) == 0
        || engine.getProgramLoadSuccessCount() < 20
        || engine.getProgramLoadFailureCount() < 5
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        juce::Logger::writeToLog ("Program-test failed: reload storm diagnostics are wrong");
        return 82;
    }

    juce::Logger::writeToLog ("Program-test passed: programSuccesses="
                              + juce::String (static_cast<juce::int64> (engine.getProgramLoadSuccessCount()))
                              + " programFailures="
                              + juce::String (static_cast<juce::int64> (engine.getProgramLoadFailureCount())));
    return 0;
}

int runParameterBindingTest()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr int blockSize = 256;
    constexpr float expectedOutputLimit = 0.981f;

    EmbeddedChucKEngine engine;
    if (! engine.prepare (48000.0, blockSize, 2, 2))
    {
        juce::Logger::writeToLog ("Parameter-binding test prepare failed: " + engine.getLastError());
        return 90;
    }

    const auto defaultBindings = engine.getCurrentParameterBindings();
    if (defaultBindings.size() != 3
        || engine.getParameterIndex ("hostFreq") < 0
        || engine.getParameterIndex ("hostGain") < 0
        || engine.getParameterIndex ("hostBlend") < 0)
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: default bindings are wrong");
        return 91;
    }

    const auto customBindings = boundProgramBindings();
    if (! engine.loadProgram (boundProgram(), customBindings)
        || engine.getParameterCount() != static_cast<int> (customBindings.size())
        || engine.getParameterIndex ("toneHz") != 0
        || engine.getParameterIndex ("amp") != 1
        || engine.getParameterIndex ("ratio") != 2
        || engine.getParameterIndex ("hostFreq") >= 0)
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: custom bindings did not commit");
        return 92;
    }

    if (! engine.setParameterValue (0, 600.0f)
        || ! engine.setParameterValue ("amp", 0.18f)
        || ! engine.setParameterValue (2, 1.5f)
        || engine.setParameterValue ("missing", 0.1f))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: parameter setter result was wrong");
        return 93;
    }

    const auto sanitisedBefore = engine.getSanitisedControlCount();
    if (! engine.setParameterValue (0, std::numeric_limits<float>::infinity())
        || ! valuesAreClose (engine.getParameterValue (0), customBindings[0].defaultValue)
        || ! engine.setParameterValue (1, 99.0f)
        || ! valuesAreClose (engine.getParameterValue (1), customBindings[1].maximumValue)
        || engine.getSanitisedControlCount() < sanitisedBefore + 2)
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: parameter sanitising was wrong");
        return 94;
    }

    if (! engine.setParameterValue (0, 777.0f)
        || ! engine.loadProgram (alternateBoundProgram(), customBindings)
        || ! valuesAreClose (engine.getParameterValue (0), 777.0f))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: same-name parameter value was not preserved");
        return 95;
    }

    juce::AudioBuffer<float> input (2, blockSize);
    juce::AudioBuffer<float> output (2, blockSize);
    input.clear();
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || ! bufferIsFiniteAndWithin (output, expectedOutputLimit))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: custom-bound render was invalid");
        return 96;
    }

    const auto currentProgram = engine.getCurrentProgram();
    const auto currentBindings = engine.getCurrentParameterBindings();
    const auto loadFailuresBeforeInvalid = engine.getProgramLoadFailureCount();

    if (engine.loadProgram (boundProgram(), { { "bad-name", 0.0f, 0.0f, 1.0f } })
        || engine.getCurrentProgram() != currentProgram
        || engine.getCurrentParameterBindings().size() != currentBindings.size()
        || engine.getProgramLoadFailureCount() == loadFailuresBeforeInvalid)
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: invalid binding name was not rejected transactionally");
        return 97;
    }

    if (engine.loadProgram (boundProgram(), { { "amp", 0.1f, 0.0f, 1.0f }, { "amp", 0.2f, 0.0f, 1.0f } }))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: duplicate binding was accepted");
        return 98;
    }

    if (engine.loadProgram (boundProgram(), { { "amp", 2.0f, 0.0f, 1.0f } }))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: out-of-range default was accepted");
        return 99;
    }

    std::vector<ParameterBinding> tooManyBindings;
    for (int i = 0; i <= EmbeddedChucKEngine::maximumParameterCount; ++i)
        tooManyBindings.push_back ({ "p" + juce::String (i), 0.0f, 0.0f, 1.0f });

    if (engine.loadProgram (boundProgram(), tooManyBindings))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: too many bindings were accepted");
        return 100;
    }

    if (! engine.loadProgram (fixedProgram(), {})
        || engine.getParameterCount() != 0
        || engine.setParameterValue (0, 0.5f)
        || engine.setParameterValue ("amp", 0.5f))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: empty binding set did not commit cleanly");
        return 101;
    }

    input.clear();
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || ! bufferIsFiniteAndWithin (output, expectedOutputLimit))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: zero-binding program render was invalid");
        return 102;
    }

    if (! engine.loadProgram (boundProgram(), customBindings))
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: custom bindings did not recover after empty set");
        return 103;
    }

    std::atomic<bool> stopRequested { false };
    std::atomic<bool> failed { false };
    std::thread controlThread ([&]
    {
        uint32_t randomState = 0x51a7e5u;

        while (! stopRequested.load (std::memory_order_acquire))
        {
            const auto index = static_cast<int> (nextRandom (randomState) % static_cast<uint32_t> (customBindings.size()));
            const auto value = static_cast<float> (nextRandom (randomState) % 1600u) / 2.0f;

            if (! engine.setParameterValue (index, value))
                failed.store (true, std::memory_order_release);

            std::this_thread::yield();
        }
    });

    for (int i = 0; i < 16 && ! failed.load (std::memory_order_acquire); ++i)
    {
        if (! engine.loadProgram ((i % 2) == 0 ? boundProgram() : alternateBoundProgram(), customBindings))
        {
            failed.store (true, std::memory_order_release);
            break;
        }

        input.clear();
        output.clear();
        engine.process (input, output);

        if (! bufferIsFiniteAndWithin (output, expectedOutputLimit))
        {
            failed.store (true, std::memory_order_release);
            break;
        }
    }

    stopRequested.store (true, std::memory_order_release);
    controlThread.join();

    if (failed.load (std::memory_order_acquire)
        || engine.getInternalErrorCount() != 0
        || engine.getRenderExceptionCount() != 0)
    {
        juce::Logger::writeToLog ("Parameter-binding test failed: reload/control race produced invalid state");
        return 104;
    }

    juce::Logger::writeToLog ("Parameter-binding test passed: parameters="
                              + juce::String (engine.getParameterCount())
                              + " sanitisedControls="
                              + juce::String (static_cast<juce::int64> (engine.getSanitisedControlCount())));
    return 0;
}

int runAsyncProgramLoadTest()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr int blockSize = 256;
    constexpr float expectedOutputLimit = 0.981f;

    {
        EmbeddedChucKEngine coldEngine;
        if (coldEngine.loadProgramAsync (fixedProgram()) || coldEngine.isAsyncProgramLoadActive())
        {
            juce::Logger::writeToLog ("Async-program test failed: cold async load should fail without queueing");
            return 110;
        }

        if (coldEngine.getProgramLoadFailureCount() == 0
            || coldEngine.getAsyncProgramLoadQueuedCount() != 0
            || coldEngine.getAsyncProgramLoadCompletedCount() != 0)
        {
            juce::Logger::writeToLog ("Async-program test failed: cold async diagnostics are wrong");
            return 111;
        }
    }

    EmbeddedChucKEngine engine;
    if (! engine.prepare (48000.0, blockSize, 2, 2))
    {
        juce::Logger::writeToLog ("Async-program test prepare failed: " + engine.getLastError());
        return 112;
    }

    const auto customBindings = boundProgramBindings();
    if (! engine.loadProgramAsync (boundProgram(), customBindings)
        || ! engine.isAsyncProgramLoadActive()
        || engine.getAsyncProgramLoadQueuedCount() != 1)
    {
        juce::Logger::writeToLog ("Async-program test failed: valid async load was not queued");
        return 113;
    }

    if (! engine.waitForAsyncProgramLoads (5000)
        || engine.isAsyncProgramLoadActive()
        || engine.getAsyncProgramLoadCompletedCount() != 1
        || engine.getCurrentProgram() != boundProgram()
        || engine.getParameterIndex ("toneHz") != 0
        || engine.getParameterIndex ("hostFreq") >= 0)
    {
        juce::Logger::writeToLog ("Async-program test failed: valid async load did not commit");
        return 114;
    }

    if (! engine.setParameterValue ("amp", 0.16f)
        || ! engine.setParameterValue ("toneHz", 440.0f))
    {
        juce::Logger::writeToLog ("Async-program test failed: async-bound parameter setters failed");
        return 115;
    }

    juce::AudioBuffer<float> input (2, blockSize);
    juce::AudioBuffer<float> output (2, blockSize);
    input.clear();
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || ! bufferIsFiniteAndWithin (output, expectedOutputLimit))
    {
        juce::Logger::writeToLog ("Async-program test failed: async-loaded program render was invalid");
        return 116;
    }

    const auto programBeforeBadLoad = engine.getCurrentProgram();
    const auto successesBeforeBadLoad = engine.getProgramLoadSuccessCount();
    const auto failuresBeforeBadLoad = engine.getProgramLoadFailureCount();

    if (! engine.loadProgramAsync (malformedProgram(), customBindings)
        || ! engine.waitForAsyncProgramLoads (5000)
        || engine.getCurrentProgram() != programBeforeBadLoad
        || engine.getProgramLoadSuccessCount() != successesBeforeBadLoad
        || engine.getProgramLoadFailureCount() == failuresBeforeBadLoad)
    {
        juce::Logger::writeToLog ("Async-program test failed: malformed async load did not preserve current VM");
        return 117;
    }

    if (engine.loadProgramAsync (boundProgram(), { { "bad-name", 0.0f, 0.0f, 1.0f } })
        || engine.getAsyncProgramLoadQueuedCount() != 2)
    {
        juce::Logger::writeToLog ("Async-program test failed: invalid async binding should fail before queueing");
        return 118;
    }

    std::atomic<bool> stopRequested { false };
    std::atomic<bool> failed { false };
    std::atomic<int> audibleBlocks { 0 };

    std::thread audioThread ([&]
    {
        juce::ScopedNoDenormals threadNoDenormals;
        juce::AudioBuffer<float> threadInput (2, blockSize);
        juce::AudioBuffer<float> threadOutput (2, blockSize);

        while (! stopRequested.load (std::memory_order_acquire))
        {
            threadInput.clear();
            threadOutput.clear();
            engine.process (threadInput, threadOutput);

            if (! bufferIsFiniteAndWithin (threadOutput, expectedOutputLimit))
                failed.store (true, std::memory_order_release);

            if (! bufferIsSilent (threadOutput))
                audibleBlocks.fetch_add (1, std::memory_order_relaxed);

            std::this_thread::yield();
        }
    });

    for (int i = 0; i < 16 && ! failed.load (std::memory_order_acquire); ++i)
    {
        const auto program = (i % 2) == 0 ? boundProgram() : alternateBoundProgram();
        if (! engine.loadProgramAsync (program, customBindings))
        {
            failed.store (true, std::memory_order_release);
            break;
        }
    }

    if (! engine.waitForAsyncProgramLoads (10000))
        failed.store (true, std::memory_order_release);

    stopRequested.store (true, std::memory_order_release);
    audioThread.join();

    if (failed.load (std::memory_order_acquire)
        || audibleBlocks.load (std::memory_order_relaxed) == 0
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0
        || engine.isAsyncProgramLoadActive())
    {
        juce::Logger::writeToLog ("Async-program test failed: render/async-load race produced invalid state");
        return 119;
    }

    if (engine.getAsyncProgramLoadQueuedCount() < 18
        || engine.getAsyncProgramLoadCompletedCount() == 0
        || engine.getProgramLoadSuccessCount() < 2)
    {
        juce::Logger::writeToLog ("Async-program test failed: async diagnostics are too low");
        return 120;
    }

    juce::Logger::writeToLog ("Async-program test passed: queued="
                              + juce::String (static_cast<juce::int64> (engine.getAsyncProgramLoadQueuedCount()))
                              + " completed="
                              + juce::String (static_cast<juce::int64> (engine.getAsyncProgramLoadCompletedCount()))
                              + " dropped="
                              + juce::String (static_cast<juce::int64> (engine.getAsyncProgramLoadDroppedCount())));
    return 0;
}

int runScoreScriptTest()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr int blockSize = 256;
    EmbeddedChucKEngine engine;
    if (! engine.prepare (48000.0, blockSize, 0, 1))
    {
        juce::Logger::writeToLog ("Score-script test prepare failed: " + engine.getLastError());
        return 210;
    }

    const auto program = ChucKScoreScript::buildProgram (R"chuck(
score.clear();
tempo(120);
meter(4, 4);
phase(0.25);
state.add("Intro", 16, 4);
state.add("Alt", 8, 2);
state.connect(1, 2, 50);
track.add(1, "Lead", "chuck");
track.gain(1, 1, 0.7);
track.mute(1, 1, 0);
track.solo(1, 1, 0);
track.code(1, 1, "base64:U2luT3NjIHMgPT4gZGFjOwo=");
1::bar => now;
track.phase(1, 1, 0.5);
stop();
)chuck");

    if (! engine.loadProgram (program.source, ChucKScoreScript::getParameterBindings()))
    {
        juce::Logger::writeToLog ("Score-script test compile failed: " + engine.getLastError());
        return 211;
    }

    const auto commandIndex = engine.getParameterIndex ("weldScoreCommand");
    const auto frameIndex = engine.getParameterIndex ("weldScoreFrame");
    if (commandIndex < 0 || frameIndex < 0)
    {
        juce::Logger::writeToLog ("Score-script test failed: command/frame bridge missing");
        return 212;
    }

    std::array<int, ChucKScoreScript::argumentCount> argIndices {};
    for (int i = 0; i < ChucKScoreScript::argumentCount; ++i)
    {
        argIndices[static_cast<size_t> (i)] = engine.getParameterIndex ("weldScoreArg" + juce::String (i));
        if (argIndices[static_cast<size_t> (i)] < 0)
        {
            juce::Logger::writeToLog ("Score-script test failed: argument bridge missing");
            return 213;
        }
    }

    juce::AudioBuffer<float> input (0, blockSize);
    juce::AudioBuffer<float> output (1, blockSize);
    std::vector<int> commands;
    std::vector<int> framesByCommand;
    std::vector<std::array<float, ChucKScoreScript::argumentCount>> argsByCommand;
    std::vector<std::array<juce::String, 2>> textByCommand;

    constexpr int maxBlocks = 1200;
    for (int block = 0; block < maxBlocks; ++block)
    {
        output.clear();
        engine.process (input, output);
        engine.pullParameterValuesFromGlobals();

        const auto command = juce::roundToInt (engine.getParameterValue (commandIndex));
        if (command != 0)
        {
            std::array<float, ChucKScoreScript::argumentCount> args {};
            for (int i = 0; i < ChucKScoreScript::argumentCount; ++i)
                args[static_cast<size_t> (i)] = engine.getParameterValue (argIndices[static_cast<size_t> (i)]);

            commands.push_back (command);
            framesByCommand.push_back (juce::roundToInt (engine.getGlobalFloatValue ("weldScoreFrame")));
            argsByCommand.push_back (args);
            textByCommand.push_back ({ engine.getGlobalStringValue ("weldScoreText0"),
                                       engine.getGlobalStringValue ("weldScoreText1") });
            engine.setParameterValue (commandIndex, 0.0f);

            juce::AudioBuffer<float> ackInput (0, 1);
            juce::AudioBuffer<float> ackOutput (1, 1);
            engine.process (ackInput, ackOutput);
            engine.pullParameterValuesFromGlobals();

            if (command == static_cast<int> (ChucKScoreScript::CommandId::scoreComplete))
                break;
        }
    }

    const std::vector<int> expected
    {
        static_cast<int> (ChucKScoreScript::CommandId::scoreClear),
        static_cast<int> (ChucKScoreScript::CommandId::tempo),
        static_cast<int> (ChucKScoreScript::CommandId::meter),
        static_cast<int> (ChucKScoreScript::CommandId::phase),
        static_cast<int> (ChucKScoreScript::CommandId::stateAdd),
        static_cast<int> (ChucKScoreScript::CommandId::stateAdd),
        static_cast<int> (ChucKScoreScript::CommandId::stateConnect),
        static_cast<int> (ChucKScoreScript::CommandId::trackAdd),
        static_cast<int> (ChucKScoreScript::CommandId::trackGain),
        static_cast<int> (ChucKScoreScript::CommandId::trackMute),
        static_cast<int> (ChucKScoreScript::CommandId::trackSolo),
        static_cast<int> (ChucKScoreScript::CommandId::trackCode),
        static_cast<int> (ChucKScoreScript::CommandId::trackPhase),
        static_cast<int> (ChucKScoreScript::CommandId::stop),
        static_cast<int> (ChucKScoreScript::CommandId::scoreComplete)
    };

    if (commands != expected)
    {
        juce::Logger::writeToLog ("Score-script test failed: command sequence length="
                                  + juce::String (static_cast<int> (commands.size())));
        return 214;
    }

    if (textByCommand[4][0] != "Intro"
        || textByCommand[5][0] != "Alt"
        || argsByCommand[6][0] != 1.0f
        || argsByCommand[6][1] != 2.0f
        || ! valuesAreClose (argsByCommand[6][2], 50.0f)
        || argsByCommand[7][0] != 1.0f
        || textByCommand[7][0] != "Lead"
        || textByCommand[7][1] != "chuck"
        || ! valuesAreClose (argsByCommand[8][2], 0.7f)
        || textByCommand[11][0] != "base64:U2luT3NjIHMgPT4gZGFjOwo="
        || ! valuesAreClose (argsByCommand[12][2], 0.5f)
        || framesByCommand[12] != 96000)
    {
        juce::Logger::writeToLog ("Score-script test failed: bridged arguments are wrong; phaseFrame="
                                  + juce::String (framesByCommand[12]));
        return 215;
    }

    juce::Logger::writeToLog ("Score-script test passed: commands="
                              + juce::String (static_cast<int> (commands.size())));
    return 0;
}

struct LanguageExample
{
    EmbeddedLanguageEngine::Language language;
    const char* name;
    const char* sourceUrl;
    const char* source;
};

float renderLanguageExamplePeak (const LanguageExample& example, juce::String& error)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;
    constexpr int renderBlocks = 512;

    EmbeddedLanguageEngine engine (example.language);
    if (! engine.prepare (sampleRate, blockSize, 0, 2))
    {
        error = engine.getLastError();
        return -1.0f;
    }

    auto bindings = EmbeddedPerformanceEngine::withPerformanceParameterBindings ({});
    if (! engine.loadProgram (juce::String (example.source), bindings))
    {
        error = engine.getLastError();
        return -1.0f;
    }

    static_cast<void> (engine.setParameterValue ("hostFreq", 220.0f));
    static_cast<void> (engine.setParameterValue ("hostGain", 0.16f));
    static_cast<void> (engine.setParameterValue ("hostBlend", 0.35f));
    static_cast<void> (engine.setParameterValue ("hostStateGate", 1.0f));
    static_cast<void> (engine.setParameterValue ("hostStateGain", 1.0f));
    static_cast<void> (engine.setParameterValue ("hostTempoBpm", 120.0f));
    static_cast<void> (engine.setParameterValue ("hostTrackGain", 1.0f));

    juce::AudioBuffer<float> input (0, blockSize);
    juce::AudioBuffer<float> output (2, blockSize);
    auto peak = 0.0f;

    for (int block = 0; block < renderBlocks; ++block)
    {
        output.clear();
        engine.process (input, output);

        for (int channel = 0; channel < output.getNumChannels(); ++channel)
            for (int sample = 0; sample < output.getNumSamples(); ++sample)
            {
                const auto value = output.getSample (channel, sample);
                if (! std::isfinite (value))
                {
                    error = "rendered a non-finite sample";
                    return -1.0f;
                }

                peak = juce::jmax (peak, std::abs (value));
            }
    }

    if (engine.getRenderExceptionCount() != 0)
    {
        error = "reported render exceptions";
        return -1.0f;
    }

    error.clear();
    return peak;
}

int runLanguageExampleCompatibilityTest()
{
    juce::ScopedNoDenormals noDenormals;

    const LanguageExample examples[]
    {
        { EmbeddedLanguageEngine::Language::chuck,
          "ChucK official-style SinOsc -> dac",
          "https://chuck.stanford.edu/doc/examples/",
          R"chuck(
SinOsc s => Gain g => dac;
while (true)
{
    hostFreq => s.freq;
    Math.max(0.0, Math.min(hostGain, 0.2)) * hostStateGain * hostTrackGain => g.gain;
    1::samp => now;
}
)chuck" },
        { EmbeddedLanguageEngine::Language::chuck,
          "ChucK SawOsc through LPF",
          "https://chuck.cs.princeton.edu/doc/program/ugen.html",
          R"chuck(
SawOsc saw => LPF filt => Gain g => dac;
while (true)
{
    hostFreq * 0.5 => saw.freq;
    220.0 + (hostFreq * (4.0 + (hostBlend * 8.0))) => filt.freq;
    Math.max(0.0, Math.min(hostGain, 0.16)) * 0.75 * hostStateGain * hostTrackGain => g.gain;
    1::samp => now;
}
)chuck" },
        { EmbeddedLanguageEngine::Language::chuck,
          "ChucK Noise through LPF",
          "https://chuck.cs.princeton.edu/doc/program/ugen.html",
          R"chuck(
Noise n => LPF filt => Gain g => dac;
while (true)
{
    150.0 + (hostFreq * (1.5 + (hostBlend * 6.0))) => filt.freq;
    Math.max(0.0, Math.min(hostGain, 0.12)) * 0.28 * hostStateGain * hostTrackGain => g.gain;
    1::samp => now;
}
)chuck" },
        { EmbeddedLanguageEngine::Language::csound,
          "Csound ftgen/oscili instrument",
          "https://csound.com/docs/manual/oscili.html",
          R"csound(
giSine ftgen 1, 0, 4096, 10, 1

instr 1
    kfreq chnget "hostFreq"
    kgain chnget "hostGain"
    kstate chnget "hostStateGain"
    ktrack chnget "hostTrackGain"
    a1 oscili kgain * 0.7 * kstate * ktrack, kfreq, giSine
    outs a1, a1
endin
)csound" },
        { EmbeddedLanguageEngine::Language::csound,
          "Csound vco2 through tone",
          "https://csound.com/docs/manual/vco2.html",
          R"csound(
instr 1
    kfreq chnget "hostFreq"
    kgain chnget "hostGain"
    kblend chnget "hostBlend"
    kstate chnget "hostStateGain"
    ktrack chnget "hostTrackGain"
    asaw vco2 kgain * 0.32 * kstate * ktrack, kfreq * 0.5, 0
    afilt tone asaw, 220 + (kfreq * (4 + (kblend * 10)))
    outs afilt, afilt
endin
)csound" },
        { EmbeddedLanguageEngine::Language::csound,
          "Csound rand-modulated oscili",
          "https://csound.com/docs/manual/rand.html",
          R"csound(
giSine ftgen 1, 0, 4096, 10, 1

instr 1
    kfreq chnget "hostFreq"
    kgain chnget "hostGain"
    kstate chnget "hostStateGain"
    ktrack chnget "hostTrackGain"
    krnd rand kfreq * 0.08, 0.25
    acar oscili kgain * 0.38 * kstate * ktrack, kfreq + krnd, giSine
    anoise rand kgain * 0.08 * kstate * ktrack, 0.5
    afilt tone anoise, kfreq * 5
    outs acar + afilt, acar - afilt
endin
)csound" },
        { EmbeddedLanguageEngine::Language::faust,
          "Faust stdfaust oscillator",
          "https://faustlibraries.grame.fr/standardFunctions/",
          R"faust(
import("stdfaust.lib");
hostFreq = hslider("hostFreq", 220, 30, 4000, 1);
hostGain = hslider("hostGain", 0.14, 0, 0.4, 0.001);
hostStateGain = hslider("hostStateGain", 1, 0, 1, 0.001);
hostTrackGain = hslider("hostTrackGain", 1, 0, 2, 0.001);
process = os.osc(hostFreq) * hostGain * hostStateGain * hostTrackGain <: _, _;
)faust" },
        { EmbeddedLanguageEngine::Language::faust,
          "Faust sawtooth through lowpass",
          "https://faustlibraries.grame.fr/libs/oscillators/",
          R"faust(
import("stdfaust.lib");
hostFreq = hslider("hostFreq", 220, 30, 4000, 1);
hostGain = hslider("hostGain", 0.14, 0, 0.4, 0.001);
hostBlend = hslider("hostBlend", 0.35, 0, 1, 0.001);
hostStateGain = hslider("hostStateGain", 1, 0, 1, 0.001);
hostTrackGain = hslider("hostTrackGain", 1, 0, 2, 0.001);
cutoff = 220 + (hostFreq * (4 + (hostBlend * 8)));
tone = os.sawtooth(hostFreq * 0.5) : fi.lowpass(3, cutoff);
process = tone * hostGain * 0.42 * hostStateGain * hostTrackGain <: _, _;
)faust" },
        { EmbeddedLanguageEngine::Language::faust,
          "Faust filtered noise",
          "https://faustlibraries.grame.fr/standardFunctions/",
          R"faust(
import("stdfaust.lib");
hostFreq = hslider("hostFreq", 220, 30, 4000, 1);
hostGain = hslider("hostGain", 0.14, 0, 0.4, 0.001);
hostStateGain = hslider("hostStateGain", 1, 0, 1, 0.001);
hostTrackGain = hslider("hostTrackGain", 1, 0, 2, 0.001);
process = (no.noise : fi.lowpass(2, 600 + (hostFreq * 3))) * hostGain * 0.10 * hostStateGain * hostTrackGain <: _, _;
)faust" },
        { EmbeddedLanguageEngine::Language::supercollider,
          "SuperCollider examples-page drummer function",
          "https://supercollider.github.io/examples",
          R"supercollider(
{ |freq = 440, gain = 0.14, blend = 0.5, stateGate = 1, stateGain = 1, tempoBpm = 120,
   stateBeat = 0, globalBeat = 0, timeSigNumerator = 4, timeSigDenominator = 4,
   barBeat = 0, barPhase = 0, phaseRotation = 0, trackGain = 1|
    var snare, bdrum, hihat;
    var tempo = 4;
    tempo = Impulse.ar(tempo);
    snare = WhiteNoise.ar(Decay2.ar(PulseDivider.ar(tempo, 4, 2), 0.005, 0.5));
    bdrum = SinOsc.ar(Line.ar(120, 60, 1), 0, Decay2.ar(PulseDivider.ar(tempo, 4, 0), 0.005, 0.5));
    hihat = HPF.ar(WhiteNoise.ar(1), 10000) * Decay2.ar(tempo, 0.005, 0.5);
    ((snare + bdrum + hihat) * 0.4 * stateGain * trackGain).dup
}
)supercollider" },
        { EmbeddedLanguageEngine::Language::supercollider,
          "SuperCollider simple FM function",
          "https://supercollider.github.io/examples",
          R"supercollider(
{ |freq = 440, gain = 0.14, blend = 0.5, stateGate = 1, stateGain = 1, tempoBpm = 120,
   stateBeat = 0, globalBeat = 0, timeSigNumerator = 4, timeSigDenominator = 4,
   barBeat = 0, barPhase = 0, phaseRotation = 0, trackGain = 1|
    var mod, sig;
    mod = SinOsc.kr([1, 3]).exprange(freq * 0.5, freq * (2 + (blend * 4)));
    sig = SinOsc.ar(mod, 0, gain * 0.42 * stateGain * trackGain);
    sig
}
)supercollider" },
        { EmbeddedLanguageEngine::Language::supercollider,
          "SuperCollider pulse through RLPF",
          "https://docs.supercollider.online/",
          R"supercollider(
{ |freq = 440, gain = 0.14, blend = 0.5, stateGate = 1, stateGain = 1, tempoBpm = 120,
   stateBeat = 0, globalBeat = 0, timeSigNumerator = 4, timeSigDenominator = 4,
   barBeat = 0, barPhase = 0, phaseRotation = 0, trackGain = 1|
    var pulse, cutoff;
    pulse = Pulse.ar(freq * [0.5, 0.505], 0.35);
    cutoff = SinOsc.kr(0.35).range(freq * 2, freq * (6 + (blend * 8)));
    RLPF.ar(pulse, cutoff, 0.25) * gain * 0.32 * stateGain * trackGain
}
)supercollider" },
        { EmbeddedLanguageEngine::Language::rtcmix,
          "RTcmix maketable/WAVETABLE score",
          "https://rtcmix.org/reference/instruments/WAVETABLE.html",
          R"rtcmix(
bus_config("WAVETABLE", "out 0-1")
freq = makeconnection("inlet", 1, 220)
gain = makeconnection("inlet", 2, 0.08)
stategain = makeconnection("inlet", 5, 1.0)
trackgain = makeconnection("inlet", 14, 1.0)
env = maketable("line", 1000, 0,0, 0.01,1, 1,1)
wave = maketable("wave", 4000, 1, 0.5, 0.25)
WAVETABLE(0, 4.0, gain * stategain * trackgain * 32767.0 * env, freq, 0.5, wave)
)rtcmix" },
        { EmbeddedLanguageEngine::Language::rtcmix,
          "RTcmix FMINST score",
          "https://rtcmix.org/reference/instruments/FMINST.html",
          R"rtcmix(
bus_config("FMINST", "out 0-1")
freq = makeconnection("inlet", 1, 220)
gain = makeconnection("inlet", 2, 0.08)
stategain = makeconnection("inlet", 5, 1.0)
trackgain = makeconnection("inlet", 14, 1.0)
env = maketable("line", 1000, 0,0.4, 0.01,1, 4,0.9)
guide = maketable("line", "nonorm", 1000, 0,0, 1,1, 4,0.25)
wave = maketable("wave", 1000, "sine")
FMINST(0, 4.0, gain * stategain * trackgain * 32767.0 * env, freq, freq * 1.5, 0, 5, 0.5, wave, guide)
)rtcmix" },
        { EmbeddedLanguageEngine::Language::rtcmix,
          "RTcmix AMINST score",
          "https://rtcmix.org/reference/instruments/AMINST.html",
          R"rtcmix(
bus_config("AMINST", "out 0-1")
freq = makeconnection("inlet", 1, 220)
gain = makeconnection("inlet", 2, 0.08)
stategain = makeconnection("inlet", 5, 1.0)
trackgain = makeconnection("inlet", 14, 1.0)
env = maketable("line", 1000, 0,0.5, 0.01,1, 4,0.8)
modenv = maketable("line", 1000, 0,0.2, 0.4,1, 4,0.4)
carwave = maketable("wave", 1000, 1, 0.45, 0.2)
modwave = maketable("wave", 1000, 1)
AMINST(0, 4.0, gain * stategain * trackgain * 32767.0 * env, 220, freq * 2.0, 0.5, modenv, carwave, modwave)
)rtcmix" }
    };

    auto tested = 0;
    for (const auto& example : examples)
    {
        const auto languageName = EmbeddedLanguageEngine::getLanguageName (example.language);
        if (! EmbeddedLanguageEngine::isLanguageBuiltIn (example.language))
        {
            juce::Logger::writeToLog ("Language-example test skipped "
                                      + languageName
                                      + ": "
                                      + EmbeddedLanguageEngine::getLanguageBuildStatus (example.language));
            continue;
        }

        juce::String error;
        const auto peak = renderLanguageExamplePeak (example, error);
        if (peak < 0.001f)
        {
            juce::Logger::writeToLog ("Language-example test failed for "
                                      + languageName
                                      + " ("
                                      + example.name
                                      + ", source "
                                      + example.sourceUrl
                                      + "): peak="
                                      + juce::String (peak)
                                      + " error="
                                      + error);
            return 230 + tested;
        }

        ++tested;
        juce::Logger::writeToLog ("Language-example test passed for "
                                  + languageName
                                  + " ("
                                  + example.name
                                  + ", source "
                                  + example.sourceUrl
                                  + "): peak="
                                  + juce::String (peak));
    }

    if (tested == 0)
    {
        juce::Logger::writeToLog ("Language-example test failed: no built-in language backends were available");
        return 239;
    }

    juce::Logger::writeToLog ("Language-example test passed: tested=" + juce::String (tested));
    return 0;
}

int runBoundaryTest()
{
    juce::ScopedNoDenormals noDenormals;

    if (expectPrepareFailure (48000.0,
                              EmbeddedChucKEngine::maximumBlockSizeLimit + 1,
                              2,
                              2,
                              "above maximum block size") != 0)
        return 60;

    if (expectPrepareFailure (48000.0,
                              64,
                              EmbeddedChucKEngine::maximumChannelLimit + 1,
                              2,
                              "above maximum input channels") != 0)
        return 61;

    if (expectPrepareFailure (48000.0,
                              64,
                              2,
                              EmbeddedChucKEngine::maximumChannelLimit + 1,
                              "above maximum output channels") != 0)
        return 62;

    EmbeddedChucKEngine maxBlockEngine;
    if (! maxBlockEngine.prepare (48000.0, EmbeddedChucKEngine::maximumBlockSizeLimit, 0, 2))
    {
        juce::Logger::writeToLog ("Boundary-test max-block prepare failed: " + maxBlockEngine.getLastError());
        return 63;
    }

    maxBlockEngine.setFrequency (-1.0f);
    maxBlockEngine.setGain (1.0f);
    maxBlockEngine.setToneBlend (std::numeric_limits<float>::quiet_NaN());

    if (maxBlockEngine.getSanitisedControlCount() < 3)
    {
        juce::Logger::writeToLog ("Boundary-test failed: control sanitising was not counted");
        return 64;
    }

    juce::AudioBuffer<float> noInput (0, EmbeddedChucKEngine::maximumBlockSizeLimit);
    juce::AudioBuffer<float> maxBlockOutput (2, EmbeddedChucKEngine::maximumBlockSizeLimit);
    maxBlockEngine.process (noInput, maxBlockOutput);

    if (bufferEnergy (maxBlockOutput) <= 0.0
        || ! bufferIsFiniteAndWithin (maxBlockOutput, 0.981f)
        || maxBlockEngine.getRenderedBlockCount() != 1
        || maxBlockEngine.getRenderedFrameCount() != static_cast<uint64_t> (EmbeddedChucKEngine::maximumBlockSizeLimit))
    {
        juce::Logger::writeToLog ("Boundary-test failed: maximum block render was invalid");
        return 65;
    }

    EmbeddedChucKEngine maxChannelEngine;
    constexpr int smallBlock = 32;
    if (! maxChannelEngine.prepare (44100.0,
                                    smallBlock,
                                    EmbeddedChucKEngine::maximumChannelLimit,
                                    EmbeddedChucKEngine::maximumChannelLimit))
    {
        juce::Logger::writeToLog ("Boundary-test max-channel prepare failed: " + maxChannelEngine.getLastError());
        return 66;
    }

    juce::AudioBuffer<float> maxChannelInput (EmbeddedChucKEngine::maximumChannelLimit, smallBlock);
    juce::AudioBuffer<float> maxChannelOutput (EmbeddedChucKEngine::maximumChannelLimit, smallBlock);
    maxChannelInput.clear();
    maxChannelInput.setSample (0, 0, std::numeric_limits<float>::infinity());
    maxChannelInput.setSample (EmbeddedChucKEngine::maximumChannelLimit - 1, smallBlock - 1, -42.0f);
    maxChannelEngine.process (maxChannelInput, maxChannelOutput);

    if (! bufferIsFiniteAndWithin (maxChannelOutput, 0.981f)
        || maxChannelEngine.getSanitisedSampleCount() < 2
        || maxChannelEngine.getRenderedBlockCount() != 1
        || maxChannelEngine.getRenderedFrameCount() != smallBlock
        || maxChannelEngine.getInternalErrorCount() != 0
        || maxChannelEngine.getRenderExceptionCount() != 0)
    {
        juce::Logger::writeToLog ("Boundary-test failed: maximum channel render was invalid");
        return 67;
    }

    juce::Logger::writeToLog ("Boundary-test passed");
    return 0;
}

int runConcurrencyTest()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr int blockSize = 256;
    constexpr int prepareIterations = 48;
    constexpr float expectedOutputLimit = 0.981f;

    EmbeddedChucKEngine engine;
    std::atomic<bool> stopRequested { false };
    std::atomic<bool> failed { false };
    std::atomic<int> silentBlocks { 0 };
    std::atomic<int> audibleBlocks { 0 };

    std::thread audioThread ([&]
    {
        juce::ScopedNoDenormals threadNoDenormals;
        juce::AudioBuffer<float> input (2, blockSize);
        juce::AudioBuffer<float> output (2, blockSize);

        while (! stopRequested.load (std::memory_order_acquire))
        {
            input.clear();
            output.clear();
            output.setSample (0, 0, 1.0f);

            engine.process (input, output);

            if (! bufferIsFiniteAndWithin (output, expectedOutputLimit))
                failed.store (true, std::memory_order_release);

            if (bufferIsSilent (output))
                silentBlocks.fetch_add (1, std::memory_order_relaxed);
            else
                audibleBlocks.fetch_add (1, std::memory_order_relaxed);

            std::this_thread::yield();
        }
    });

    for (int i = 0; i < prepareIterations && ! failed.load (std::memory_order_acquire); ++i)
    {
        if (! engine.prepare (44100.0 + static_cast<double> (i % 2) * 3900.0, blockSize, 2, 2))
        {
            juce::Logger::writeToLog ("Concurrency-test prepare failed: " + engine.getLastError());
            failed.store (true, std::memory_order_release);
            break;
        }

        if (i % 7 == 0)
        {
            engine.setFrequency (std::numeric_limits<float>::infinity());
            engine.setGain (std::numeric_limits<float>::quiet_NaN());
            engine.setToneBlend (2.0f);
        }
        else
        {
            engine.setFrequency (110.0f + static_cast<float> (i * 13));
            engine.setGain (0.05f + static_cast<float> (i % 5) * 0.04f);
            engine.setToneBlend (static_cast<float> (i % 9) / 8.0f);
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (1));
        engine.release();
        std::this_thread::yield();
    }

    stopRequested.store (true, std::memory_order_release);
    audioThread.join();
    engine.release();

    if (failed.load (std::memory_order_acquire))
    {
        juce::Logger::writeToLog ("Concurrency-test failed: render/prepare contention produced invalid output");
        return 50;
    }

    if (audibleBlocks.load (std::memory_order_relaxed) == 0)
    {
        juce::Logger::writeToLog ("Concurrency-test failed: no audible render occurred during contention test");
        return 51;
    }

    juce::Logger::writeToLog ("Concurrency-test passed: audibleBlocks="
                              + juce::String (audibleBlocks.load (std::memory_order_relaxed))
                              + " silentBlocks="
                              + juce::String (silentBlocks.load (std::memory_order_relaxed)));
    return 0;
}

int runCallbackTest()
{
    juce::ScopedNoDenormals noDenormals;

    ChucKAudioCallback callback;
    callback.audioDeviceAboutToStart (nullptr);

    if (! callback.deviceReady.load (std::memory_order_acquire) || ! callback.engine.isReady())
    {
        juce::Logger::writeToLog ("Callback-test failed: callback did not prepare with fallback device info");
        return 30;
    }

    juce::AudioBuffer<float> output (2, 128);
    output.clear();

    callback.audioDeviceIOCallbackWithContext (nullptr,
                                               0,
                                               output.getArrayOfWritePointers(),
                                               output.getNumChannels(),
                                               output.getNumSamples(),
                                               {});

    if (bufferEnergy (output) <= 0.0)
    {
        juce::Logger::writeToLog ("Callback-test failed: null-input callback rendered silence");
        return 31;
    }

    if (callback.renderedCallbackCount.load (std::memory_order_relaxed) == 0)
    {
        juce::Logger::writeToLog ("Callback-test failed: rendered callback was not counted");
        return 32;
    }

    const auto rejectedCallbacksBeforeNullOutput = callback.rejectedCallbackCount.load (std::memory_order_relaxed);
    const auto nullOutputsBefore = callback.nullOutputCallbackCount.load (std::memory_order_relaxed);
    callback.audioDeviceIOCallbackWithContext (nullptr, 0, nullptr, 2, 128, {});

    if (callback.rejectedCallbackCount.load (std::memory_order_relaxed) == rejectedCallbacksBeforeNullOutput
        || callback.nullOutputCallbackCount.load (std::memory_order_relaxed) == nullOutputsBefore)
    {
        juce::Logger::writeToLog ("Callback-test failed: null output callback was not counted");
        return 33;
    }

    juce::AudioBuffer<float> callbackInput (2, 128);
    callbackInput.clear();
    callbackInput.setSample (0, 0, std::numeric_limits<float>::infinity());
    const float* callbackInputPointers[] =
    {
        callbackInput.getReadPointer (0),
        callbackInput.getReadPointer (1)
    };

    const auto sanitisedBefore = callback.engine.getSanitisedSampleCount();
    output.clear();
    callback.audioDeviceIOCallbackWithContext (callbackInputPointers,
                                               2,
                                               output.getArrayOfWritePointers(),
                                               output.getNumChannels(),
                                               output.getNumSamples(),
                                               {});

    if (callback.engine.getSanitisedSampleCount() == sanitisedBefore)
    {
        juce::Logger::writeToLog ("Callback-test failed: callback input sanitising was not counted");
        return 34;
    }

    juce::AudioBuffer<float> oversizedOutput (2, ChucKAudioCallback::maximumHostBlockSize + 1);
    oversizedOutput.clear();
    oversizedOutput.setSample (0, 0, 1.0f);

    callback.audioDeviceIOCallbackWithContext (nullptr,
                                               0,
                                               oversizedOutput.getArrayOfWritePointers(),
                                               oversizedOutput.getNumChannels(),
                                               oversizedOutput.getNumSamples(),
                                               {});

    if (! bufferIsSilent (oversizedOutput))
    {
        juce::Logger::writeToLog ("Callback-test failed: oversized callback did not clear outputs");
        return 35;
    }

    constexpr int tooManyOutputChannels = ChucKAudioCallback::maxHostChannels + 1;
    juce::AudioBuffer<float> tooWideOutput (tooManyOutputChannels, 128);
    std::array<float*, tooManyOutputChannels> tooWidePointers {};

    for (int channel = 0; channel < tooWideOutput.getNumChannels(); ++channel)
    {
        tooWideOutput.setSample (channel, 0, 1.0f);
        tooWidePointers[static_cast<size_t> (channel)] = tooWideOutput.getWritePointer (channel);
    }

    callback.audioDeviceIOCallbackWithContext (nullptr,
                                               0,
                                               tooWidePointers.data(),
                                               tooManyOutputChannels,
                                               tooWideOutput.getNumSamples(),
                                               {});

    if (! bufferIsSilent (tooWideOutput))
    {
        juce::Logger::writeToLog ("Callback-test failed: unsupported channel count did not clear bounded outputs");
        return 36;
    }

    juce::AudioBuffer<float> mixedNullOutput (2, 128);
    mixedNullOutput.clear();
    std::array<float*, 2> mixedNullPointers { mixedNullOutput.getWritePointer (0), nullptr };

    callback.audioDeviceIOCallbackWithContext (nullptr,
                                               0,
                                               mixedNullPointers.data(),
                                               static_cast<int> (mixedNullPointers.size()),
                                               mixedNullOutput.getNumSamples(),
                                               {});

    if (bufferEnergy (mixedNullOutput) <= 0.0)
    {
        juce::Logger::writeToLog ("Callback-test failed: mixed null output channel render failed");
        return 37;
    }

    ChucKAudioCallback reconfiguredCallback;
    if (! reconfiguredCallback.prepareForDevice (48000.0, 128))
    {
        juce::Logger::writeToLog ("Callback-test failed: explicit prepare did not recover");
        return 38;
    }

    if (reconfiguredCallback.prepareForDevice (0.0, 128)
        || reconfiguredCallback.deviceReady.load (std::memory_order_acquire)
        || reconfiguredCallback.engine.isReady()
        || reconfiguredCallback.scratchInput.getNumSamples() != 0
        || reconfiguredCallback.scratchOutput.getNumSamples() != 0
        || reconfiguredCallback.rejectedPrepareCount.load (std::memory_order_relaxed) == 0)
    {
        juce::Logger::writeToLog ("Callback-test failed: invalid sample rate did not fully reset");
        return 39;
    }

    output.clear();
    output.setSample (0, 0, 1.0f);
    reconfiguredCallback.audioDeviceIOCallbackWithContext (nullptr,
                                                           0,
                                                           output.getArrayOfWritePointers(),
                                                           output.getNumChannels(),
                                                           output.getNumSamples(),
                                                           {});

    if (! bufferIsSilent (output))
    {
        juce::Logger::writeToLog ("Callback-test failed: invalidly prepared callback did not fail silent");
        return 40;
    }

    if (reconfiguredCallback.prepareForDevice (48000.0, ChucKAudioCallback::maximumHostBlockSize + 1)
        || reconfiguredCallback.deviceReady.load (std::memory_order_acquire)
        || reconfiguredCallback.engine.isReady()
        || reconfiguredCallback.scratchInput.getNumSamples() != 0
        || reconfiguredCallback.scratchOutput.getNumSamples() != 0)
    {
        juce::Logger::writeToLog ("Callback-test failed: unsupported block size did not fully reset");
        return 41;
    }

    if (! reconfiguredCallback.prepareForDevice (48000.0, 64))
    {
        juce::Logger::writeToLog ("Callback-test failed: callback did not recover after rejected device info");
        return 42;
    }

    reconfiguredCallback.audioDeviceStopped();

    callback.audioDeviceStopped();

    output.clear();
    output.setSample (0, 0, 1.0f);
    callback.audioDeviceIOCallbackWithContext (nullptr,
                                               0,
                                               output.getArrayOfWritePointers(),
                                               output.getNumChannels(),
                                               output.getNumSamples(),
                                               {});

    if (! bufferIsSilent (output))
    {
        juce::Logger::writeToLog ("Callback-test failed: stopped callback did not fail silent");
        return 43;
    }

    if (callback.rejectedCallbackCount.load (std::memory_order_relaxed) < 3)
    {
        juce::Logger::writeToLog ("Callback-test failed: rejected callback diagnostics are too low");
        return 44;
    }

    if (callback.callbackExceptionCount.load (std::memory_order_relaxed) != 0
        || reconfiguredCallback.callbackExceptionCount.load (std::memory_order_relaxed) != 0)
    {
        juce::Logger::writeToLog ("Callback-test failed: callback exception containment fired unexpectedly");
        return 45;
    }

    juce::Logger::writeToLog ("Callback-test passed");
    return 0;
}

int runPerformanceDemo (bool quickDemo)
{
    PerformanceAudioCallback callback (quickDemo);
    juce::AudioDeviceManager deviceManager;

    const auto error = deviceManager.initialiseWithDefaultDevices (0, 2);
    if (error.isNotEmpty())
    {
        juce::Logger::writeToLog ("Audio device error: " + error);
        return 1;
    }

    deviceManager.addAudioCallback (&callback);

    for (int attempt = 0; attempt < 100
         && ! callback.deviceReady.load (std::memory_order_acquire)
         && callback.rejectedPrepareCount.load (std::memory_order_relaxed) == 0;
         ++attempt)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    if (! callback.deviceReady.load (std::memory_order_acquire))
    {
        deviceManager.removeAudioCallback (&callback);
        juce::Logger::writeToLog ("Performance demo could not start: " + callback.performance.getLastError());
        return 1;
    }

    juce::Logger::writeToLog (quickDemo
                                ? "Playing quick performance demo: ChucK -> SuperCollider -> RTcmix."
                                : "Playing performance demo: 30s ChucK -> 20s SuperCollider -> 25s RTcmix, with tails.");

    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds (quickDemo ? 35 : 120);
    while (! callback.hasFinished() && std::chrono::steady_clock::now() < timeout)
        std::this_thread::sleep_for (std::chrono::milliseconds (100));

    deviceManager.removeAudioCallback (&callback);

    juce::Logger::writeToLog ("Performance demo diagnostics: renderedCallbacks="
                              + juce::String (static_cast<juce::int64> (callback.renderedCallbackCount.load()))
                              + " rejectedCallbacks="
                              + juce::String (static_cast<juce::int64> (callback.rejectedCallbackCount.load()))
                              + " callbackExceptions="
                              + juce::String (static_cast<juce::int64> (callback.callbackExceptionCount.load()))
                              + " renderedFrames="
                              + juce::String (static_cast<juce::int64> (callback.performance.getRenderedFrameCount()))
                              + " activeStates="
                              + juce::String (callback.performance.getActiveStateCount()));

    return callback.callbackExceptionCount.load (std::memory_order_relaxed) == 0 ? 0 : 1;
}

bool hasArgument (int argc, char* argv[], const char* argument)
{
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == argument)
            return true;

    return false;
}
}

int main (int argc, char* argv[])
{
    if (hasArgument (argc, argv, "--self-test"))
        return runSelfTest();

    if (hasArgument (argc, argv, "--stress-test"))
        return runStressTest();

    if (hasArgument (argc, argv, "--callback-test"))
        return runCallbackTest();

    if (hasArgument (argc, argv, "--fuzz-test"))
        return runFuzzTest();

    if (hasArgument (argc, argv, "--program-test"))
        return runProgramTest();

    if (hasArgument (argc, argv, "--parameter-test"))
        return runParameterBindingTest();

    if (hasArgument (argc, argv, "--async-program-test"))
        return runAsyncProgramLoadTest();

    if (hasArgument (argc, argv, "--score-script-test"))
        return runScoreScriptTest();

    if (hasArgument (argc, argv, "--language-example-test"))
        return runLanguageExampleCompatibilityTest();

    if (hasArgument (argc, argv, "--boundary-test"))
        return runBoundaryTest();

    if (hasArgument (argc, argv, "--concurrency-test"))
        return runConcurrencyTest();

    if (hasArgument (argc, argv, "--performance-demo"))
        return runPerformanceDemo (false);

    if (hasArgument (argc, argv, "--quick-performance-demo"))
        return runPerformanceDemo (true);

    ChucKAudioCallback callback;
    juce::AudioDeviceManager deviceManager;

    const auto error = deviceManager.initialiseWithDefaultDevices (2, 2);
    if (error.isNotEmpty())
    {
        juce::Logger::writeToLog ("Audio device error: " + error);
        return 1;
    }

    deviceManager.addAudioCallback (&callback);

    juce::Logger::writeToLog ("Alchemy is running with ChucK embedded in the JUCE audio callback.");
    juce::Logger::writeToLog ("Press return to quit.");
    std::cin.get();

    deviceManager.removeAudioCallback (&callback);

    juce::Logger::writeToLog ("Diagnostics: silent="
                              + juce::String (static_cast<juce::int64> (callback.engine.getSilentProcessCount()))
                              + " oversized="
                              + juce::String (static_cast<juce::int64> (callback.engine.getOversizedBlockCount()))
                              + " renderExceptions="
                              + juce::String (static_cast<juce::int64> (callback.engine.getRenderExceptionCount()))
                              + " sanitisedSamples="
                              + juce::String (static_cast<juce::int64> (callback.engine.getSanitisedSampleCount()))
                              + " sanitisedControls="
                              + juce::String (static_cast<juce::int64> (callback.engine.getSanitisedControlCount()))
                              + " internalErrors="
                              + juce::String (static_cast<juce::int64> (callback.engine.getInternalErrorCount()))
                              + " renderedCallbacks="
                              + juce::String (static_cast<juce::int64> (callback.renderedCallbackCount.load()))
                              + " rejectedCallbacks="
                              + juce::String (static_cast<juce::int64> (callback.rejectedCallbackCount.load()))
                              + " rejectedPrepares="
                              + juce::String (static_cast<juce::int64> (callback.rejectedPrepareCount.load()))
                              + " callbackExceptions="
                              + juce::String (static_cast<juce::int64> (callback.callbackExceptionCount.load()))
                              + " programSuccesses="
                              + juce::String (static_cast<juce::int64> (callback.engine.getProgramLoadSuccessCount()))
                              + " programFailures="
                              + juce::String (static_cast<juce::int64> (callback.engine.getProgramLoadFailureCount()))
                              + " parameters="
                              + juce::String (callback.engine.getParameterCount()));
    return 0;
}
