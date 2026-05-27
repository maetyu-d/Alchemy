#include "EmbeddedPerformanceEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr float defaultTempoBpm = 120.0f;
constexpr float maximumTempoBpm = 999.0f;
constexpr float maximumBeatValue = 1000000.0f;
constexpr float outputSafetyLimit = 0.98f;
constexpr double stateFadeInSeconds = 0.010;

bool hasBindingNamed (const std::vector<EmbeddedPerformanceEngine::ParameterBinding>& bindings,
                      const juce::String& name)
{
    return std::any_of (bindings.begin(), bindings.end(), [&name] (const auto& binding)
    {
        return binding.name == name;
    });
}

void addBindingIfMissing (std::vector<EmbeddedPerformanceEngine::ParameterBinding>& bindings,
                          const juce::String& name,
                          float defaultValue,
                          float minimumValue,
                          float maximumValue)
{
    if (! hasBindingNamed (bindings, name)
        && bindings.size() < static_cast<size_t> (EmbeddedChucKEngine::maximumParameterCount))
    {
        bindings.push_back ({ name, defaultValue, minimumValue, maximumValue });
    }
}
}

EmbeddedPerformanceEngine::~EmbeddedPerformanceEngine()
{
    release();
}

bool EmbeddedPerformanceEngine::prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels)
{
    const juce::ScopedLock lock (engineLock);

    releaseSequenceUnlocked();
    resetDiagnostics();

    if (sampleRate <= 0.0 || ! std::isfinite (sampleRate))
    {
        lastError = "Invalid performance sample rate";
        return false;
    }

    if (maximumBlockSize <= 0 || maximumBlockSize > EmbeddedChucKEngine::maximumBlockSizeLimit)
    {
        lastError = "Unsupported performance block size";
        return false;
    }

    if (inputChannels < 0
        || inputChannels > EmbeddedChucKEngine::maximumChannelLimit
        || outputChannels <= 0
        || outputChannels > EmbeddedChucKEngine::maximumChannelLimit)
    {
        lastError = "Unsupported performance channel count";
        return false;
    }

    currentSampleRate = sampleRate;
    maxBlockSize = maximumBlockSize;
    numInputChannels = inputChannels;
    numOutputChannels = outputChannels;
    scratchInput.setSize (numInputChannels, maxBlockSize);
    scratchOutput.setSize (numOutputChannels, maxBlockSize);
    scratchInputPointers.resize (static_cast<size_t> (numInputChannels));
    scratchOutputPointers.resize (static_cast<size_t> (numOutputChannels));

    if (! requestedStates.empty() && ! prepareSequenceUnlocked())
        return false;

    ready.store (! stateRuntimes.empty(), std::memory_order_release);
    lastError.clear();
    return true;
}

void EmbeddedPerformanceEngine::release() noexcept
{
    const juce::ScopedLock lock (engineLock);
    releaseSequenceUnlocked();
    requestedStates.clear();
    scratchInput.setSize (0, 0);
    scratchOutput.setSize (0, 0);
    scratchInputPointers.clear();
    scratchOutputPointers.clear();
    currentSampleRate = 0.0;
    maxBlockSize = 0;
    numInputChannels = 0;
    numOutputChannels = 0;
    lastError.clear();
}

bool EmbeddedPerformanceEngine::loadSequence (const std::vector<State>& states)
{
    const juce::ScopedLock lock (engineLock);

    releaseSequenceUnlocked();
    requestedStates = states;

    if (requestedStates.empty())
    {
        lastError = "Performance sequence has no states";
        return false;
    }

    auto rtcmixStateCount = 0;
    for (const auto& state : requestedStates)
    {
        if (! isFinitePositive (state.durationBeats))
        {
            lastError = "Performance state duration must be a positive beat value";
            requestedStates.clear();
            return false;
        }

        if (state.tailBeats < 0.0 || ! std::isfinite (state.tailBeats))
        {
            lastError = "Performance state tail must be a finite non-negative beat value";
            requestedStates.clear();
            return false;
        }

        for (const auto& track : normaliseTracksForState (state))
        {
            if (track.language == Language::rtcmix)
                ++rtcmixStateCount;

            if (track.gain < 0.0f || ! std::isfinite (track.gain))
            {
                lastError = "Performance track gain must be finite and non-negative";
                requestedStates.clear();
                return false;
            }

            for (const auto& event : track.gainEvents)
            {
                if (event.beat < 0.0 || ! std::isfinite (event.beat)
                    || event.gain < 0.0f || ! std::isfinite (event.gain))
                {
                    lastError = "Performance track gain events need finite non-negative beats and gains";
                    requestedStates.clear();
                    return false;
                }
            }

            if (! track.tightlySynced)
            {
                if (! isFinitePositive (track.tempoBpm))
                {
                    lastError = "Performance track tempo must be a positive BPM value";
                    requestedStates.clear();
                    return false;
                }

                if (track.timeSignatureNumerator <= 0 || track.timeSignatureDenominator <= 0)
                {
                    lastError = "Performance track time signature must use positive values";
                    requestedStates.clear();
                    return false;
                }

                if (! std::isfinite (track.phaseRotationBeats))
                {
                    lastError = "Performance track phase must be finite";
                    requestedStates.clear();
                    return false;
                }
            }
        }
    }

    if (rtcmixStateCount > 1)
    {
        lastError = "Stock RTcmix exposes one embedded runtime, so one sequence may prepare only one RTcmix track";
        requestedStates.clear();
        return false;
    }

    if (currentSampleRate > 0.0 && maxBlockSize > 0 && numOutputChannels > 0)
    {
        if (! prepareSequenceUnlocked())
            return false;
    }

    ready.store (! stateRuntimes.empty(), std::memory_order_release);
    lastError.clear();
    return true;
}

