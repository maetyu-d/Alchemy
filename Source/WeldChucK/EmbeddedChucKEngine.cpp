#include "EmbeddedChucKEngine.h"

#include "chuck.h"
#include "chuck_globals.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <chrono>
#include <utility>

namespace
{
const char* defaultProgramBody = R"chuck(
SinOsc sine => Gain sineGain => dac;
SawOsc saw => LPF lowpass => Gain sawGain => dac;

1.0 => sine.gain;
1.0 => saw.gain;
900.0 => lowpass.freq;
0.7 => lowpass.Q;

while (true)
{
    Math.max(30.0, hostFreq) => float f;
    Math.max(0.0, Math.min(hostGain, 0.4)) => float g;
    Math.max(0.0, Math.min(hostBlend, 1.0)) => float b;

    f => sine.freq;
    f * 0.5 => saw.freq;
    250.0 + (f * 3.0) => lowpass.freq;

    g * (1.0 - b) => sineGain.gain;
    g * b * 0.55 => sawGain.gain;

    5::ms => now;
}
)chuck";

struct CandidateProgram
{
    std::unique_ptr<ChucK> chuck;
    std::array<double*, EmbeddedChucKEngine::maximumParameterCount> parameterPointers {};
};

juce::String buildCompleteProgram (const juce::String& programBody,
                                   const std::vector<EmbeddedChucKEngine::ParameterBinding>& bindings)
{
    juce::String completeProgram;

    for (const auto& binding : bindings)
        completeProgram << "global float " << binding.name << ";\n";

    completeProgram << "\n" << programBody;
    return completeProgram;
}

void destroyChucKInstance (std::unique_ptr<ChucK> instance) noexcept
{
    if (instance == nullptr)
        return;

    try
    {
        instance->removeAllShreds();
    }
    catch (...)
    {
    }

    try
    {
        instance.reset();
    }
    catch (...)
    {
        static_cast<void> (instance.release());
    }
}

bool initialiseCandidate (CandidateProgram& candidate,
                          double sampleRate,
                          int inputChannels,
                          int outputChannels,
                          const juce::String& programBody,
                          const std::vector<EmbeddedChucKEngine::ParameterBinding>& bindings,
                          juce::String& error)
{
    candidate.chuck.reset();
    candidate.parameterPointers.fill (nullptr);
    candidate.chuck = std::make_unique<ChucK>();
    candidate.chuck->setParam (CHUCK_PARAM_SAMPLE_RATE, static_cast<t_CKINT> (sampleRate));
    candidate.chuck->setParam (CHUCK_PARAM_INPUT_CHANNELS, static_cast<t_CKINT> (inputChannels));
    candidate.chuck->setParam (CHUCK_PARAM_OUTPUT_CHANNELS, static_cast<t_CKINT> (outputChannels));
    candidate.chuck->setParam (CHUCK_PARAM_VM_HALT, FALSE);
    candidate.chuck->setParam (CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, TRUE);
    candidate.chuck->setParam (CHUCK_PARAM_CHUGIN_ENABLE, FALSE);

    if (! candidate.chuck->init())
    {
        error = "ChucK failed to initialise";
        return false;
    }

    if (! candidate.chuck->start())
    {
        error = "ChucK failed to start";
        return false;
    }

    if (! candidate.chuck->compileCode (buildCompleteProgram (programBody, bindings).toStdString(), "", 1, TRUE))
    {
        error = "ChucK program did not compile";
        return false;
    }

    auto* globals = candidate.chuck->globals();
    if (globals == nullptr)
    {
        error = "ChucK globals are unavailable";
        return false;
    }

    for (size_t i = 0; i < bindings.size(); ++i)
    {
        globals->init_global_float (bindings[i].name.toStdString());
        candidate.parameterPointers[i] = globals->get_ptr_to_global_float (bindings[i].name.toStdString());

        if (candidate.parameterPointers[i] == nullptr)
        {
            error = "ChucK host global could not be bound: " + bindings[i].name;
            return false;
        }
    }

    return true;
}
}

