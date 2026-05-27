#pragma once

#include "EmbeddedLanguageEngine.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class EmbeddedPerformanceEngine
{
public:
    using Language = EmbeddedLanguageEngine::Language;
    using ParameterBinding = EmbeddedLanguageEngine::ParameterBinding;

    struct TrackGainEvent
    {
        double beat = 0.0;
        float gain = 1.0f;
    };

    struct Track
    {
        juce::String name;
        Language language = Language::chuck;
        juce::String programBody;
        std::vector<ParameterBinding> parameterBindings;
        float gain = 1.0f;
        bool tightlySynced = true;
        double tempoBpm = 120.0;
        int timeSignatureNumerator = 4;
        int timeSignatureDenominator = 4;
        double phaseRotationBeats = 0.0;
        std::vector<TrackGainEvent> gainEvents;
    };

    struct State
    {
        juce::String name;
        Language language = Language::chuck;
        juce::String programBody;
        std::vector<ParameterBinding> parameterBindings;
        double durationBeats = 4.0;
        double tailBeats = 0.0;
        std::vector<Track> tracks;
    };

    struct TempoEvent
    {
        double beat = 0.0;
        double bpm = 120.0;
    };

    struct TimeSignatureEvent
    {
        double beat = 0.0;
        int numerator = 4;
        int denominator = 4;
    };

    struct PhaseRotationEvent
    {
        double beat = 0.0;
        double rotationBeats = 0.0;
    };

    EmbeddedPerformanceEngine() = default;
    ~EmbeddedPerformanceEngine();

    EmbeddedPerformanceEngine (EmbeddedPerformanceEngine&&) noexcept = delete;
    EmbeddedPerformanceEngine& operator= (EmbeddedPerformanceEngine&&) noexcept = delete;
    EmbeddedPerformanceEngine (const EmbeddedPerformanceEngine&) = delete;
    EmbeddedPerformanceEngine& operator= (const EmbeddedPerformanceEngine&) = delete;

    bool prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels);
    void release() noexcept;

    bool loadSequence (const std::vector<State>& states);
    bool start();
    void stop() noexcept;
    void resetToStart() noexcept;
    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output);

    bool setTempoBpm (double bpm);
    bool setTempoMap (const std::vector<TempoEvent>& events);
    bool setTimeSignatureMap (const std::vector<TimeSignatureEvent>& events);
    bool setPhaseRotationMap (const std::vector<PhaseRotationEvent>& events);
    bool setStopBeat (double beat);
    double getTempoBpm() const noexcept { return tempoBpm.load (std::memory_order_acquire); }

    bool setStateParameterValue (int stateIndex, const juce::String& name, float value);
    bool setTrackParameterValue (int stateIndex, int trackIndex, const juce::String& name, float value);
    bool setTrackGain (int stateIndex, int trackIndex, float gain);
    bool setParameterValueForAllStates (const juce::String& name, float value);

    bool isReady() const noexcept { return ready.load (std::memory_order_acquire); }
    bool isPlaying() const noexcept { return playing.load (std::memory_order_acquire); }
    int getCurrentStateIndex() const noexcept { return currentStateIndex.load (std::memory_order_acquire); }
    int getActiveStateCount() const noexcept { return activeStateCount.load (std::memory_order_acquire); }
    double getCurrentBeat() const;
    uint64_t getRenderedFrameCount() const noexcept { return renderedFrameCount.load (std::memory_order_relaxed); }
    uint64_t getSilentProcessCount() const noexcept { return silentProcessCount.load (std::memory_order_relaxed); }
    uint64_t getOversizedBlockCount() const noexcept { return oversizedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderExceptionCount() const noexcept { return renderExceptionCount.load (std::memory_order_relaxed); }
    juce::String getLastError() const;

    static double secondsToBeats (double seconds, double bpm) noexcept;
    static double beatsToSeconds (double beats, double bpm) noexcept;
    static std::vector<ParameterBinding> withPerformanceParameterBindings (std::vector<ParameterBinding> bindings);

private:
    struct StateRuntime
    {
        struct TrackRuntime
        {
            Track definition;
            std::unique_ptr<EmbeddedLanguageEngine> engine;
        };

        State definition;
        std::vector<TrackRuntime> tracks;
        double startBeat = 0.0;
        double endBeat = 0.0;
        double tailEndBeat = 0.0;
        int64_t startFrame = 0;
        int64_t endFrame = 0;
        int64_t tailEndFrame = 0;
        int64_t durationFrames = 0;
        int64_t tailFrames = 0;
    };

    bool prepareSequenceUnlocked();
    bool prepareStateRuntimeUnlocked (StateRuntime& runtime);
    bool prepareTrackRuntimeUnlocked (StateRuntime::TrackRuntime& runtime);
    bool rebuildTimelineUnlocked();
    bool setTempoMapUnlocked (std::vector<TempoEvent> events);
    bool setTimeSignatureMapUnlocked (std::vector<TimeSignatureEvent> events);
    bool setPhaseRotationMapUnlocked (std::vector<PhaseRotationEvent> events);
    void releaseSequenceUnlocked() noexcept;
    void resetDiagnostics() noexcept;
    bool validatePreparedRenderStateFor (int frames) const noexcept;
    int64_t nextBoundaryAfterUnlocked (int64_t frame) const noexcept;
    int currentStateIndexForFrameUnlocked (int64_t frame) const noexcept;
    int activeStateCountForFrameUnlocked (int64_t frame) const noexcept;
    bool stateShouldRender (const StateRuntime& runtime, int64_t frameStart, int64_t frameEnd) const noexcept;
    float stateGateAtFrame (const StateRuntime& runtime, int64_t frame) const noexcept;
    float stateGainAtFrame (const StateRuntime& runtime, int64_t frame) const noexcept;
    float stateBeatAtFrame (const StateRuntime& runtime, int64_t frame) const noexcept;
    float globalBeatAtFrame (int64_t frame) const noexcept;
    float tempoAtBeatUnlocked (double beat) const noexcept;
    TimeSignatureEvent timeSignatureAtBeatUnlocked (double beat) const noexcept;
    float phaseRotationAtBeatUnlocked (double beat) const noexcept;
    float barBeatAtBeatUnlocked (double beat) const noexcept;
    float barPhaseAtBeatUnlocked (double beat) const noexcept;
    float trackGainAtBeatUnlocked (const StateRuntime::TrackRuntime& trackRuntime, double beat) const noexcept;
    float trackGainAtFrameUnlocked (const StateRuntime::TrackRuntime& trackRuntime, int64_t frame) const noexcept;
    int64_t beatToFrameUnlocked (double beat) const noexcept;
    double frameToBeatUnlocked (int64_t frame) const noexcept;
    void pushPerformanceControls (StateRuntime& stateRuntime, StateRuntime::TrackRuntime& trackRuntime, int64_t frame);
    void copyInputSegment (const juce::AudioBuffer<float>& input, int offset, int frames);
    static std::vector<Track> normaliseTracksForState (const State& state);
    void renderChunkUnlocked (const juce::AudioBuffer<float>& input,
                              juce::AudioBuffer<float>& output,
                              int offset,
                              int frames,
                              int64_t frameStart);
    static bool isFinitePositive (double value) noexcept;
    static int64_t beatsToFrames (double beats, double sampleRate, double bpm) noexcept;

    mutable juce::CriticalSection engineLock;
    juce::String lastError;
    std::vector<State> requestedStates;
    std::vector<StateRuntime> stateRuntimes;
    std::vector<TempoEvent> tempoMap { { 0.0, 120.0 } };
    std::vector<TimeSignatureEvent> timeSignatureMap { { 0.0, 4, 4 } };
    std::vector<PhaseRotationEvent> phaseRotationMap { { 0.0, 0.0 } };
    double stopBeat = -1.0;
    std::vector<float*> scratchInputPointers;
    std::vector<float*> scratchOutputPointers;
    juce::AudioBuffer<float> scratchInput;
    juce::AudioBuffer<float> scratchOutput;

    double currentSampleRate = 0.0;
    int maxBlockSize = 0;
    int numInputChannels = 0;
    int numOutputChannels = 0;
    int64_t playheadFrame = 0;
    int64_t sequenceEndFrame = 0;

    std::atomic<double> tempoBpm { 120.0 };
    std::atomic<bool> ready { false };
    std::atomic<bool> playing { false };
    std::atomic<int> currentStateIndex { -1 };
    std::atomic<int> activeStateCount { 0 };
    std::atomic<uint64_t> renderedFrameCount { 0 };
    std::atomic<uint64_t> silentProcessCount { 0 };
    std::atomic<uint64_t> oversizedBlockCount { 0 };
    std::atomic<uint64_t> renderExceptionCount { 0 };
};