bool EmbeddedPerformanceEngine::start()
{
    const juce::ScopedLock lock (engineLock);

    if (stateRuntimes.empty())
    {
        lastError = "Cannot start a performance sequence before it is loaded and prepared";
        return false;
    }

    playheadFrame = 0;
    playing.store (true, std::memory_order_release);
    currentStateIndex.store (currentStateIndexForFrameUnlocked (playheadFrame), std::memory_order_release);
    activeStateCount.store (activeStateCountForFrameUnlocked (playheadFrame), std::memory_order_release);
    lastError.clear();
    return true;
}

void EmbeddedPerformanceEngine::stop() noexcept
{
    const juce::ScopedLock lock (engineLock);
    playing.store (false, std::memory_order_release);
    currentStateIndex.store (-1, std::memory_order_release);
    activeStateCount.store (0, std::memory_order_release);
}

void EmbeddedPerformanceEngine::resetToStart() noexcept
{
    const juce::ScopedLock lock (engineLock);
    playheadFrame = 0;
    currentStateIndex.store (stateRuntimes.empty() ? -1 : 0, std::memory_order_release);
    activeStateCount.store (stateRuntimes.empty() ? 0 : activeStateCountForFrameUnlocked (0), std::memory_order_release);
}

void EmbeddedPerformanceEngine::process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output)
{
    const juce::ScopedTryLock lock (engineLock);
    output.clear();

    if (! lock.isLocked() || ! playing.load (std::memory_order_acquire) || stateRuntimes.empty())
    {
        silentProcessCount.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    const auto frames = output.getNumSamples();
    if (frames <= 0)
        return;

    if (! validatePreparedRenderStateFor (frames))
    {
        oversizedBlockCount.fetch_add (frames > maxBlockSize ? 1 : 0, std::memory_order_relaxed);
        renderExceptionCount.fetch_add (frames <= maxBlockSize ? 1 : 0, std::memory_order_relaxed);
        playing.store (false, std::memory_order_release);
        output.clear();
        return;
    }

    int offset = 0;

    try
    {
        while (offset < frames && playing.load (std::memory_order_relaxed))
        {
            if (playheadFrame >= sequenceEndFrame)
            {
                playing.store (false, std::memory_order_release);
                currentStateIndex.store (-1, std::memory_order_release);
                activeStateCount.store (0, std::memory_order_release);
                break;
            }

            const auto nextBoundary = nextBoundaryAfterUnlocked (playheadFrame);
            const auto frameLimit = nextBoundary > playheadFrame
                                      ? juce::jmin<int64_t> (playheadFrame + frames - offset, nextBoundary)
                                      : playheadFrame + frames - offset;
            const auto chunkFrames = static_cast<int> (juce::jmax<int64_t> (1, frameLimit - playheadFrame));

            renderChunkUnlocked (input, output, offset, chunkFrames, playheadFrame);

            playheadFrame += chunkFrames;
            offset += chunkFrames;
            renderedFrameCount.fetch_add (static_cast<uint64_t> (chunkFrames), std::memory_order_relaxed);
            currentStateIndex.store (currentStateIndexForFrameUnlocked (playheadFrame), std::memory_order_release);
            activeStateCount.store (activeStateCountForFrameUnlocked (playheadFrame), std::memory_order_release);
        }
    }
    catch (...)
    {
        renderExceptionCount.fetch_add (1, std::memory_order_relaxed);
        output.clear();
        playing.store (false, std::memory_order_release);
        currentStateIndex.store (-1, std::memory_order_release);
        activeStateCount.store (0, std::memory_order_release);
    }
}

bool EmbeddedPerformanceEngine::setTempoBpm (double bpm)
{
    const juce::ScopedLock lock (engineLock);
    return setTempoMapUnlocked ({ { 0.0, bpm } });
}

bool EmbeddedPerformanceEngine::setTempoMap (const std::vector<TempoEvent>& events)
{
    const juce::ScopedLock lock (engineLock);
    return setTempoMapUnlocked (events);
}

bool EmbeddedPerformanceEngine::setTimeSignatureMap (const std::vector<TimeSignatureEvent>& events)
{
    const juce::ScopedLock lock (engineLock);
    return setTimeSignatureMapUnlocked (events);
}

bool EmbeddedPerformanceEngine::setPhaseRotationMap (const std::vector<PhaseRotationEvent>& events)
{
    const juce::ScopedLock lock (engineLock);
    return setPhaseRotationMapUnlocked (events);
}

bool EmbeddedPerformanceEngine::setTempoMapUnlocked (std::vector<TempoEvent> events)
{
    if (events.empty())
        events.push_back ({ 0.0, defaultTempoBpm });

    for (const auto& event : events)
    {
        if (event.beat < 0.0 || ! std::isfinite (event.beat)
            || event.bpm <= 0.0 || event.bpm > static_cast<double> (maximumTempoBpm)
            || ! std::isfinite (event.bpm))
        {
            lastError = "Performance tempo events need finite non-negative beats and BPM between 0 and 999";
            return false;
        }
    }

    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b)
    {
        return a.beat < b.beat;
    });

    std::vector<TempoEvent> normalised;
    normalised.reserve (events.size() + 1);

    if (events.front().beat > 0.0)
        normalised.push_back ({ 0.0, events.front().bpm });

    for (const auto& event : events)
    {
        if (! normalised.empty() && std::abs (normalised.back().beat - event.beat) < 1.0e-9)
            normalised.back() = event;
        else
            normalised.push_back (event);
    }

    tempoMap = std::move (normalised);
    tempoBpm.store (tempoMap.front().bpm, std::memory_order_release);

    if (! stateRuntimes.empty() && ! rebuildTimelineUnlocked())
        return false;

    lastError.clear();
    return true;
}