EmbeddedChucKEngine::EmbeddedChucKEngine()
{
    programLoaderThread = std::thread ([this] { programLoaderLoop(); });
}

EmbeddedChucKEngine::~EmbeddedChucKEngine()
{
    stopAsyncProgramLoader();
    release();
}

bool EmbeddedChucKEngine::prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels)
{
    const juce::ScopedLock lock (engineLock);

    try
    {
        releaseUnlocked();
        resetDiagnostics();

        if (sampleRate <= 0.0 || ! std::isfinite (sampleRate))
        {
            lastError = "Invalid audio sample rate";
            return false;
        }

        if (maximumBlockSize <= 0 || maximumBlockSize > maximumBlockSizeLimit)
        {
            lastError = "Unsupported audio block size";
            return false;
        }

        if (inputChannels < 0 || inputChannels > maximumChannelLimit
            || outputChannels <= 0 || outputChannels > maximumChannelLimit)
        {
            lastError = "Unsupported audio channel count";
            return false;
        }

        numInputChannels = inputChannels;
        numOutputChannels = outputChannels;
        maxBlockSize = maximumBlockSize;
        currentSampleRate = sampleRate;

        interleavedInput.assign (static_cast<size_t> (maxBlockSize) * static_cast<size_t> (numInputChannels), 0.0f);
        interleavedOutput.assign (static_cast<size_t> (maxBlockSize) * static_cast<size_t> (numOutputChannels), 0.0f);

        CandidateProgram candidate;
        juce::String candidateError;
        const auto programBody = getDefaultProgram();
        const auto bindings = getDefaultParameterBindings();
        if (! initialiseCandidate (candidate,
                                   currentSampleRate,
                                   numInputChannels,
                                   numOutputChannels,
                                   programBody,
                                   bindings,
                                   candidateError))
        {
            lastError = candidateError;
            destroyChucKInstance (std::move (candidate.chuck));
            releaseUnlocked();
            return false;
        }

        chuck = std::move (candidate.chuck);
        std::vector<float> values;
        values.reserve (bindings.size());
        for (const auto& binding : bindings)
            values.push_back (binding.defaultValue);

        applyParameterSlots (bindings, values, candidate.parameterPointers);
        currentProgram = programBody;
        ++engineGeneration;
        programLoadSuccessCount.fetch_add (1, std::memory_order_relaxed);
        pushGlobals();
        ready.store (true, std::memory_order_release);
        lastError.clear();
        return true;
    }
    catch (const std::exception& e)
    {
        lastError = juce::String ("ChucK prepare exception: ") + e.what();
        releaseUnlocked();
        return false;
    }
    catch (...)
    {
        lastError = "Unknown ChucK prepare exception";
        releaseUnlocked();
        return false;
    }
}

void EmbeddedChucKEngine::release() noexcept
{
    const juce::ScopedLock lock (engineLock);
    releaseUnlocked();
}

bool EmbeddedChucKEngine::loadProgram (const juce::String& programBody)
{
    return loadProgram (programBody, getCurrentParameterBindings());
}

bool EmbeddedChucKEngine::loadProgramAsync (const juce::String& programBody)
{
    return loadProgramAsync (programBody, getCurrentParameterBindings());
}

