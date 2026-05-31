#pragma once

#include "EmbeddedChucKEngine.h"

#include <memory>
#include <vector>

class EmbeddedLanguageEngine
{
public:
    class Runtime;

    using ParameterBinding = EmbeddedChucKEngine::ParameterBinding;

    enum class Language
    {
        chuck,
        faust,
        csound,
        supercollider,
        supercolliderScore,
        rtcmix
    };

    explicit EmbeddedLanguageEngine (Language language = Language::chuck);
    ~EmbeddedLanguageEngine();

    EmbeddedLanguageEngine (EmbeddedLanguageEngine&&) noexcept;
    EmbeddedLanguageEngine& operator= (EmbeddedLanguageEngine&&) noexcept;

    EmbeddedLanguageEngine (const EmbeddedLanguageEngine&) = delete;
    EmbeddedLanguageEngine& operator= (const EmbeddedLanguageEngine&) = delete;

    static std::vector<Language> getSupportedLanguages();
    static juce::String getLanguageName (Language language);
    static juce::String getLanguageBuildStatus (Language language);
    static bool isLanguageBuiltIn (Language language) noexcept;

    Language getLanguage() const noexcept;

    bool prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels);
    void release() noexcept;
    bool loadProgram (const juce::String& programBody);
    bool loadProgram (const juce::String& programBody, const std::vector<ParameterBinding>& bindings);
    bool loadProgramAsync (const juce::String& programBody);
    bool loadProgramAsync (const juce::String& programBody, const std::vector<ParameterBinding>& bindings);
    bool waitForAsyncProgramLoads (int timeoutMilliseconds);
    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output);

    juce::String getCurrentProgram() const;
    std::vector<ParameterBinding> getCurrentParameterBindings() const;

    bool setParameterValue (int index, float value) noexcept;
    bool setParameterValue (const juce::String& name, float value);
    float getParameterValue (int index) const noexcept;
    int getParameterIndex (const juce::String& name) const;
    int getParameterCount() const noexcept;
    void setFrequency (float value);
    void setGain (float value);
    void setToneBlend (float value);
    bool isReady() const noexcept;
    juce::String getLastError() const;

    uint64_t getSilentProcessCount() const noexcept;
    uint64_t getOversizedBlockCount() const noexcept;
    uint64_t getRenderExceptionCount() const noexcept;
    uint64_t getSanitisedSampleCount() const noexcept;
    uint64_t getSanitisedControlCount() const noexcept;
    uint64_t getInternalErrorCount() const noexcept;
    uint64_t getRenderedBlockCount() const noexcept;
    uint64_t getRenderedFrameCount() const noexcept;
    uint64_t getProgramLoadSuccessCount() const noexcept;
    uint64_t getProgramLoadFailureCount() const noexcept;
    uint64_t getAsyncProgramLoadQueuedCount() const noexcept;
    uint64_t getAsyncProgramLoadCompletedCount() const noexcept;
    uint64_t getAsyncProgramLoadDroppedCount() const noexcept;
    bool isAsyncProgramLoadActive() const noexcept;

private:
    static std::unique_ptr<Runtime> createRuntime (Language language);

    Language language;
    std::unique_ptr<Runtime> runtime;
};