bool EmbeddedPerformanceEngine::setTimeSignatureMapUnlocked (std::vector<TimeSignatureEvent> events)
{
    if (events.empty())
        events.push_back ({ 0.0, 4, 4 });

    for (const auto& event : events)
    {
        if (event.beat < 0.0 || ! std::isfinite (event.beat)
            || event.numerator <= 0 || event.numerator > 32
            || event.denominator <= 0 || event.denominator > 64)
        {
            lastError = "Performance time signatures need finite non-negative beats and practical positive meters";
            return false;
        }
    }

    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b)
    {
        return a.beat < b.beat;
    });

    std::vector<TimeSignatureEvent> normalised;
    normalised.reserve (events.size() + 1);

    if (events.front().beat > 0.0)
        normalised.push_back ({ 0.0, events.front().numerator, events.front().denominator });

    for (const auto& event : events)
    {
        if (! normalised.empty() && std::abs (normalised.back().beat - event.beat) < 1.0e-9)
            normalised.back() = event;
        else
            normalised.push_back (event);
    }

    timeSignatureMap = std::move (normalised);
    lastError.clear();
    return true;
}

bool EmbeddedPerformanceEngine::setPhaseRotationMapUnlocked (std::vector<PhaseRotationEvent> events)
{
    if (events.empty())
        events.push_back ({ 0.0, 0.0 });

    for (const auto& event : events)
    {
        if (event.beat < 0.0 || ! std::isfinite (event.beat) || ! std::isfinite (event.rotationBeats))
        {
            lastError = "Performance phase rotations need finite non-negative beats and finite rotation values";
            return false;
        }
    }

    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b)
    {
        return a.beat < b.beat;
    });

    std::vector<PhaseRotationEvent> normalised;
    normalised.reserve (events.size() + 1);

    if (events.front().beat > 0.0)
        normalised.push_back ({ 0.0, 0.0 });

    for (const auto& event : events)
    {
        if (! normalised.empty() && std::abs (normalised.back().beat - event.beat) < 1.0e-9)
            normalised.back() = event;
        else
            normalised.push_back (event);
    }

    phaseRotationMap = std::move (normalised);
    lastError.clear();
    return true;
}

bool EmbeddedPerformanceEngine::setStateParameterValue (int stateIndex, const juce::String& name, float value)
{
    const juce::ScopedLock lock (engineLock);

    if (stateIndex < 0 || stateIndex >= static_cast<int> (stateRuntimes.size()))
    {
        lastError = "Performance state index is out of range";
        return false;
    }

    auto& runtime = stateRuntimes[static_cast<size_t> (stateIndex)];
    auto changed = false;

    for (auto& track : runtime.tracks)
        if (track.engine != nullptr && track.engine->setParameterValue (name, value))
            changed = true;

    if (! changed)
    {
        lastError = "Performance state parameter could not be set: " + name;
        return false;
    }

    lastError.clear();
    return true;
}

bool EmbeddedPerformanceEngine::setTrackParameterValue (int stateIndex, int trackIndex, const juce::String& name, float value)
{
    const juce::ScopedLock lock (engineLock);

    if (stateIndex < 0 || stateIndex >= static_cast<int> (stateRuntimes.size()))
    {
        lastError = "Performance state index is out of range";
        return false;
    }

    auto& state = stateRuntimes[static_cast<size_t> (stateIndex)];
    if (trackIndex < 0 || trackIndex >= static_cast<int> (state.tracks.size()))
    {
        lastError = "Performance track index is out of range";
        return false;
    }

    auto& track = state.tracks[static_cast<size_t> (trackIndex)];
    if (track.engine == nullptr || ! track.engine->setParameterValue (name, value))
    {
        lastError = "Performance track parameter could not be set: " + name;
        return false;
    }

    lastError.clear();
    return true;
}