bool EmbeddedChucKEngine::loadProgramAsync (const juce::String& programBody,
                                            const std::vector<ParameterBinding>& bindings)
{
    std::vector<ParameterBinding> normalisedBindings = bindings;
    juce::String validationError;

    if (! validateParameterBindings (normalisedBindings, validationError))
    {
        const juce::ScopedLock lock (engineLock);
        lastError = validationError;
        programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
        return false;
    }

    {
        const juce::ScopedLock lock (engineLock);

        if (! ready.load (std::memory_order_acquire)
            || currentSampleRate <= 0.0
            || maxBlockSize <= 0
            || numOutputChannels <= 0)
        {
            lastError = "Cannot queue a ChucK program before the engine is prepared";
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
    }

    bool loaderWasStopped = false;

    {
        std::lock_guard<std::mutex> lock (programQueueMutex);

        if (stopProgramLoader)
            loaderWasStopped = true;
        else
        {
            if (pendingProgramLoad.has_value())
                asyncProgramLoadDroppedCount.fetch_add (1, std::memory_order_relaxed);

            pendingProgramLoad = AsyncProgramLoadRequest { ++nextAsyncProgramLoadId,
                                                           programBody,
                                                           std::move (normalisedBindings) };
            asyncProgramLoadActive.store (true, std::memory_order_release);
            asyncProgramLoadQueuedCount.fetch_add (1, std::memory_order_relaxed);
        }
    }

    if (loaderWasStopped)
    {
        const juce::ScopedLock engineScope (engineLock);
        lastError = "Cannot queue a ChucK program after the loader has stopped";
        programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
        return false;
    }

    programQueueCondition.notify_one();
    return true;
}

bool EmbeddedChucKEngine::waitForAsyncProgramLoads (int timeoutMilliseconds)
{
    std::unique_lock<std::mutex> lock (programQueueMutex);
    const auto isIdle = [this]
    {
        return ! pendingProgramLoad.has_value() && ! programLoaderBusy;
    };

    if (timeoutMilliseconds < 0)
    {
        programQueueIdleCondition.wait (lock, isIdle);
        return true;
    }

    return programQueueIdleCondition.wait_for (lock,
                                               std::chrono::milliseconds (timeoutMilliseconds),
                                               isIdle);
}

bool EmbeddedChucKEngine::loadProgram (const juce::String& programBody,
                                       const std::vector<ParameterBinding>& bindings)
{
    double sampleRate = 0.0;
    int inputChannels = 0;
    int outputChannels = 0;
    uint64_t generation = 0;
    std::vector<ParameterBinding> normalisedBindings = bindings;
    std::vector<float> values;
    juce::String validationError;

    if (! validateParameterBindings (normalisedBindings, validationError))
    {
        const juce::ScopedLock lock (engineLock);
        lastError = validationError;
        programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
        return false;
    }

    {
        const juce::ScopedLock lock (engineLock);

        if (! ready.load (std::memory_order_acquire)
            || currentSampleRate <= 0.0
            || maxBlockSize <= 0
            || numOutputChannels <= 0)
        {
            lastError = "Cannot load a ChucK program before the engine is prepared";
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        sampleRate = currentSampleRate;
        inputChannels = numInputChannels;
        outputChannels = numOutputChannels;
        generation = engineGeneration;

        values.reserve (normalisedBindings.size());
        for (const auto& binding : normalisedBindings)
        {
            const auto existingIndex = getParameterIndexUnlocked (binding.name);
            const auto value = existingIndex >= 0
                                 ? parameterSlots[static_cast<size_t> (existingIndex)].value.load (std::memory_order_relaxed)
                                 : binding.defaultValue;

            values.push_back (sanitiseControlValue (value,
                                                    binding.defaultValue,
                                                    binding.minimumValue,
                                                    binding.maximumValue));
        }
    }

    CandidateProgram candidate;
    juce::String candidateError;

    try
    {
        if (! initialiseCandidate (candidate,
                                  sampleRate,
                                  inputChannels,
                                  outputChannels,
                                  programBody,
                                  normalisedBindings,
                                  candidateError))
        {
            destroyChucKInstance (std::move (candidate.chuck));

            const juce::ScopedLock lock (engineLock);
            lastError = candidateError;
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
    }
    catch (const std::exception& e)
    {
        destroyChucKInstance (std::move (candidate.chuck));

        const juce::ScopedLock lock (engineLock);
        lastError = juce::String ("ChucK program load exception: ") + e.what();
        programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
        return false;
    }
    catch (...)
    {
        destroyChucKInstance (std::move (candidate.chuck));

        const juce::ScopedLock lock (engineLock);
        lastError = "Unknown ChucK program load exception";
        programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
        return false;
    }

    std::unique_ptr<ChucK> oldChuck;
    bool committed = false;

    {
        const juce::ScopedLock lock (engineLock);

        if (! ready.load (std::memory_order_acquire)
            || engineGeneration != generation
            || numInputChannels != inputChannels
            || numOutputChannels != outputChannels)
        {
            lastError = "ChucK engine changed before the program transaction could be committed";
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            oldChuck = std::move (candidate.chuck);
        }
        else
        {
            oldChuck = std::move (chuck);
            chuck = std::move (candidate.chuck);
            applyParameterSlots (normalisedBindings, values, candidate.parameterPointers);
            currentProgram = programBody;
            ++engineGeneration;
            programLoadSuccessCount.fetch_add (1, std::memory_order_relaxed);
            ready.store (true, std::memory_order_release);
            pushGlobals();
            lastError.clear();
            committed = true;
        }
    }

    destroyChucKInstance (std::move (oldChuck));
    return committed;
}

void EmbeddedChucKEngine::releaseUnlocked() noexcept
{
    ready.store (false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> queueLock (programQueueMutex);
        if (pendingProgramLoad.has_value())
        {
            pendingProgramLoad.reset();
            asyncProgramLoadDroppedCount.fetch_add (1, std::memory_order_relaxed);
        }

        asyncProgramLoadActive.store (programLoaderBusy, std::memory_order_release);
    }

    programQueueIdleCondition.notify_all();

    destroyChucKInstance (std::move (chuck));

    interleavedInput.clear();
    interleavedOutput.clear();
    clearParameterSlots();
    currentProgram.clear();
    currentSampleRate = 0.0;
    numInputChannels = 0;
    numOutputChannels = 0;
    maxBlockSize = 0;
    ++engineGeneration;
}

void EmbeddedChucKEngine::resetDiagnostics() noexcept
{
    silentProcessCount.store (0, std::memory_order_relaxed);
    oversizedBlockCount.store (0, std::memory_order_relaxed);
    renderExceptionCount.store (0, std::memory_order_relaxed);
    sanitisedSampleCount.store (0, std::memory_order_relaxed);
    sanitisedControlCount.store (0, std::memory_order_relaxed);
    internalErrorCount.store (0, std::memory_order_relaxed);
    renderedBlockCount.store (0, std::memory_order_relaxed);
    renderedFrameCount.store (0, std::memory_order_relaxed);
    programLoadSuccessCount.store (0, std::memory_order_relaxed);
    programLoadFailureCount.store (0, std::memory_order_relaxed);
    asyncProgramLoadQueuedCount.store (0, std::memory_order_relaxed);
    asyncProgramLoadCompletedCount.store (0, std::memory_order_relaxed);
    asyncProgramLoadDroppedCount.store (0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock (programQueueMutex);
        asyncProgramLoadActive.store (pendingProgramLoad.has_value() || programLoaderBusy,
                                      std::memory_order_release);
    }
}

void EmbeddedChucKEngine::stopAsyncProgramLoader() noexcept
{
    {
        std::lock_guard<std::mutex> lock (programQueueMutex);
        stopProgramLoader = true;

        if (pendingProgramLoad.has_value())
        {
            pendingProgramLoad.reset();
            asyncProgramLoadDroppedCount.fetch_add (1, std::memory_order_relaxed);
        }

        asyncProgramLoadActive.store (programLoaderBusy, std::memory_order_release);
    }

    programQueueCondition.notify_all();
    programQueueIdleCondition.notify_all();

    if (programLoaderThread.joinable())
        programLoaderThread.join();

    asyncProgramLoadActive.store (false, std::memory_order_release);
}

void EmbeddedChucKEngine::programLoaderLoop() noexcept
{
    for (;;)
    {
        AsyncProgramLoadRequest request;

        {
            std::unique_lock<std::mutex> lock (programQueueMutex);
            programQueueCondition.wait (lock, [this]
            {
                return stopProgramLoader || pendingProgramLoad.has_value();
            });

            if (stopProgramLoader && ! pendingProgramLoad.has_value())
                break;

            request = std::move (*pendingProgramLoad);
            pendingProgramLoad.reset();
            programLoaderBusy = true;
            asyncProgramLoadActive.store (true, std::memory_order_release);
        }

        static_cast<void> (loadProgram (request.programBody, request.bindings));

        {
            std::lock_guard<std::mutex> lock (programQueueMutex);
            programLoaderBusy = false;
            asyncProgramLoadCompletedCount.fetch_add (1, std::memory_order_relaxed);
            asyncProgramLoadActive.store (pendingProgramLoad.has_value(), std::memory_order_release);
        }

        programQueueIdleCondition.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock (programQueueMutex);
        programLoaderBusy = false;
        asyncProgramLoadActive.store (pendingProgramLoad.has_value(), std::memory_order_release);
    }

    programQueueIdleCondition.notify_all();
}

void EmbeddedChucKEngine::process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output)
{
    const juce::ScopedTryLock lock (engineLock);
    output.clear();

    if (! lock.isLocked() || ! ready.load (std::memory_order_acquire) || chuck == nullptr)
    {
        silentProcessCount.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    const auto frames = output.getNumSamples();

    if (frames <= 0)
        return;

    if (frames > maxBlockSize || output.getNumChannels() <= 0)
    {
        oversizedBlockCount.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    if (! preparedStateIsValidFor (frames))
    {
        internalErrorCount.fetch_add (1, std::memory_order_relaxed);
        ready.store (false, std::memory_order_release);
        return;
    }

    const auto inputSamples = static_cast<size_t> (frames) * static_cast<size_t> (numInputChannels);
    const auto outputSamples = static_cast<size_t> (frames) * static_cast<size_t> (numOutputChannels);

    std::fill (interleavedInput.begin(), interleavedInput.begin() + static_cast<std::ptrdiff_t> (inputSamples), 0.0f);
    std::fill (interleavedOutput.begin(), interleavedOutput.begin() + static_cast<std::ptrdiff_t> (outputSamples), 0.0f);
    pushGlobals();

    uint64_t sanitisedInBlock = 0;

    const auto inputFrames = juce::jmin (frames, input.getNumSamples());

    if (input.getNumChannels() > 0 && inputFrames > 0 && numInputChannels > 0)
    {
        for (int frame = 0; frame < inputFrames; ++frame)
            for (int channel = 0; channel < numInputChannels; ++channel)
            {
                const auto sample = input.getSample (juce::jmin (channel, input.getNumChannels() - 1), frame);
                if (audioSampleNeedsSanitising (sample))
                    ++sanitisedInBlock;

                interleavedInput[static_cast<size_t> (frame * numInputChannels + channel)] = sanitiseAudioSample (sample);
            }
    }

    try
    {
        chuck->run (interleavedInput.data(), interleavedOutput.data(), frames);
    }
    catch (...)
    {
        renderExceptionCount.fetch_add (1, std::memory_order_relaxed);
        ready.store (false, std::memory_order_release);
        output.clear();
        return;
    }

    for (int channel = 0; channel < output.getNumChannels(); ++channel)
    {
        auto* dst = output.getWritePointer (channel);
        const auto sourceChannel = juce::jmin (channel, numOutputChannels - 1);

        for (int frame = 0; frame < frames; ++frame)
        {
            const auto sample = interleavedOutput[static_cast<size_t> (frame * numOutputChannels + sourceChannel)];
            if (audioSampleNeedsSanitising (sample))
                ++sanitisedInBlock;

            dst[frame] = sanitiseAudioSample (sample);
        }
    }

    if (sanitisedInBlock != 0)
        sanitisedSampleCount.fetch_add (sanitisedInBlock, std::memory_order_relaxed);

    renderedBlockCount.fetch_add (1, std::memory_order_relaxed);
    renderedFrameCount.fetch_add (static_cast<uint64_t> (frames), std::memory_order_relaxed);
}

juce::String EmbeddedChucKEngine::getLastError() const
{
    const juce::ScopedLock lock (engineLock);
    return lastError;
}

juce::String EmbeddedChucKEngine::getDefaultProgram()
{
    return juce::String (defaultProgramBody);
}

std::vector<EmbeddedChucKEngine::ParameterBinding> EmbeddedChucKEngine::getDefaultParameterBindings()
{
    return
    {
        { "hostFreq", 220.0f, 30.0f, 4000.0f },
        { "hostGain", 0.14f, 0.0f, 0.4f },
        { "hostBlend", 0.25f, 0.0f, 1.0f }
    };
}

juce::String EmbeddedChucKEngine::getCurrentProgram() const
{
    const juce::ScopedLock lock (engineLock);
    return currentProgram;
}

std::vector<EmbeddedChucKEngine::ParameterBinding> EmbeddedChucKEngine::getCurrentParameterBindings() const
{
    const juce::ScopedLock lock (engineLock);
    std::vector<ParameterBinding> bindings;
    const auto count = activeParameterCount.load (std::memory_order_relaxed);
    bindings.reserve (static_cast<size_t> (count));

    for (int i = 0; i < count; ++i)
    {
        const auto& slot = parameterSlots[static_cast<size_t> (i)];
        bindings.push_back ({ slot.name,
                              slot.defaultValue.load (std::memory_order_relaxed),
                              slot.minimumValue.load (std::memory_order_relaxed),
                              slot.maximumValue.load (std::memory_order_relaxed) });
    }

    return bindings;
}

bool EmbeddedChucKEngine::setParameterValue (int index, float value) noexcept
{
    const auto count = activeParameterCount.load (std::memory_order_acquire);
    if (index < 0 || index >= count || index >= maximumParameterCount)
        return false;

    auto& slot = parameterSlots[static_cast<size_t> (index)];
    const auto minimum = slot.minimumValue.load (std::memory_order_relaxed);
    const auto maximum = slot.maximumValue.load (std::memory_order_relaxed);
    const auto fallback = slot.defaultValue.load (std::memory_order_relaxed);

    if (controlValueNeedsSanitising (value, minimum, maximum))
        sanitisedControlCount.fetch_add (1, std::memory_order_relaxed);

    slot.value.store (sanitiseControlValue (value, fallback, minimum, maximum), std::memory_order_relaxed);
    return true;
}

bool EmbeddedChucKEngine::setParameterValue (const juce::String& name, float value)
{
    const juce::ScopedLock lock (engineLock);
    return setParameterValue (getParameterIndexUnlocked (name), value);
}

float EmbeddedChucKEngine::getParameterValue (int index) const noexcept
{
    const auto count = activeParameterCount.load (std::memory_order_acquire);
    if (index < 0 || index >= count || index >= maximumParameterCount)
        return 0.0f;

    return parameterSlots[static_cast<size_t> (index)].value.load (std::memory_order_relaxed);
}

void EmbeddedChucKEngine::pullParameterValuesFromGlobals() noexcept
{
    const juce::ScopedLock lock (engineLock);
    const auto count = activeParameterCount.load (std::memory_order_acquire);

    for (int i = 0; i < count && i < maximumParameterCount; ++i)
    {
        auto& slot = parameterSlots[static_cast<size_t> (i)];
        if (slot.globalPointer != nullptr)
            slot.value.store (static_cast<float> (*slot.globalPointer), std::memory_order_relaxed);
    }
}

double EmbeddedChucKEngine::getGlobalFloatValue (const juce::String& name) const
{
    const juce::ScopedLock lock (engineLock);

    if (! ready.load (std::memory_order_acquire) || chuck == nullptr || name.isEmpty())
        return 0.0;

    auto* globals = chuck->globals();
    if (globals == nullptr)
        return 0.0;

    auto* value = globals->get_ptr_to_global_float (name.toStdString());
    return value != nullptr ? *value : 0.0;
}

juce::String EmbeddedChucKEngine::getGlobalStringValue (const juce::String& name) const
{
    const juce::ScopedLock lock (engineLock);

    if (! ready.load (std::memory_order_acquire) || chuck == nullptr || name.isEmpty())
        return {};

    auto* globals = chuck->globals();
    if (globals == nullptr)
        return {};

    auto* value = globals->get_global_string (name.toStdString());
    return value != nullptr ? juce::String (value->c_str()) : juce::String();
}

int EmbeddedChucKEngine::getParameterIndex (const juce::String& name) const
{
    const juce::ScopedLock lock (engineLock);
    return getParameterIndexUnlocked (name);
}

void EmbeddedChucKEngine::setFrequency (float value)
{
    static_cast<void> (setParameterValue ("hostFreq", value));
}

void EmbeddedChucKEngine::setGain (float value)
{
    static_cast<void> (setParameterValue ("hostGain", value));
}

void EmbeddedChucKEngine::setToneBlend (float value)
{
    static_cast<void> (setParameterValue ("hostBlend", value));
}

void EmbeddedChucKEngine::pushGlobals()
{
    const auto count = activeParameterCount.load (std::memory_order_relaxed);

    for (int i = 0; i < count; ++i)
    {
        auto& slot = parameterSlots[static_cast<size_t> (i)];
        if (slot.globalPointer != nullptr)
            *slot.globalPointer = slot.value.load (std::memory_order_relaxed);
    }
}

bool EmbeddedChucKEngine::preparedStateIsValidFor (int frames) const noexcept
{
    if (frames < 0
        || maxBlockSize <= 0
        || frames > maxBlockSize
        || numInputChannels < 0
        || numInputChannels > maximumChannelLimit
        || numOutputChannels <= 0
        || numOutputChannels > maximumChannelLimit)
        return false;

    const auto count = activeParameterCount.load (std::memory_order_relaxed);
    if (count < 0 || count > maximumParameterCount)
        return false;

    for (int i = 0; i < count; ++i)
        if (! parameterSlots[static_cast<size_t> (i)].active
            || parameterSlots[static_cast<size_t> (i)].globalPointer == nullptr)
            return false;

    const auto neededInputSamples = static_cast<size_t> (frames) * static_cast<size_t> (numInputChannels);
    const auto neededOutputSamples = static_cast<size_t> (frames) * static_cast<size_t> (numOutputChannels);
    return neededInputSamples <= interleavedInput.size()
           && neededOutputSamples <= interleavedOutput.size();
}

void EmbeddedChucKEngine::applyParameterSlots (const std::vector<ParameterBinding>& bindings,
                                               const std::vector<float>& values,
                                               const std::array<double*, maximumParameterCount>& globalPointers)
{
    const auto count = juce::jmin (static_cast<int> (bindings.size()), maximumParameterCount);
    const auto oldCount = activeParameterCount.load (std::memory_order_relaxed);

    if (count < oldCount)
        activeParameterCount.store (count, std::memory_order_release);

    for (int i = 0; i < count; ++i)
    {
        const auto index = static_cast<size_t> (i);
        auto& slot = parameterSlots[index];
        const auto& binding = bindings[index];
        const auto value = index < values.size() ? values[index] : binding.defaultValue;

        slot.name = binding.name;
        slot.defaultValue.store (binding.defaultValue, std::memory_order_relaxed);
        slot.minimumValue.store (binding.minimumValue, std::memory_order_relaxed);
        slot.maximumValue.store (binding.maximumValue, std::memory_order_relaxed);
        slot.value.store (sanitiseControlValue (value,
                                                binding.defaultValue,
                                                binding.minimumValue,
                                                binding.maximumValue),
                          std::memory_order_relaxed);
        slot.globalPointer = globalPointers[index];
        slot.active = true;
    }

    activeParameterCount.store (count, std::memory_order_release);

    for (int i = count; i < maximumParameterCount; ++i)
    {
        auto& slot = parameterSlots[static_cast<size_t> (i)];
        slot.name.clear();
        slot.value.store (0.0f, std::memory_order_relaxed);
        slot.defaultValue.store (0.0f, std::memory_order_relaxed);
        slot.minimumValue.store (0.0f, std::memory_order_relaxed);
        slot.maximumValue.store (1.0f, std::memory_order_relaxed);
        slot.globalPointer = nullptr;
        slot.active = false;
    }
}

void EmbeddedChucKEngine::clearParameterSlots() noexcept
{
    activeParameterCount.store (0, std::memory_order_release);

    for (auto& slot : parameterSlots)
    {
        slot.name.clear();
        slot.value.store (0.0f, std::memory_order_relaxed);
        slot.defaultValue.store (0.0f, std::memory_order_relaxed);
        slot.minimumValue.store (0.0f, std::memory_order_relaxed);
        slot.maximumValue.store (1.0f, std::memory_order_relaxed);
        slot.globalPointer = nullptr;
        slot.active = false;
    }
}

int EmbeddedChucKEngine::getParameterIndexUnlocked (const juce::String& name) const
{
    const auto count = activeParameterCount.load (std::memory_order_relaxed);
    for (int i = 0; i < count; ++i)
        if (parameterSlots[static_cast<size_t> (i)].name == name)
            return i;

    return -1;
}

bool EmbeddedChucKEngine::validateParameterBindings (const std::vector<ParameterBinding>& bindings, juce::String& error)
{
    if (bindings.size() > static_cast<size_t> (maximumParameterCount))
    {
        error = "Too many ChucK parameter bindings";
        return false;
    }

    for (size_t i = 0; i < bindings.size(); ++i)
    {
        const auto& binding = bindings[i];

        if (! isValidParameterName (binding.name))
        {
            error = "Invalid ChucK parameter binding name: " + binding.name;
            return false;
        }

        if (! std::isfinite (binding.minimumValue)
            || ! std::isfinite (binding.maximumValue)
            || ! std::isfinite (binding.defaultValue)
            || binding.minimumValue > binding.maximumValue
            || binding.defaultValue < binding.minimumValue
            || binding.defaultValue > binding.maximumValue)
        {
            error = "Invalid range/default for ChucK parameter binding: " + binding.name;
            return false;
        }

        for (size_t other = i + 1; other < bindings.size(); ++other)
            if (bindings[other].name == binding.name)
            {
                error = "Duplicate ChucK parameter binding name: " + binding.name;
                return false;
            }
    }

    error.clear();
    return true;
}

bool EmbeddedChucKEngine::isValidParameterName (const juce::String& name) noexcept
{
    if (name.isEmpty())
        return false;

    const auto first = static_cast<unsigned char> (name[0]);
    if (! (std::isalpha (first) || first == '_'))
        return false;

    for (int i = 1; i < name.length(); ++i)
    {
        const auto character = static_cast<unsigned char> (name[i]);
        if (! (std::isalnum (character) || character == '_'))
            return false;
    }

    return true;
}

bool EmbeddedChucKEngine::audioSampleNeedsSanitising (float sample) noexcept
{
    return ! std::isfinite (sample) || sample < -outputSafetyLimit || sample > outputSafetyLimit;
}

bool EmbeddedChucKEngine::controlValueNeedsSanitising (float value, float lower, float upper) noexcept
{
    return ! std::isfinite (value) || value < lower || value > upper;
}

float EmbeddedChucKEngine::sanitiseAudioSample (float sample) noexcept
{
    if (! std::isfinite (sample))
        return 0.0f;

    return juce::jlimit (-outputSafetyLimit, outputSafetyLimit, sample);
}

float EmbeddedChucKEngine::sanitiseControlValue (float value, float fallback, float lower, float upper) noexcept
{
    if (! std::isfinite (value))
        return fallback;

    return juce::jlimit (lower, upper, value);
}
