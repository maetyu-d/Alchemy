#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

class ChucK;

class EmbeddedChucKEngine
{
public:
    struct ParameterBinding
    {
        juce::String name;
        float defaultValue = 0.0f;
        float minimumValue = 0.0f;
        float maximumValue = 1.0f;
    };

    EmbeddedChucKEngine();
    ~EmbeddedChucKEngine();

    static constexpr int maximumBlockSizeLimit = 65536;
    static constexpr int maximumChannelLimit = 64;
    static constexpr int maximumParameterCount = 32;

    bool prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels);
    void release() noexcept;
    bool loadProgram (const juce::String& programBody);
    bool loadProgram (const juce::String& programBody, const std::vector<ParameterBinding>& bindings);
    bool loadProgramAsync (const juce::String& programBody);
    bool loadProgramAsync (const juce::String& programBody, const std::vector<ParameterBinding>& bindings);
    bool waitForAsyncProgramLoads (int timeoutMilliseconds);
    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output);

    static juce::String getDefaultProgram();
    static std::vector<ParameterBinding> getDefaultParameterBindings();
    juce::String getCurrentProgram() const;
    std::vector<ParameterBinding> getCurrentParameterBindings() const;

    bool setParameterValue (int index, float value) noexcept;
    bool setParameterValue (const juce::String& name, float value);
    float getParameterValue (int index) const noexcept;
    void pullParameterValuesFromGlobals() noexcept;
    double getGlobalFloatValue (const juce::String& name) const;
    juce::String getGlobalStringValue (const juce::String& name) const;
    int getParameterIndex (const juce::String& name) const;
    int getParameterCount() const noexcept { return activeParameterCount.load (std::memory_order_acquire); }
    void setFrequency (float value);
    void setGain (float value);
    void setToneBlend (float value);
    bool isReady() const noexcept { return ready.load (std::memory_order_acquire); }
    juce::String getLastError() const;
    uint64_t getSilentProcessCount() const noexcept { return silentProcessCount.load (std::memory_order_relaxed); }
    uint64_t getOversizedBlockCount() const noexcept { return oversizedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderExceptionCount() const noexcept { return renderExceptionCount.load (std::memory_order_relaxed); }
    uint64_t getSanitisedSampleCount() const noexcept { return sanitisedSampleCount.load (std::memory_order_relaxed); }
    uint64_t getSanitisedControlCount() const noexcept { return sanitisedControlCount.load (std::memory_order_relaxed); }
    uint64_t getInternalErrorCount() const noexcept { return internalErrorCount.load (std::memory_order_relaxed); }
    uint64_t getRenderedBlockCount() const noexcept { return renderedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderedFrameCount() const noexcept { return renderedFrameCount.load (std::memory_order_relaxed); }
    uint64_t getProgramLoadSuccessCount() const noexcept { return programLoadSuccessCount.load (std::memory_order_relaxed); }
    uint64_t getProgramLoadFailureCount() const noexcept { return programLoadFailureCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadQueuedCount() const noexcept { return asyncProgramLoadQueuedCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadCompletedCount() const noexcept { return asyncProgramLoadCompletedCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadDroppedCount() const noexcept { return asyncProgramLoadDroppedCount.load (std::memory_order_relaxed); }
    bool isAsyncProgramLoadActive() const noexcept { return asyncProgramLoadActive.load (std::memory_order_acquire); }

private:
    struct AsyncProgramLoadRequest
    {
        uint64_t requestId = 0;
        juce::String programBody;
        std::vector<ParameterBinding> bindings;
    };

    void releaseUnlocked() noexcept;
    void resetDiagnostics() noexcept;
    void stopAsyncProgramLoader() noexcept;
    void programLoaderLoop() noexcept;
    void pushGlobals();
    void applyParameterSlots (const std::vector<ParameterBinding>& bindings,
                              const std::vector<float>& values,
                              const std::array<double*, maximumParameterCount>& globalPointers);
    void clearParameterSlots() noexcept;
    bool preparedStateIsValidFor (int frames) const noexcept;
    int getParameterIndexUnlocked (const juce::String& name) const;
    static bool validateParameterBindings (const std::vector<ParameterBinding>& bindings, juce::String& error);
    static bool isValidParameterName (const juce::String& name) noexcept;
    static bool audioSampleNeedsSanitising (float sample) noexcept;
    static bool controlValueNeedsSanitising (float value, float lower, float upper) noexcept;
    static float sanitiseAudioSample (float sample) noexcept;
    static float sanitiseControlValue (float value, float fallback, float lower, float upper) noexcept;

    std::unique_ptr<ChucK> chuck;
    std::vector<float> interleavedInput;
    std::vector<float> interleavedOutput;

    mutable juce::CriticalSection engineLock;
    juce::String lastError;
    juce::String currentProgram;

    double currentSampleRate = 0.0;
    int numInputChannels = 0;
    int numOutputChannels = 0;
    int maxBlockSize = 0;
    uint64_t engineGeneration = 0;
    struct ParameterSlot
    {
        juce::String name;
        std::atomic<float> value { 0.0f };
        std::atomic<float> defaultValue { 0.0f };
        std::atomic<float> minimumValue { 0.0f };
        std::atomic<float> maximumValue { 1.0f };
        double* globalPointer = nullptr;
        bool active = false;
    };

    std::array<ParameterSlot, maximumParameterCount> parameterSlots;
    std::atomic<int> activeParameterCount { 0 };

    std::thread programLoaderThread;
    mutable std::mutex programQueueMutex;
    std::condition_variable programQueueCondition;
    std::condition_variable programQueueIdleCondition;
    std::optional<AsyncProgramLoadRequest> pendingProgramLoad;
    bool stopProgramLoader = false;
    bool programLoaderBusy = false;
    uint64_t nextAsyncProgramLoadId = 0;

    static constexpr float outputSafetyLimit = 0.98f;
    std::atomic<bool> ready { false };
    std::atomic<bool> asyncProgramLoadActive { false };
    std::atomic<uint64_t> silentProcessCount { 0 };
    std::atomic<uint64_t> oversizedBlockCount { 0 };
    std::atomic<uint64_t> renderExceptionCount { 0 };
    std::atomic<uint64_t> sanitisedSampleCount { 0 };
    std::atomic<uint64_t> sanitisedControlCount { 0 };
    std::atomic<uint64_t> internalErrorCount { 0 };
    std::atomic<uint64_t> renderedBlockCount { 0 };
    std::atomic<uint64_t> renderedFrameCount { 0 };
    std::atomic<uint64_t> programLoadSuccessCount { 0 };
    std::atomic<uint64_t> programLoadFailureCount { 0 };
    std::atomic<uint64_t> asyncProgramLoadQueuedCount { 0 };
    std::atomic<uint64_t> asyncProgramLoadCompletedCount { 0 };
    std::atomic<uint64_t> asyncProgramLoadDroppedCount { 0 };
};