bool EmbeddedPerformanceEngine::setTrackGain (int stateIndex, int trackIndex, float gain)
{
    const juce::ScopedLock lock (engineLock);

    if (gain < 0.0f || ! std::isfinite (gain))
    {
        lastError = "Performance track gain must be finite and non-negative";
        return false;
    }

    if (stateIndex < 0 || stateIndex >= static_cast<int> (requestedStates.size()))
    {
        lastError = "Performance state index is out of range";
        return false;
    }

    auto& requestedState = requestedStates[static_cast<size_t> (stateIndex)];
    if (trackIndex < 0 || trackIndex >= static_cast<int> (requestedState.tracks.size()))
    {
        lastError = "Performance track index is out of range";
        return false;
    }

    const auto limitedGain = juce::jlimit (0.0f, 4.0f, gain);
        requestedState.tracks[static_cast<size_t> (trackIndex)].gain = limitedGain;
        requestedState.tracks[static_cast<size_t> (trackIndex)].gainEvents.clear();

        if (stateIndex < static_cast<int> (stateRuntimes.size()))
        {
            auto& state = stateRuntimes[static_cast<size_t> (stateIndex)];
            if (trackIndex < static_cast<int> (state.tracks.size()))
            {
                state.tracks[static_cast<size_t> (trackIndex)].definition.gain = limitedGain;
                state.tracks[static_cast<size_t> (trackIndex)].definition.gainEvents.clear();
            }
        }

    lastError.clear();
    return true;
}

bool EmbeddedPerformanceEngine::setParameterValueForAllStates (const juce::String& name, float value)
{
    const juce::ScopedLock lock (engineLock);
    auto changed = false;

    for (auto& runtime : stateRuntimes)
        for (auto& track : runtime.tracks)
            if (track.engine != nullptr && track.engine->setParameterValue (name, value))
                changed = true;

    if (! changed)
    {
        lastError = "Performance parameter was not found in any state: " + name;
        return false;
    }

    lastError.clear();
    return true;
}

juce::String EmbeddedPerformanceEngine::getLastError() const
{
    const juce::ScopedLock lock (engineLock);
    return lastError;
}

double EmbeddedPerformanceEngine::secondsToBeats (double seconds, double bpm) noexcept
{
    if (seconds <= 0.0 || bpm <= 0.0 || ! std::isfinite (seconds) || ! std::isfinite (bpm))
        return 0.0;

    return seconds * bpm / 60.0;
}

double EmbeddedPerformanceEngine::beatsToSeconds (double beats, double bpm) noexcept
{
    if (beats <= 0.0 || bpm <= 0.0 || ! std::isfinite (beats) || ! std::isfinite (bpm))
        return 0.0;

    return beats * 60.0 / bpm;
}

std::vector<EmbeddedPerformanceEngine::ParameterBinding>
EmbeddedPerformanceEngine::withPerformanceParameterBindings (std::vector<ParameterBinding> bindings)
{
    if (bindings.empty())
        bindings = EmbeddedChucKEngine::getDefaultParameterBindings();

    addBindingIfMissing (bindings, "hostStateGate", 0.0f, 0.0f, 1.0f);
    addBindingIfMissing (bindings, "hostStateGain", 1.0f, 0.0f, 1.0f);
    addBindingIfMissing (bindings, "hostTempoBpm", defaultTempoBpm, 1.0f, maximumTempoBpm);
    addBindingIfMissing (bindings, "hostStateBeat", 0.0f, 0.0f, maximumBeatValue);
    addBindingIfMissing (bindings, "hostGlobalBeat", 0.0f, 0.0f, maximumBeatValue);
    addBindingIfMissing (bindings, "hostTimeSigNumerator", 4.0f, 1.0f, 32.0f);
    addBindingIfMissing (bindings, "hostTimeSigDenominator", 4.0f, 1.0f, 64.0f);
    addBindingIfMissing (bindings, "hostBarBeat", 0.0f, 0.0f, maximumBeatValue);
    addBindingIfMissing (bindings, "hostBarPhase", 0.0f, 0.0f, 1.0f);
    addBindingIfMissing (bindings, "hostPhaseRotation", 0.0f, -maximumBeatValue, maximumBeatValue);
    addBindingIfMissing (bindings, "hostTrackGain", 1.0f, 0.0f, 4.0f);
    addBindingIfMissing (bindings, "hostTrackTempoBpm", defaultTempoBpm, 1.0f, maximumTempoBpm);
    addBindingIfMissing (bindings, "hostTrackTimeSigNumerator", 4.0f, 1.0f, 64.0f);
    addBindingIfMissing (bindings, "hostTrackTimeSigDenominator", 4.0f, 1.0f, 64.0f);
    addBindingIfMissing (bindings, "hostTrackPhaseRotation", 0.0f, -maximumBeatValue, maximumBeatValue);
    return bindings;
}

bool EmbeddedPerformanceEngine::prepareSequenceUnlocked()
{
    stateRuntimes.clear();
    stateRuntimes.reserve (requestedStates.size());

    for (const auto& state : requestedStates)
    {
        StateRuntime runtime;
        runtime.definition = state;

        if (! prepareStateRuntimeUnlocked (runtime))
        {
            stateRuntimes.clear();
            ready.store (false, std::memory_order_release);
            return false;
        }

        stateRuntimes.push_back (std::move (runtime));
    }

    if (! rebuildTimelineUnlocked())
    {
        stateRuntimes.clear();
        ready.store (false, std::memory_order_release);
        return false;
    }

    playheadFrame = 0;
    currentStateIndex.store (stateRuntimes.empty() ? -1 : 0, std::memory_order_release);
    activeStateCount.store (stateRuntimes.empty() ? 0 : activeStateCountForFrameUnlocked (0), std::memory_order_release);
    ready.store (! stateRuntimes.empty(), std::memory_order_release);
    return true;
}

bool EmbeddedPerformanceEngine::prepareStateRuntimeUnlocked (StateRuntime& runtime)
{
    auto tracks = normaliseTracksForState (runtime.definition);
    runtime.tracks.clear();
    runtime.tracks.reserve (tracks.size());

    for (auto& track : tracks)
    {
        StateRuntime::TrackRuntime trackRuntime;
        trackRuntime.definition = std::move (track);

        if (! prepareTrackRuntimeUnlocked (trackRuntime))
            return false;

        runtime.tracks.push_back (std::move (trackRuntime));
    }

    if (runtime.tracks.empty())
    {
        lastError = "Performance state has no playable tracks";
        return false;
    }

    return true;
}

bool EmbeddedPerformanceEngine::prepareTrackRuntimeUnlocked (StateRuntime::TrackRuntime& runtime)
{
    runtime.engine = std::make_unique<EmbeddedLanguageEngine> (runtime.definition.language);

    if (! runtime.engine->prepare (currentSampleRate, maxBlockSize, numInputChannels, numOutputChannels))
    {
        lastError = "Performance track could not prepare "
                    + EmbeddedLanguageEngine::getLanguageName (runtime.definition.language)
                    + ": "
                    + runtime.engine->getLastError();
        return false;
    }

    const auto bindings = withPerformanceParameterBindings (runtime.definition.parameterBindings);
    auto program = runtime.definition.programBody;

    if (program.isEmpty())
        program = runtime.engine->getCurrentProgram();

    if (program.isNotEmpty()
        && ! runtime.engine->loadProgram (program, bindings))
    {
        lastError = "Performance track could not load "
                    + EmbeddedLanguageEngine::getLanguageName (runtime.definition.language)
                    + " program: "
                    + runtime.engine->getLastError();
        return false;
    }

    return true;
}

bool EmbeddedPerformanceEngine::rebuildTimelineUnlocked()
{
    if (currentSampleRate <= 0.0 || tempoMap.empty())
    {
        lastError = "Cannot build a performance timeline without a valid sample rate and tempo map";
        return false;
    }

    double beat = 0.0;

    for (auto& runtime : stateRuntimes)
    {
        runtime.startBeat = beat;
        runtime.endBeat = runtime.startBeat + runtime.definition.durationBeats;
        runtime.tailEndBeat = runtime.endBeat + runtime.definition.tailBeats;
        runtime.startFrame = beatToFrameUnlocked (runtime.startBeat);
        runtime.endFrame = juce::jmax (runtime.startFrame + 1, beatToFrameUnlocked (runtime.endBeat));
        runtime.tailEndFrame = juce::jmax (runtime.endFrame, beatToFrameUnlocked (runtime.tailEndBeat));
        runtime.durationFrames = runtime.endFrame - runtime.startFrame;
        runtime.tailFrames = runtime.tailEndFrame - runtime.endFrame;
        beat = runtime.endBeat;
    }

    sequenceEndFrame = 0;
    for (const auto& runtime : stateRuntimes)
        sequenceEndFrame = juce::jmax (sequenceEndFrame, runtime.tailEndFrame);

    return sequenceEndFrame > 0;
}

void EmbeddedPerformanceEngine::releaseSequenceUnlocked() noexcept
{
    playing.store (false, std::memory_order_release);
    ready.store (false, std::memory_order_release);
    currentStateIndex.store (-1, std::memory_order_release);
    activeStateCount.store (0, std::memory_order_release);
    stateRuntimes.clear();
    playheadFrame = 0;
    sequenceEndFrame = 0;
}

void EmbeddedPerformanceEngine::resetDiagnostics() noexcept
{
    renderedFrameCount.store (0, std::memory_order_relaxed);
    silentProcessCount.store (0, std::memory_order_relaxed);
    oversizedBlockCount.store (0, std::memory_order_relaxed);
    renderExceptionCount.store (0, std::memory_order_relaxed);
}

bool EmbeddedPerformanceEngine::validatePreparedRenderStateFor (int frames) const noexcept
{
    return frames > 0
           && frames <= maxBlockSize
           && numOutputChannels > 0
           && numOutputChannels <= EmbeddedChucKEngine::maximumChannelLimit;
}

int64_t EmbeddedPerformanceEngine::nextBoundaryAfterUnlocked (int64_t frame) const noexcept
{
    auto next = std::numeric_limits<int64_t>::max();

    for (const auto& runtime : stateRuntimes)
    {
        if (runtime.startFrame > frame)
            next = juce::jmin (next, runtime.startFrame);
        if (runtime.endFrame > frame)
            next = juce::jmin (next, runtime.endFrame);
        if (runtime.tailEndFrame > frame)
            next = juce::jmin (next, runtime.tailEndFrame);

        for (const auto& track : runtime.tracks)
            for (const auto& event : track.definition.gainEvents)
            {
                const auto eventFrame = beatToFrameUnlocked (event.beat);
                if (eventFrame > frame)
                    next = juce::jmin (next, eventFrame);
            }
    }

    return next == std::numeric_limits<int64_t>::max() ? frame + maxBlockSize : next;
}

int EmbeddedPerformanceEngine::currentStateIndexForFrameUnlocked (int64_t frame) const noexcept
{
    for (size_t i = 0; i < stateRuntimes.size(); ++i)
        if (frame >= stateRuntimes[i].startFrame && frame < stateRuntimes[i].endFrame)
            return static_cast<int> (i);

    return -1;
}

int EmbeddedPerformanceEngine::activeStateCountForFrameUnlocked (int64_t frame) const noexcept
{
    auto count = 0;

    for (const auto& runtime : stateRuntimes)
        if (frame >= runtime.startFrame && frame < runtime.tailEndFrame)
            ++count;

    return count;
}

bool EmbeddedPerformanceEngine::stateShouldRender (const StateRuntime& runtime,
                                                   int64_t frameStart,
                                                   int64_t frameEnd) const noexcept
{
    return frameEnd > runtime.startFrame && frameStart < runtime.tailEndFrame;
}

float EmbeddedPerformanceEngine::stateGateAtFrame (const StateRuntime& runtime, int64_t frame) const noexcept
{
    return frame >= runtime.startFrame && frame < runtime.endFrame ? 1.0f : 0.0f;
}

float EmbeddedPerformanceEngine::stateGainAtFrame (const StateRuntime& runtime, int64_t frame) const noexcept
{
    if (frame < runtime.startFrame || frame >= runtime.tailEndFrame)
        return 0.0f;

    auto gain = 1.0f;
    const auto fadeInFrames = juce::jmin<int64_t> (runtime.durationFrames,
                                                  juce::jmax<int64_t> (1, static_cast<int64_t> (std::llround (stateFadeInSeconds * currentSampleRate))));

    if (frame < runtime.startFrame + fadeInFrames)
        gain *= juce::jlimit (0.0f,
                              1.0f,
                              static_cast<float> (frame - runtime.startFrame) / static_cast<float> (fadeInFrames));

    if (frame < runtime.endFrame)
        return gain;

    if (runtime.tailFrames <= 0)
        return 0.0f;

    const auto tailPosition = static_cast<double> (frame - runtime.endFrame)
                              / static_cast<double> (runtime.tailFrames);
    return gain * juce::jlimit (0.0f, 1.0f, static_cast<float> (1.0 - tailPosition));
}

float EmbeddedPerformanceEngine::stateBeatAtFrame (const StateRuntime& runtime, int64_t frame) const noexcept
{
    return juce::jlimit (0.0f,
                         maximumBeatValue,
                         static_cast<float> (juce::jmax (0.0, frameToBeatUnlocked (frame) - runtime.startBeat)));
}

float EmbeddedPerformanceEngine::globalBeatAtFrame (int64_t frame) const noexcept
{
    return juce::jlimit (0.0f, maximumBeatValue, static_cast<float> (frameToBeatUnlocked (frame)));
}

float EmbeddedPerformanceEngine::tempoAtBeatUnlocked (double beat) const noexcept
{
    if (tempoMap.empty())
        return defaultTempoBpm;

    auto bpm = tempoMap.front().bpm;
    for (const auto& event : tempoMap)
    {
        if (event.beat > beat)
            break;

        bpm = event.bpm;
    }

    return static_cast<float> (juce::jlimit (1.0, static_cast<double> (maximumTempoBpm), bpm));
}

EmbeddedPerformanceEngine::TimeSignatureEvent
EmbeddedPerformanceEngine::timeSignatureAtBeatUnlocked (double beat) const noexcept
{
    if (timeSignatureMap.empty())
        return { 0.0, 4, 4 };

    auto signature = timeSignatureMap.front();
    for (const auto& event : timeSignatureMap)
    {
        if (event.beat > beat)
            break;

        signature = event;
    }

    return signature;
}

float EmbeddedPerformanceEngine::phaseRotationAtBeatUnlocked (double beat) const noexcept
{
    if (phaseRotationMap.empty())
        return 0.0f;

    auto rotation = phaseRotationMap.front().rotationBeats;
    for (const auto& event : phaseRotationMap)
    {
        if (event.beat > beat)
            break;

        rotation = event.rotationBeats;
    }

    return juce::jlimit (-maximumBeatValue, maximumBeatValue, static_cast<float> (rotation));
}

float EmbeddedPerformanceEngine::barBeatAtBeatUnlocked (double beat) const noexcept
{
    const auto signature = timeSignatureAtBeatUnlocked (beat);
    const auto barLengthBeats = static_cast<double> (signature.numerator) * (4.0 / static_cast<double> (signature.denominator));

    if (barLengthBeats <= 0.0 || ! std::isfinite (barLengthBeats))
        return 0.0f;

    auto localBeat = beat - signature.beat + static_cast<double> (phaseRotationAtBeatUnlocked (beat));
    localBeat = std::fmod (localBeat, barLengthBeats);

    if (localBeat < 0.0)
        localBeat += barLengthBeats;

    return static_cast<float> (localBeat);
}

float EmbeddedPerformanceEngine::barPhaseAtBeatUnlocked (double beat) const noexcept
{
    const auto signature = timeSignatureAtBeatUnlocked (beat);
    const auto barLengthBeats = static_cast<double> (signature.numerator) * (4.0 / static_cast<double> (signature.denominator));

    if (barLengthBeats <= 0.0 || ! std::isfinite (barLengthBeats))
        return 0.0f;

    return juce::jlimit (0.0f, 1.0f, barBeatAtBeatUnlocked (beat) / static_cast<float> (barLengthBeats));
}

float EmbeddedPerformanceEngine::trackGainAtBeatUnlocked (const StateRuntime::TrackRuntime& trackRuntime, double beat) const noexcept
{
    auto gain = juce::jlimit (0.0f, 4.0f, trackRuntime.definition.gain);

    for (const auto& event : trackRuntime.definition.gainEvents)
    {
        if (event.beat > beat)
            break;

        gain = juce::jlimit (0.0f, 4.0f, event.gain);
    }

    return gain;
}

float EmbeddedPerformanceEngine::trackGainAtFrameUnlocked (const StateRuntime::TrackRuntime& trackRuntime, int64_t frame) const noexcept
{
    return trackGainAtBeatUnlocked (trackRuntime, frameToBeatUnlocked (frame));
}

int64_t EmbeddedPerformanceEngine::beatToFrameUnlocked (double beat) const noexcept
{
    if (beat <= 0.0 || currentSampleRate <= 0.0 || tempoMap.empty())
        return 0;

    auto currentBeat = 0.0;
    auto currentBpm = tempoMap.front().bpm;
    auto frames = 0.0;

    for (size_t i = 1; i < tempoMap.size(); ++i)
    {
        const auto nextBeat = tempoMap[i].beat;
        if (nextBeat <= currentBeat)
        {
            currentBpm = tempoMap[i].bpm;
            continue;
        }

        const auto segmentEndBeat = juce::jmin (beat, nextBeat);
        if (segmentEndBeat > currentBeat)
            frames += beatsToSeconds (segmentEndBeat - currentBeat, currentBpm) * currentSampleRate;

        if (beat <= nextBeat)
            return static_cast<int64_t> (std::llround (frames));

        currentBeat = nextBeat;
        currentBpm = tempoMap[i].bpm;
    }

    if (beat > currentBeat)
        frames += beatsToSeconds (beat - currentBeat, currentBpm) * currentSampleRate;

    return static_cast<int64_t> (std::llround (frames));
}

double EmbeddedPerformanceEngine::frameToBeatUnlocked (int64_t frame) const noexcept
{
    if (frame <= 0 || currentSampleRate <= 0.0 || tempoMap.empty())
        return 0.0;

    const auto targetFrames = static_cast<double> (frame);
    auto currentBeat = 0.0;
    auto currentBpm = tempoMap.front().bpm;
    auto accumulatedFrames = 0.0;

    for (size_t i = 1; i < tempoMap.size(); ++i)
    {
        const auto nextBeat = tempoMap[i].beat;
        if (nextBeat <= currentBeat)
        {
            currentBpm = tempoMap[i].bpm;
            continue;
        }

        const auto segmentFrames = beatsToSeconds (nextBeat - currentBeat, currentBpm) * currentSampleRate;
        if (targetFrames <= accumulatedFrames + segmentFrames)
            return currentBeat + secondsToBeats ((targetFrames - accumulatedFrames) / currentSampleRate, currentBpm);

        accumulatedFrames += segmentFrames;
        currentBeat = nextBeat;
        currentBpm = tempoMap[i].bpm;
    }

    return currentBeat + secondsToBeats ((targetFrames - accumulatedFrames) / currentSampleRate, currentBpm);
}

void EmbeddedPerformanceEngine::pushPerformanceControls (StateRuntime& stateRuntime,
                                                         StateRuntime::TrackRuntime& trackRuntime,
                                                         int64_t frame)
{
    if (trackRuntime.engine == nullptr)
        return;

    const auto globalBeat = static_cast<double> (globalBeatAtFrame (frame));
    const auto signature = timeSignatureAtBeatUnlocked (globalBeat);
    const auto trackTempo = trackRuntime.definition.tightlySynced
                              ? tempoAtBeatUnlocked (globalBeat)
                              : static_cast<float> (juce::jlimit (1.0, static_cast<double> (maximumTempoBpm), trackRuntime.definition.tempoBpm));
    const auto trackNumerator = trackRuntime.definition.tightlySynced ? signature.numerator : trackRuntime.definition.timeSignatureNumerator;
    const auto trackDenominator = trackRuntime.definition.tightlySynced ? signature.denominator : trackRuntime.definition.timeSignatureDenominator;
    const auto trackPhase = trackRuntime.definition.tightlySynced
                              ? phaseRotationAtBeatUnlocked (globalBeat)
                              : static_cast<float> (juce::jlimit (static_cast<double> (-maximumBeatValue),
                                                                  static_cast<double> (maximumBeatValue),
                                                                  trackRuntime.definition.phaseRotationBeats));

    static_cast<void> (trackRuntime.engine->setParameterValue ("hostStateGate", stateGateAtFrame (stateRuntime, frame)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostStateGain", stateGainAtFrame (stateRuntime, frame)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostTempoBpm", tempoAtBeatUnlocked (globalBeat)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostStateBeat", stateBeatAtFrame (stateRuntime, frame)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostGlobalBeat", static_cast<float> (globalBeat)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostTimeSigNumerator", static_cast<float> (signature.numerator)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostTimeSigDenominator", static_cast<float> (signature.denominator)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostBarBeat", barBeatAtBeatUnlocked (globalBeat)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostBarPhase", barPhaseAtBeatUnlocked (globalBeat)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostPhaseRotation", phaseRotationAtBeatUnlocked (globalBeat)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostTrackGain", trackGainAtFrameUnlocked (trackRuntime, frame)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostTrackTempoBpm", trackTempo));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostTrackTimeSigNumerator", static_cast<float> (trackNumerator)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostTrackTimeSigDenominator", static_cast<float> (trackDenominator)));
    static_cast<void> (trackRuntime.engine->setParameterValue ("hostTrackPhaseRotation", trackPhase));
}

void EmbeddedPerformanceEngine::copyInputSegment (const juce::AudioBuffer<float>& input, int offset, int frames)
{
    if (numInputChannels <= 0)
        return;

    scratchInput.clear (0, frames);
    const auto availableFrames = juce::jmax (0, input.getNumSamples() - offset);
    const auto framesToCopy = juce::jmin (frames, availableFrames);

    if (framesToCopy <= 0 || input.getNumChannels() <= 0)
        return;

    for (int channel = 0; channel < numInputChannels; ++channel)
    {
        const auto sourceChannel = juce::jmin (channel, input.getNumChannels() - 1);
        scratchInput.copyFrom (channel, 0, input, sourceChannel, offset, framesToCopy);
    }
}

std::vector<EmbeddedPerformanceEngine::Track> EmbeddedPerformanceEngine::normaliseTracksForState (const State& state)
{
    if (! state.tracks.empty())
        return state.tracks;

    return { { state.name,
               state.language,
               state.programBody,
               state.parameterBindings,
               1.0f,
               true,
               defaultTempoBpm,
               4,
               4,
               0.0,
               {} } };
}

void EmbeddedPerformanceEngine::renderChunkUnlocked (const juce::AudioBuffer<float>& input,
                                                     juce::AudioBuffer<float>& output,
                                                     int offset,
                                                     int frames,
                                                     int64_t frameStart)
{
    copyInputSegment (input, offset, frames);

    for (auto& runtime : stateRuntimes)
    {
        const auto frameEnd = frameStart + frames;
        if (! stateShouldRender (runtime, frameStart, frameEnd))
            continue;

        for (auto& track : runtime.tracks)
        {
            if (track.engine == nullptr)
                continue;

            pushPerformanceControls (runtime, track, frameStart);
            scratchOutput.clear (0, frames);

            for (int channel = 0; channel < numInputChannels; ++channel)
                scratchInputPointers[static_cast<size_t> (channel)] = scratchInput.getWritePointer (channel);

            for (int channel = 0; channel < numOutputChannels; ++channel)
                scratchOutputPointers[static_cast<size_t> (channel)] = scratchOutput.getWritePointer (channel);

            juce::AudioBuffer<float> inputView (scratchInputPointers.data(), numInputChannels, frames);
            juce::AudioBuffer<float> outputView (scratchOutputPointers.data(), numOutputChannels, frames);
            track.engine->process (inputView, outputView);

            for (int channel = 0; channel < output.getNumChannels(); ++channel)
            {
                auto* dst = output.getWritePointer (channel, offset);
                const auto sourceChannel = juce::jmin (channel, scratchOutput.getNumChannels() - 1);
                const auto* src = scratchOutput.getReadPointer (sourceChannel);

                for (int sample = 0; sample < frames; ++sample)
                {
                    const auto frame = frameStart + sample;
                    const auto gain = stateGainAtFrame (runtime, frame) * trackGainAtFrameUnlocked (track, frame);
                    dst[sample] += src[sample] * gain;
                }
            }
        }
    }

    for (int channel = 0; channel < output.getNumChannels(); ++channel)
    {
        auto* dst = output.getWritePointer (channel, offset);

        for (int sample = 0; sample < frames; ++sample)
        {
            const auto value = dst[sample];
            dst[sample] = std::isfinite (value)
                            ? juce::jlimit (-outputSafetyLimit, outputSafetyLimit, value)
                            : 0.0f;
        }
    }
}

bool EmbeddedPerformanceEngine::isFinitePositive (double value) noexcept
{
    return value > 0.0 && std::isfinite (value);
}

int64_t EmbeddedPerformanceEngine::beatsToFrames (double beats, double sampleRate, double bpm) noexcept
{
    if (beats <= 0.0 || sampleRate <= 0.0 || bpm <= 0.0
        || ! std::isfinite (beats) || ! std::isfinite (sampleRate) || ! std::isfinite (bpm))
    {
        return 0;
    }

    const auto frames = beatsToSeconds (beats, bpm) * sampleRate;
    return juce::jmax<int64_t> (1, static_cast<int64_t> (std::llround (frames)));
}
