#include "EmbeddedLanguageEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#ifndef WELD_HAS_RTCMIX
#define WELD_HAS_RTCMIX 0
#endif

#ifndef WELD_HAS_SUPERCOLLIDER
#define WELD_HAS_SUPERCOLLIDER 0
#endif

#ifndef WELD_HAS_CSOUND
#define WELD_HAS_CSOUND 0
#endif

#if WELD_HAS_RTCMIX || WELD_HAS_SUPERCOLLIDER || WELD_HAS_CSOUND
#include <dlfcn.h>
#endif

#if WELD_HAS_RTCMIX
#ifndef EMBEDDEDAUDIO
#define EMBEDDEDAUDIO 1
#endif
#include <RTcmix_API.h>
#endif

#if WELD_HAS_SUPERCOLLIDER
#include <SC_WorldOptions.h>
#endif

#ifndef WELD_RTCMIX_DEFAULT_LIBRARY
#define WELD_RTCMIX_DEFAULT_LIBRARY ""
#endif

#ifndef WELD_CSOUND_DEFAULT_LIBRARY
#define WELD_CSOUND_DEFAULT_LIBRARY ""
#endif

#ifndef WELD_SUPERCOLLIDER_DEFAULT_LIBRARY
#define WELD_SUPERCOLLIDER_DEFAULT_LIBRARY ""
#endif

#ifndef WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY
#define WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY ""
#endif

class EmbeddedLanguageEngine::Runtime
{
public:
    virtual ~Runtime() = default;

    virtual bool prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels) = 0;
    virtual void release() noexcept = 0;
    virtual bool loadProgram (const juce::String& programBody, const std::vector<ParameterBinding>& bindings) = 0;
    virtual bool loadProgramAsync (const juce::String& programBody, const std::vector<ParameterBinding>& bindings) = 0;
    virtual bool waitForAsyncProgramLoads (int timeoutMilliseconds) = 0;
    virtual void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output) = 0;

    virtual juce::String getCurrentProgram() const = 0;
    virtual std::vector<ParameterBinding> getCurrentParameterBindings() const = 0;
    virtual bool setParameterValue (int index, float value) noexcept = 0;
    virtual bool setParameterValue (const juce::String& name, float value) = 0;
    virtual float getParameterValue (int index) const noexcept = 0;
    virtual int getParameterIndex (const juce::String& name) const = 0;
    virtual int getParameterCount() const noexcept = 0;
    virtual bool isReady() const noexcept = 0;
    virtual juce::String getLastError() const = 0;

    virtual uint64_t getSilentProcessCount() const noexcept = 0;
    virtual uint64_t getOversizedBlockCount() const noexcept = 0;
    virtual uint64_t getRenderExceptionCount() const noexcept = 0;
    virtual uint64_t getSanitisedSampleCount() const noexcept = 0;
    virtual uint64_t getSanitisedControlCount() const noexcept = 0;
    virtual uint64_t getInternalErrorCount() const noexcept = 0;
    virtual uint64_t getRenderedBlockCount() const noexcept = 0;
    virtual uint64_t getRenderedFrameCount() const noexcept = 0;
    virtual uint64_t getProgramLoadSuccessCount() const noexcept = 0;
    virtual uint64_t getProgramLoadFailureCount() const noexcept = 0;
    virtual uint64_t getAsyncProgramLoadQueuedCount() const noexcept = 0;
    virtual uint64_t getAsyncProgramLoadCompletedCount() const noexcept = 0;
    virtual uint64_t getAsyncProgramLoadDroppedCount() const noexcept = 0;
    virtual bool isAsyncProgramLoadActive() const noexcept = 0;
};

namespace
{
class ChucKRuntime final : public EmbeddedLanguageEngine::Runtime
{
public:
    bool prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels) override
    {
        return engine.prepare (sampleRate, maximumBlockSize, inputChannels, outputChannels);
    }

    void release() noexcept override
    {
        engine.release();
    }

    bool loadProgram (const juce::String& programBody,
                      const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings) override
    {
        return engine.loadProgram (programBody, bindings);
    }

    bool loadProgramAsync (const juce::String& programBody,
                           const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings) override
    {
        return engine.loadProgramAsync (programBody, bindings);
    }

    bool waitForAsyncProgramLoads (int timeoutMilliseconds) override
    {
        return engine.waitForAsyncProgramLoads (timeoutMilliseconds);
    }

    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output) override
    {
        engine.process (input, output);
    }

    juce::String getCurrentProgram() const override { return engine.getCurrentProgram(); }
    std::vector<EmbeddedLanguageEngine::ParameterBinding> getCurrentParameterBindings() const override { return engine.getCurrentParameterBindings(); }
    bool setParameterValue (int index, float value) noexcept override { return engine.setParameterValue (index, value); }
    bool setParameterValue (const juce::String& name, float value) override { return engine.setParameterValue (name, value); }
    float getParameterValue (int index) const noexcept override { return engine.getParameterValue (index); }
    int getParameterIndex (const juce::String& name) const override { return engine.getParameterIndex (name); }
    int getParameterCount() const noexcept override { return engine.getParameterCount(); }
    bool isReady() const noexcept override { return engine.isReady(); }
    juce::String getLastError() const override { return engine.getLastError(); }

    uint64_t getSilentProcessCount() const noexcept override { return engine.getSilentProcessCount(); }
    uint64_t getOversizedBlockCount() const noexcept override { return engine.getOversizedBlockCount(); }
    uint64_t getRenderExceptionCount() const noexcept override { return engine.getRenderExceptionCount(); }
    uint64_t getSanitisedSampleCount() const noexcept override { return engine.getSanitisedSampleCount(); }
    uint64_t getSanitisedControlCount() const noexcept override { return engine.getSanitisedControlCount(); }
    uint64_t getInternalErrorCount() const noexcept override { return engine.getInternalErrorCount(); }
    uint64_t getRenderedBlockCount() const noexcept override { return engine.getRenderedBlockCount(); }
    uint64_t getRenderedFrameCount() const noexcept override { return engine.getRenderedFrameCount(); }
    uint64_t getProgramLoadSuccessCount() const noexcept override { return engine.getProgramLoadSuccessCount(); }
    uint64_t getProgramLoadFailureCount() const noexcept override { return engine.getProgramLoadFailureCount(); }
    uint64_t getAsyncProgramLoadQueuedCount() const noexcept override { return engine.getAsyncProgramLoadQueuedCount(); }
    uint64_t getAsyncProgramLoadCompletedCount() const noexcept override { return engine.getAsyncProgramLoadCompletedCount(); }
    uint64_t getAsyncProgramLoadDroppedCount() const noexcept override { return engine.getAsyncProgramLoadDroppedCount(); }
    bool isAsyncProgramLoadActive() const noexcept override { return engine.isAsyncProgramLoadActive(); }

private:
    EmbeddedChucKEngine engine;
};

#if WELD_HAS_CSOUND
class CsoundRuntime final : public EmbeddedLanguageEngine::Runtime
{
public:
    CsoundRuntime()
    {
        programLoaderThread = std::thread ([this] { programLoaderLoop(); });
    }

    ~CsoundRuntime() override
    {
        stopAsyncProgramLoader();
        release();
    }

    bool prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels) override
    {
        const juce::ScopedLock lock (engineLock);

        releaseUnlocked();
        resetDiagnostics();

        if (sampleRate <= 0.0 || ! std::isfinite (sampleRate))
        {
            lastError = "Invalid Csound audio sample rate";
            return false;
        }

        if (maximumBlockSize <= 0 || maximumBlockSize > EmbeddedChucKEngine::maximumBlockSizeLimit)
        {
            lastError = "Unsupported Csound audio block size";
            return false;
        }

        if (inputChannels < 0
            || inputChannels > EmbeddedChucKEngine::maximumChannelLimit
            || outputChannels <= 0
            || outputChannels > EmbeddedChucKEngine::maximumChannelLimit)
        {
            lastError = "Unsupported Csound audio channel count";
            return false;
        }

        if (! ensureApiLoaded())
            return false;

        currentSampleRate = sampleRate;
        maxBlockSize = maximumBlockSize;
        numInputChannels = inputChannels;
        numOutputChannels = outputChannels;

        if (! loadProgramUnlocked (getDefaultProgram(), EmbeddedChucKEngine::getDefaultParameterBindings(), true))
        {
            releaseUnlocked();
            return false;
        }

        ready.store (true, std::memory_order_release);
        lastError.clear();
        return true;
    }

    void release() noexcept override
    {
        const juce::ScopedLock lock (engineLock);
        releaseUnlocked();
    }

    bool loadProgram (const juce::String& programBody,
                      const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings) override
    {
        std::vector<EmbeddedLanguageEngine::ParameterBinding> normalisedBindings = bindings;
        juce::String validationError;

        if (! validateParameterBindings (normalisedBindings, validationError))
        {
            const juce::ScopedLock lock (engineLock);
            lastError = validationError;
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        const juce::ScopedLock lock (engineLock);
        if (currentSampleRate <= 0.0 || maxBlockSize <= 0)
        {
            lastError = "Cannot load a Csound orchestra before the engine is prepared";
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        return loadProgramUnlocked (programBody, normalisedBindings, false);
    }

    bool loadProgramAsync (const juce::String& programBody,
                           const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings) override
    {
        std::vector<EmbeddedLanguageEngine::ParameterBinding> normalisedBindings = bindings;
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
            if (currentSampleRate <= 0.0 || maxBlockSize <= 0)
            {
                lastError = "Cannot queue a Csound orchestra before the engine is prepared";
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
            const juce::ScopedLock lock (engineLock);
            lastError = "Cannot queue a Csound orchestra after the loader has stopped";
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        programQueueCondition.notify_one();
        return true;
    }

    bool waitForAsyncProgramLoads (int timeoutMilliseconds) override
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

    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output) override
    {
        const juce::ScopedTryLock lock (engineLock);
        output.clear();

        if (! lock.isLocked() || ! ready.load (std::memory_order_acquire) || activeInstance == nullptr)
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

        if (activeInstance->ksmps != 1
            || activeInstance->outputChannels <= 0
            || activeInstance->outputChannels > EmbeddedChucKEngine::maximumChannelLimit
            || activeInstance->inputChannels < 0
            || activeInstance->inputChannels > EmbeddedChucKEngine::maximumChannelLimit)
        {
            internalErrorCount.fetch_add (1, std::memory_order_relaxed);
            ready.store (false, std::memory_order_release);
            return;
        }

        uint64_t sanitisedInBlock = 0;
        const auto inputFrames = juce::jmin (frames, input.getNumSamples());
        pushControlChannels (activeInstance);

        for (int frame = 0; frame < frames; ++frame)
        {
            writeSpinFrame (activeInstance, input, frame, inputFrames, sanitisedInBlock);

            const auto result = csoundApi.performKsmps (activeInstance->csound);
            if (result != 0)
            {
                renderExceptionCount.fetch_add (1, std::memory_order_relaxed);
                ready.store (false, std::memory_order_release);
                output.clear();
                return;
            }

            readSpoutFrame (activeInstance, output, frame, sanitisedInBlock);
        }

        if (sanitisedInBlock != 0)
            sanitisedSampleCount.fetch_add (sanitisedInBlock, std::memory_order_relaxed);

        renderedBlockCount.fetch_add (1, std::memory_order_relaxed);
        renderedFrameCount.fetch_add (static_cast<uint64_t> (frames), std::memory_order_relaxed);
    }

    juce::String getCurrentProgram() const override
    {
        const juce::ScopedLock lock (engineLock);
        return currentProgram;
    }

    std::vector<EmbeddedLanguageEngine::ParameterBinding> getCurrentParameterBindings() const override
    {
        const juce::ScopedLock lock (engineLock);
        return currentBindings;
    }

    bool setParameterValue (int index, float value) noexcept override
    {
        const auto count = activeParameterCount.load (std::memory_order_acquire);
        if (index < 0 || index >= count || index >= EmbeddedChucKEngine::maximumParameterCount)
            return false;

        const auto& binding = currentBindings[static_cast<size_t> (index)];
        if (controlValueNeedsSanitising (value, binding.minimumValue, binding.maximumValue))
            sanitisedControlCount.fetch_add (1, std::memory_order_relaxed);

        parameterValues[static_cast<size_t> (index)].store (sanitiseControlValue (value,
                                                                                  binding.defaultValue,
                                                                                  binding.minimumValue,
                                                                                  binding.maximumValue),
                                                            std::memory_order_relaxed);
        return true;
    }

    bool setParameterValue (const juce::String& name, float value) override
    {
        const juce::ScopedLock lock (engineLock);
        return setParameterValue (getParameterIndexUnlocked (name), value);
    }

    float getParameterValue (int index) const noexcept override
    {
        const auto count = activeParameterCount.load (std::memory_order_acquire);
        if (index < 0 || index >= count || index >= EmbeddedChucKEngine::maximumParameterCount)
            return 0.0f;

        return parameterValues[static_cast<size_t> (index)].load (std::memory_order_relaxed);
    }

    int getParameterIndex (const juce::String& name) const override
    {
        const juce::ScopedLock lock (engineLock);
        return getParameterIndexUnlocked (name);
    }

    int getParameterCount() const noexcept override { return activeParameterCount.load (std::memory_order_acquire); }
    bool isReady() const noexcept override { return ready.load (std::memory_order_acquire); }

    juce::String getLastError() const override
    {
        const juce::ScopedLock lock (engineLock);
        return lastError;
    }

    uint64_t getSilentProcessCount() const noexcept override { return silentProcessCount.load (std::memory_order_relaxed); }
    uint64_t getOversizedBlockCount() const noexcept override { return oversizedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderExceptionCount() const noexcept override { return renderExceptionCount.load (std::memory_order_relaxed); }
    uint64_t getSanitisedSampleCount() const noexcept override { return sanitisedSampleCount.load (std::memory_order_relaxed); }
    uint64_t getSanitisedControlCount() const noexcept override { return sanitisedControlCount.load (std::memory_order_relaxed); }
    uint64_t getInternalErrorCount() const noexcept override { return internalErrorCount.load (std::memory_order_relaxed); }
    uint64_t getRenderedBlockCount() const noexcept override { return renderedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderedFrameCount() const noexcept override { return renderedFrameCount.load (std::memory_order_relaxed); }
    uint64_t getProgramLoadSuccessCount() const noexcept override { return programLoadSuccessCount.load (std::memory_order_relaxed); }
    uint64_t getProgramLoadFailureCount() const noexcept override { return programLoadFailureCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadQueuedCount() const noexcept override { return asyncProgramLoadQueuedCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadCompletedCount() const noexcept override { return asyncProgramLoadCompletedCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadDroppedCount() const noexcept override { return asyncProgramLoadDroppedCount.load (std::memory_order_relaxed); }
    bool isAsyncProgramLoadActive() const noexcept override { return asyncProgramLoadActive.load (std::memory_order_acquire); }

private:
    struct Csound;

    struct Instance
    {
        Csound* csound = nullptr;
        int ksmps = 0;
        int inputChannels = 0;
        int outputChannels = 0;
    };

    struct AsyncProgramLoadRequest
    {
        uint64_t requestId = 0;
        juce::String programBody;
        std::vector<EmbeddedLanguageEngine::ParameterBinding> bindings;
    };

    struct Api
    {
        using CreateLegacyFn = Csound* (*) (void*);
        using CreateModernFn = Csound* (*) (void*, const char*);
        using DestroyFn = void (*) (Csound*);
        using SetOptionFn = int (*) (Csound*, const char*);
        using SetHostImplementedAudioIOFn = void (*) (Csound*, int, int);
        using CompileOrcLegacyFn = int (*) (Csound*, const char*);
        using CompileOrcModernFn = int (*) (Csound*, const char*, int);
        using StartFn = int (*) (Csound*);
        using PerformKsmpsFn = int (*) (Csound*);
        using CleanupFn = int (*) (Csound*);
        using StopFn = void (*) (Csound*);
        using ResetFn = void (*) (Csound*);
        using GetKsmpsFn = uint32_t (*) (Csound*);
        using GetChannelsFn = uint32_t (*) (Csound*, int);
        using GetNchnlsLegacyFn = uint32_t (*) (Csound*);
        using GetNchnlsInputLegacyFn = uint32_t (*) (Csound*);
        using GetSpinFn = void* (*) (Csound*);
        using GetSpoutFn = void* (*) (Csound*);
        using ClearSpinFn = void (*) (Csound*);
        using InputMessageFn = void (*) (Csound*, const char*);
        using EventStringFn = void (*) (Csound*, const char*, int);
        using GetVersionFn = int (*) ();
        using GetSizeOfMYFLTFn = int (*) ();
        using SetControlChannelDoubleFn = void (*) (Csound*, const char*, double);
        using SetControlChannelFloatFn = void (*) (Csound*, const char*, float);

        bool isLoaded() const noexcept { return libraryHandle != nullptr; }

        void* libraryHandle = nullptr;
        void* createSymbol = nullptr;
        void* compileOrcSymbol = nullptr;
        DestroyFn destroy = nullptr;
        SetOptionFn setOption = nullptr;
        SetHostImplementedAudioIOFn setHostImplementedAudioIO = nullptr;
        StartFn start = nullptr;
        PerformKsmpsFn performKsmps = nullptr;
        CleanupFn cleanup = nullptr;
        StopFn stop = nullptr;
        ResetFn reset = nullptr;
        GetKsmpsFn getKsmps = nullptr;
        GetChannelsFn getChannels = nullptr;
        GetNchnlsLegacyFn getNchnls = nullptr;
        GetNchnlsInputLegacyFn getNchnlsInput = nullptr;
        GetSpinFn getSpin = nullptr;
        GetSpoutFn getSpout = nullptr;
        ClearSpinFn clearSpin = nullptr;
        InputMessageFn inputMessage = nullptr;
        EventStringFn eventString = nullptr;
        GetVersionFn getVersion = nullptr;
        GetSizeOfMYFLTFn getSizeOfMYFLT = nullptr;
        SetControlChannelDoubleFn setControlChannelDouble = nullptr;
        SetControlChannelFloatFn setControlChannelFloat = nullptr;
        int version = 0;
        int myfltSize = static_cast<int> (sizeof (double));
    };

    bool ensureApiLoaded()
    {
        if (csoundApi.isLoaded())
            return true;

        juce::String loadErrors;
        for (const auto& candidate : getCsoundLibraryCandidates())
        {
            dlerror();
            auto* handle = dlopen (candidate.toRawUTF8(), RTLD_NOW | RTLD_LOCAL);

            if (handle == nullptr)
            {
                if (const auto* error = dlerror())
                    loadErrors << "\n" << candidate << ": " << error;
                continue;
            }

            csoundApi.libraryHandle = handle;
            juce::String symbolError;
            const auto symbolsLoaded =
                loadRawSymbol (csoundApi.createSymbol, "csoundCreate", symbolError)
                && loadSymbol (csoundApi.destroy, "csoundDestroy", symbolError)
                && loadRawSymbol (csoundApi.compileOrcSymbol, "csoundCompileOrc", symbolError)
                && loadSymbol (csoundApi.start, "csoundStart", symbolError)
                && loadSymbol (csoundApi.performKsmps, "csoundPerformKsmps", symbolError)
                && loadSymbol (csoundApi.getKsmps, "csoundGetKsmps", symbolError)
                && loadSymbol (csoundApi.getSpin, "csoundGetSpin", symbolError)
                && loadSymbol (csoundApi.getSpout, "csoundGetSpout", symbolError)
                && loadSymbol (csoundApi.setControlChannelDouble, "csoundSetControlChannel", symbolError);

            if (symbolsLoaded)
            {
                loadOptionalSymbol (csoundApi.setOption, "csoundSetOption");
                loadOptionalSymbol (csoundApi.setHostImplementedAudioIO, "csoundSetHostImplementedAudioIO");
                loadOptionalSymbol (csoundApi.cleanup, "csoundCleanup");
                loadOptionalSymbol (csoundApi.stop, "csoundStop");
                loadOptionalSymbol (csoundApi.reset, "csoundReset");
                loadOptionalSymbol (csoundApi.getChannels, "csoundGetChannels");
                loadOptionalSymbol (csoundApi.getNchnls, "csoundGetNchnls");
                loadOptionalSymbol (csoundApi.getNchnlsInput, "csoundGetNchnlsInput");
                loadOptionalSymbol (csoundApi.clearSpin, "csoundClearSpin");
                loadOptionalSymbol (csoundApi.inputMessage, "csoundInputMessage");
                loadOptionalSymbol (csoundApi.eventString, "csoundEventString");
                loadOptionalSymbol (csoundApi.getVersion, "csoundGetVersion");
                loadOptionalSymbol (csoundApi.getSizeOfMYFLT, "csoundGetSizeOfMYFLT");
                csoundApi.setControlChannelFloat =
                    reinterpret_cast<Api::SetControlChannelFloatFn> (csoundApi.setControlChannelDouble);
                csoundApi.version = csoundApi.getVersion != nullptr ? csoundApi.getVersion() : 0;
                csoundApi.myfltSize = csoundApi.getSizeOfMYFLT != nullptr
                                        ? csoundApi.getSizeOfMYFLT()
                                        : static_cast<int> (sizeof (double));

                if (csoundApi.getChannels == nullptr
                    && (csoundApi.getNchnls == nullptr || csoundApi.getNchnlsInput == nullptr))
                {
                    loadErrors << "\n" << candidate << ": missing channel-count API";
                }
                else if (csoundApi.eventString == nullptr && csoundApi.inputMessage == nullptr)
                {
                    loadErrors << "\n" << candidate << ": missing score event API";
                }
                else if (csoundApi.myfltSize == static_cast<int> (sizeof (float))
                         || csoundApi.myfltSize == static_cast<int> (sizeof (double)))
                {
                    return true;
                }
                else
                {
                    loadErrors << "\n" << candidate << ": unsupported MYFLT size " << csoundApi.myfltSize;
                }
            }
            else
            {
                loadErrors << "\n" << candidate << ": " << symbolError;
            }

            unloadApi();
        }

        lastError = "Could not load the embedded Csound runtime library";
        if (loadErrors.isNotEmpty())
            lastError << ":" << loadErrors;

        return false;
    }

    bool loadRawSymbol (void*& symbol, const char* symbolName, juce::String& error)
    {
        dlerror();
        symbol = dlsym (csoundApi.libraryHandle, symbolName);

        if (symbol != nullptr)
            return true;

        error = juce::String ("missing symbol ") + symbolName;
        if (const auto* loaderError = dlerror())
            error << " (" << loaderError << ")";

        return false;
    }

    template <typename Function>
    bool loadSymbol (Function& function, const char* symbolName, juce::String& error)
    {
        dlerror();
        function = reinterpret_cast<Function> (dlsym (csoundApi.libraryHandle, symbolName));

        if (function != nullptr)
            return true;

        error = juce::String ("missing symbol ") + symbolName;
        if (const auto* loaderError = dlerror())
            error << " (" << loaderError << ")";

        return false;
    }

    template <typename Function>
    void loadOptionalSymbol (Function& function, const char* symbolName)
    {
        dlerror();
        function = reinterpret_cast<Function> (dlsym (csoundApi.libraryHandle, symbolName));
    }

    Csound* createCsound()
    {
        if (usesModernCsoundSignatures())
        {
            const auto create = reinterpret_cast<Api::CreateModernFn> (csoundApi.createSymbol);
            return create (nullptr, nullptr);
        }

        const auto create = reinterpret_cast<Api::CreateLegacyFn> (csoundApi.createSymbol);
        return create (nullptr);
    }

    int compileOrchestra (Csound* csound, const char* orchestra)
    {
        if (usesModernCsoundSignatures())
        {
            const auto compile = reinterpret_cast<Api::CompileOrcModernFn> (csoundApi.compileOrcSymbol);
            return compile (csound, orchestra, 0);
        }

        const auto compile = reinterpret_cast<Api::CompileOrcLegacyFn> (csoundApi.compileOrcSymbol);
        return compile (csound, orchestra);
    }

    int getOutputChannelCount (Csound* csound) const
    {
        if (csoundApi.getChannels != nullptr)
        {
            const auto channels = static_cast<int> (csoundApi.getChannels (csound, 0));
            if (channels > 0)
                return channels;
        }

        return static_cast<int> (csoundApi.getNchnls (csound));
    }

    int getInputChannelCount (Csound* csound) const
    {
        if (csoundApi.getChannels != nullptr)
        {
            const auto channels = static_cast<int> (csoundApi.getChannels (csound, 1));
            if (channels > 0)
                return channels;
        }

        return static_cast<int> (csoundApi.getNchnlsInput (csound));
    }

    void queueScoreEvent (Csound* csound, const char* eventText)
    {
        if (csoundApi.eventString != nullptr)
            csoundApi.eventString (csound, eventText, 0);
        else
            csoundApi.inputMessage (csound, eventText);
    }

    bool usesModernCsoundSignatures() const noexcept
    {
        return csoundApi.version >= 7000;
    }

    bool loadProgramUnlocked (const juce::String& programBody,
                              const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings,
                              bool preparing)
    {
        std::vector<float> values;
        values.reserve (bindings.size());

        for (const auto& binding : bindings)
        {
            const auto existingIndex = getParameterIndexUnlocked (binding.name);
            const auto value = existingIndex >= 0
                                 ? parameterValues[static_cast<size_t> (existingIndex)].load (std::memory_order_relaxed)
                                 : binding.defaultValue;

            values.push_back (sanitiseControlValue (value,
                                                    binding.defaultValue,
                                                    binding.minimumValue,
                                                    binding.maximumValue));
        }

        juce::String error;
        auto* candidate = createInstance (programBody, bindings, values, error);
        if (candidate == nullptr)
        {
            lastError = error;
            if (! preparing)
                programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);

            return false;
        }

        auto* oldInstance = activeInstance;
        activeInstance = candidate;
        destroyInstance (oldInstance);
        applyParameterSlots (bindings, values);
        currentProgram = programBody;
        ready.store (true, std::memory_order_release);
        programLoadSuccessCount.fetch_add (1, std::memory_order_relaxed);
        lastError.clear();
        return true;
    }

    Instance* createInstance (const juce::String& programBody,
                              const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings,
                              const std::vector<float>& values,
                              juce::String& error)
    {
        auto* instance = new Instance();
        instance->csound = createCsound();

        if (instance->csound == nullptr)
        {
            delete instance;
            error = "Csound failed to create an embedded runtime";
            return nullptr;
        }

        if (csoundApi.setHostImplementedAudioIO != nullptr)
            csoundApi.setHostImplementedAudioIO (instance->csound, 1, 0);

        if (csoundApi.setOption != nullptr)
        {
            static_cast<void> (csoundApi.setOption (instance->csound, "-d"));
            static_cast<void> (csoundApi.setOption (instance->csound, "-m0"));
            if (csoundApi.setHostImplementedAudioIO == nullptr)
                static_cast<void> (csoundApi.setOption (instance->csound, "-n"));
        }

        const auto orchestra = buildOrchestra (programBody).toStdString();
        if (compileOrchestra (instance->csound, orchestra.c_str()) != 0)
        {
            destroyInstance (instance);
            error = "Csound orchestra did not compile";
            return nullptr;
        }

        if (csoundApi.start (instance->csound) != 0)
        {
            destroyInstance (instance);
            error = "Csound failed to start host-driven performance";
            return nullptr;
        }

        instance->ksmps = static_cast<int> (csoundApi.getKsmps (instance->csound));
        instance->outputChannels = getOutputChannelCount (instance->csound);
        instance->inputChannels = getInputChannelCount (instance->csound);

        if (instance->ksmps != 1)
        {
            destroyInstance (instance);
            error = "Csound backend requires ksmps = 1 for sample-tight host rendering";
            return nullptr;
        }

        if (instance->outputChannels != numOutputChannels
            || (numInputChannels > 0 && instance->inputChannels != numInputChannels))
        {
            destroyInstance (instance);
            error = "Csound orchestra channel counts did not match the host configuration";
            return nullptr;
        }

        pushControlChannels (instance, bindings, values);
        queueScoreEvent (instance->csound, "i 1 0 -1");
        return instance;
    }

    juce::String buildOrchestra (const juce::String& programBody) const
    {
        juce::String orchestra;
        orchestra << "sr = " << juce::String (currentSampleRate, 10) << "\n"
                  << "ksmps = 1\n"
                  << "nchnls = " << numOutputChannels << "\n"
                  << "nchnls_i = " << numInputChannels << "\n"
                  << "0dbfs = 1\n\n"
                  << programBody.trim() << "\n";
        return orchestra;
    }

    void destroyInstance (Instance* instance) noexcept
    {
        if (instance == nullptr)
            return;

        if (instance->csound != nullptr)
        {
            try
            {
                if (csoundApi.stop != nullptr)
                    csoundApi.stop (instance->csound);

                if (csoundApi.cleanup != nullptr)
                    static_cast<void> (csoundApi.cleanup (instance->csound));
                else if (csoundApi.reset != nullptr)
                    csoundApi.reset (instance->csound);

                if (csoundApi.destroy != nullptr)
                    csoundApi.destroy (instance->csound);
            }
            catch (...)
            {
            }
        }

        delete instance;
    }

    void releaseUnlocked() noexcept
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
        destroyInstance (activeInstance);
        activeInstance = nullptr;
        unloadApi();
        currentProgram.clear();
        currentBindings.clear();
        activeParameterCount.store (0, std::memory_order_release);
        currentSampleRate = 0.0;
        maxBlockSize = 0;
        numInputChannels = 0;
        numOutputChannels = 0;
    }

    void unloadApi() noexcept
    {
        if (csoundApi.libraryHandle != nullptr)
            dlclose (csoundApi.libraryHandle);

        csoundApi = {};
    }

    void stopAsyncProgramLoader() noexcept
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

    void programLoaderLoop() noexcept
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

    void applyParameterSlots (const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings,
                              const std::vector<float>& values)
    {
        currentBindings = bindings;
        if (currentBindings.size() > static_cast<size_t> (EmbeddedChucKEngine::maximumParameterCount))
            currentBindings.resize (EmbeddedChucKEngine::maximumParameterCount);

        const auto count = currentBindings.size();
        for (size_t i = 0; i < count; ++i)
        {
            const auto& binding = currentBindings[i];
            const auto value = i < values.size() ? values[i] : binding.defaultValue;
            parameterValues[i].store (sanitiseControlValue (value,
                                                            binding.defaultValue,
                                                            binding.minimumValue,
                                                            binding.maximumValue),
                                      std::memory_order_relaxed);
        }

        for (size_t i = count; i < parameterValues.size(); ++i)
            parameterValues[i].store (0.0f, std::memory_order_relaxed);

        activeParameterCount.store (static_cast<int> (count), std::memory_order_release);
        pushControlChannels (activeInstance);
    }

    void pushControlChannels (Instance* instance)
    {
        if (instance == nullptr || instance->csound == nullptr)
            return;

        const auto count = activeParameterCount.load (std::memory_order_acquire);
        for (int i = 0; i < count; ++i)
            setControlChannel (instance,
                               currentBindings[static_cast<size_t> (i)].name,
                               parameterValues[static_cast<size_t> (i)].load (std::memory_order_relaxed));
    }

    void pushControlChannels (Instance* instance,
                              const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings,
                              const std::vector<float>& values)
    {
        if (instance == nullptr || instance->csound == nullptr)
            return;

        const auto count = juce::jmin (bindings.size(), values.size());
        for (size_t i = 0; i < count; ++i)
            setControlChannel (instance, bindings[i].name, values[i]);
    }

    void setControlChannel (Instance* instance, const juce::String& name, float value)
    {
        if (csoundApi.myfltSize == static_cast<int> (sizeof (float)))
            csoundApi.setControlChannelFloat (instance->csound, name.toRawUTF8(), value);
        else
            csoundApi.setControlChannelDouble (instance->csound, name.toRawUTF8(), static_cast<double> (value));
    }

    void writeSpinFrame (Instance* instance,
                         const juce::AudioBuffer<float>& input,
                         int frame,
                         int inputFrames,
                         uint64_t& sanitisedInBlock)
    {
        if (instance->inputChannels <= 0)
            return;

        auto* spin = csoundApi.getSpin (instance->csound);
        if (spin == nullptr)
            return;

        if (csoundApi.myfltSize == static_cast<int> (sizeof (float)))
        {
            auto* samples = static_cast<float*> (spin);
            for (int channel = 0; channel < instance->inputChannels; ++channel)
                samples[channel] = getInputSample (input, frame, inputFrames, channel, sanitisedInBlock);
        }
        else
        {
            auto* samples = static_cast<double*> (spin);
            for (int channel = 0; channel < instance->inputChannels; ++channel)
                samples[channel] = static_cast<double> (getInputSample (input, frame, inputFrames, channel, sanitisedInBlock));
        }
    }

    float getInputSample (const juce::AudioBuffer<float>& input,
                          int frame,
                          int inputFrames,
                          int channel,
                          uint64_t& sanitisedInBlock) const noexcept
    {
        if (frame >= inputFrames || input.getNumChannels() <= 0)
            return 0.0f;

        const auto sample = input.getSample (juce::jmin (channel, input.getNumChannels() - 1), frame);
        if (audioSampleNeedsSanitising (sample))
            ++sanitisedInBlock;

        return sanitiseAudioSample (sample);
    }

    void readSpoutFrame (Instance* instance,
                         juce::AudioBuffer<float>& output,
                         int frame,
                         uint64_t& sanitisedInBlock)
    {
        auto* spout = csoundApi.getSpout (instance->csound);
        if (spout == nullptr)
            return;

        for (int channel = 0; channel < output.getNumChannels(); ++channel)
        {
            const auto sourceChannel = juce::jmin (channel, instance->outputChannels - 1);
            const auto sample = csoundApi.myfltSize == static_cast<int> (sizeof (float))
                                  ? static_cast<float*> (spout)[sourceChannel]
                                  : static_cast<float> (static_cast<double*> (spout)[sourceChannel]);

            if (audioSampleNeedsSanitising (sample))
                ++sanitisedInBlock;

            output.setSample (channel, frame, sanitiseAudioSample (sample));
        }
    }

    int getParameterIndexUnlocked (const juce::String& name) const
    {
        for (size_t i = 0; i < currentBindings.size(); ++i)
            if (currentBindings[i].name == name)
                return static_cast<int> (i);

        return -1;
    }

    static bool validateParameterBindings (const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings,
                                           juce::String& error)
    {
        if (bindings.size() > static_cast<size_t> (EmbeddedChucKEngine::maximumParameterCount))
        {
            error = "Too many Csound parameter bindings";
            return false;
        }

        for (size_t i = 0; i < bindings.size(); ++i)
        {
            const auto& binding = bindings[i];

            if (! isValidParameterName (binding.name))
            {
                error = "Invalid Csound parameter binding name: " + binding.name;
                return false;
            }

            if (! std::isfinite (binding.minimumValue)
                || ! std::isfinite (binding.maximumValue)
                || ! std::isfinite (binding.defaultValue)
                || binding.minimumValue > binding.maximumValue
                || binding.defaultValue < binding.minimumValue
                || binding.defaultValue > binding.maximumValue)
            {
                error = "Invalid range/default for Csound parameter binding: " + binding.name;
                return false;
            }

            for (size_t other = i + 1; other < bindings.size(); ++other)
                if (bindings[other].name == binding.name)
                {
                    error = "Duplicate Csound parameter binding name: " + binding.name;
                    return false;
                }
        }

        error.clear();
        return true;
    }

    static bool isValidParameterName (const juce::String& name) noexcept
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

    static bool audioSampleNeedsSanitising (float sample) noexcept
    {
        return ! std::isfinite (sample) || sample < -outputSafetyLimit || sample > outputSafetyLimit;
    }

    static bool controlValueNeedsSanitising (float value, float lower, float upper) noexcept
    {
        return ! std::isfinite (value) || value < lower || value > upper;
    }

    static float sanitiseAudioSample (float sample) noexcept
    {
        if (! std::isfinite (sample))
            return 0.0f;

        return juce::jlimit (-outputSafetyLimit, outputSafetyLimit, sample);
    }

    static float sanitiseControlValue (float value, float fallback, float lower, float upper) noexcept
    {
        if (! std::isfinite (value))
            return fallback;

        return juce::jlimit (lower, upper, value);
    }

    static void addLibraryCandidate (juce::StringArray& candidates, const juce::String& path)
    {
        if (path.isNotEmpty())
            candidates.addIfNotAlreadyThere (path);
    }

    static juce::File getBundleContentsDirectory()
    {
        const auto executableDirectory = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        if (executableDirectory.getFileName() == "MacOS")
            return executableDirectory.getParentDirectory();

        return {};
    }

    static void addBundledLibraryCandidate (juce::StringArray& candidates, const juce::String& fileName)
    {
        const auto contents = getBundleContentsDirectory();
        if (contents != juce::File())
            addLibraryCandidate (candidates, contents.getChildFile ("Frameworks").getChildFile (fileName).getFullPathName());
    }

    static void addBundledFrameworkCandidate (juce::StringArray& candidates, const juce::String& frameworkName, const juce::String& executableName)
    {
        const auto contents = getBundleContentsDirectory();
        if (contents != juce::File())
            addLibraryCandidate (candidates,
                                 contents.getChildFile ("Frameworks")
                                         .getChildFile (frameworkName)
                                         .getChildFile (executableName)
                                         .getFullPathName());
    }

    static juce::StringArray getCsoundLibraryCandidates()
    {
        juce::StringArray candidates;

        if (const auto* envPath = std::getenv ("WELD_CSOUND_LIBRARY"))
            addLibraryCandidate (candidates, envPath);

        addBundledFrameworkCandidate (candidates, "CsoundLib64.framework", "CsoundLib64");
        addBundledLibraryCandidate (candidates, "libcsound64.dylib");
        addBundledLibraryCandidate (candidates, "libcsound.dylib");
        addLibraryCandidate (candidates, WELD_CSOUND_DEFAULT_LIBRARY);
        addLibraryCandidate (candidates, "build-csound/CsoundLib64.framework/CsoundLib64");
        addLibraryCandidate (candidates, "build-csound/libcsound64.dylib");
        addLibraryCandidate (candidates, "build-csound/libcsound64.so");
        addLibraryCandidate (candidates, "/opt/homebrew/lib/libcsound64.dylib");
        addLibraryCandidate (candidates, "/opt/homebrew/lib/libcsound.dylib");
        addLibraryCandidate (candidates, "/usr/local/lib/libcsound64.dylib");
        addLibraryCandidate (candidates, "/usr/local/lib/libcsound.dylib");
        addLibraryCandidate (candidates, "/Library/Frameworks/CsoundLib64.framework/CsoundLib64");
        addLibraryCandidate (candidates, "libcsound64.dylib");
        addLibraryCandidate (candidates, "libcsound.dylib");
        addLibraryCandidate (candidates, "libcsound64.so");
        addLibraryCandidate (candidates, "libcsound.so");
        return candidates;
    }

    static juce::String getDefaultProgram()
    {
        return R"csound(
giWeldSine ftgen 1, 0, 4096, 10, 1

instr 1
    kfreq chnget "hostFreq"
    kgain chnget "hostGain"
    kblend chnget "hostBlend"
    aleft oscili kgain, kfreq, giWeldSine
    aright oscili kgain * (0.65 + (0.35 * kblend)), kfreq * (1.0 + (0.005 * kblend)), giWeldSine
    outs aleft, aright
endin
)csound";
    }

    void resetDiagnostics() noexcept
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

    mutable juce::CriticalSection engineLock;
    juce::String lastError;
    juce::String currentProgram;
    std::vector<EmbeddedLanguageEngine::ParameterBinding> currentBindings;
    std::array<std::atomic<float>, EmbeddedChucKEngine::maximumParameterCount> parameterValues;
    std::atomic<int> activeParameterCount { 0 };

    Api csoundApi;
    Instance* activeInstance = nullptr;
    double currentSampleRate = 0.0;
    int maxBlockSize = 0;
    int numInputChannels = 0;
    int numOutputChannels = 0;

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
#endif

#if WELD_HAS_RTCMIX
class RTcmixRuntime;

std::mutex& rtcmixGlobalMutex()
{
    static std::mutex mutex;
    return mutex;
}

RTcmixRuntime*& activeRtcmixRuntime()
{
    static RTcmixRuntime* runtime = nullptr;
    return runtime;
}

class RTcmixRuntime final : public EmbeddedLanguageEngine::Runtime
{
public:
    static constexpr int rtcmixRenderQuantum = 64;

    RTcmixRuntime()
    {
        programLoaderThread = std::thread ([this] { programLoaderLoop(); });
    }

    ~RTcmixRuntime() override
    {
        stopAsyncProgramLoader();
        release();
    }

    bool prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels) override
    {
        const juce::ScopedLock lock (engineLock);

        try
        {
            releaseUnlocked();
            resetDiagnostics();

            if (sampleRate <= 0.0 || ! std::isfinite (sampleRate))
            {
                lastError = "Invalid RTcmix audio sample rate";
                return false;
            }

            if (maximumBlockSize <= 0 || maximumBlockSize > EmbeddedChucKEngine::maximumBlockSizeLimit)
            {
                lastError = "Unsupported RTcmix audio block size";
                return false;
            }

            if (inputChannels < 0
                || inputChannels > EmbeddedChucKEngine::maximumChannelLimit
                || outputChannels <= 0
                || outputChannels > EmbeddedChucKEngine::maximumChannelLimit
                || inputChannels > outputChannels)
            {
                lastError = "Unsupported RTcmix audio channel count";
                return false;
            }

            if (! ensureApiLoaded())
            {
                releaseUnlocked();
                return false;
            }

            {
                std::lock_guard<std::mutex> globalLock (rtcmixGlobalMutex());
                if (activeRtcmixRuntime() != nullptr && activeRtcmixRuntime() != this)
                {
                    lastError = "RTcmix exposes a single embedded runtime; another RTcmix engine is already active";
                    return false;
                }

                activeRtcmixRuntime() = this;
                ownsGlobalInstance = true;
            }

            if (rtcmixApi.init() != 0)
            {
                lastError = "RTcmix failed to initialise";
                releaseUnlocked();
                return false;
            }

            currentSampleRate = sampleRate;
            maxBlockSize = maximumBlockSize;
            numInputChannels = inputChannels;
            numOutputChannels = outputChannels;
            numCallbackChannels = outputChannels;

            interleavedInput.assign (static_cast<size_t> (rtcmixRenderQuantum) * static_cast<size_t> (numCallbackChannels), 0.0f);
            interleavedOutput.assign (static_cast<size_t> (rtcmixRenderQuantum) * static_cast<size_t> (numCallbackChannels), 0.0f);
            pendingOutput.assign (static_cast<size_t> (rtcmixRenderQuantum) * static_cast<size_t> (numCallbackChannels), 0.0f);
            pendingOutputFrames = 0;
            pendingOutputOffset = 0;

            rtcmixApi.setPrintLevel (0);
            rtcmixApi.setInteractive (0);

            if (rtcmixApi.setAudioBufferFormat (AudioFormat_32BitFloat_Normalized, numCallbackChannels) != 0)
            {
                lastError = "RTcmix failed to accept the host audio buffer format";
                releaseUnlocked();
                return false;
            }

            if (rtcmixApi.setparams (static_cast<float> (currentSampleRate),
                                     numOutputChannels,
                                     rtcmixRenderQuantum,
                                     numInputChannels > 0 ? 1 : 0,
                                     juce::jmax (64, numOutputChannels)) != 0)
            {
                lastError = "RTcmix failed to configure embedded audio";
                releaseUnlocked();
                return false;
            }

            ready.store (true, std::memory_order_release);

            const auto bindings = EmbeddedChucKEngine::getDefaultParameterBindings();
            if (! loadProgramUnlocked (getDefaultProgram(), bindings, true))
            {
                releaseUnlocked();
                return false;
            }

            lastError.clear();
            return true;
        }
        catch (const std::exception& e)
        {
            lastError = juce::String ("RTcmix prepare exception: ") + e.what();
            releaseUnlocked();
            return false;
        }
        catch (...)
        {
            lastError = "Unknown RTcmix prepare exception";
            releaseUnlocked();
            return false;
        }
    }

    void release() noexcept override
    {
        const juce::ScopedLock lock (engineLock);
        releaseUnlocked();
    }

    bool loadProgram (const juce::String& programBody,
                      const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings) override
    {
        std::vector<EmbeddedLanguageEngine::ParameterBinding> normalisedBindings = bindings;
        juce::String validationError;

        if (! validateParameterBindings (normalisedBindings, validationError))
        {
            const juce::ScopedLock lock (engineLock);
            lastError = validationError;
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        const juce::ScopedLock lock (engineLock);
        if (! ready.load (std::memory_order_acquire) || ! ownsGlobalInstance)
        {
            lastError = "Cannot load an RTcmix score before the engine is prepared";
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        return loadProgramUnlocked (programBody, normalisedBindings, false);
    }

    bool loadProgramAsync (const juce::String& programBody,
                           const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings) override
    {
        std::vector<EmbeddedLanguageEngine::ParameterBinding> normalisedBindings = bindings;
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
            if (! ready.load (std::memory_order_acquire) || ! ownsGlobalInstance)
            {
                lastError = "Cannot queue an RTcmix score before the engine is prepared";
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
            const juce::ScopedLock lock (engineLock);
            lastError = "Cannot queue an RTcmix score after the loader has stopped";
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        programQueueCondition.notify_one();
        return true;
    }

    bool waitForAsyncProgramLoads (int timeoutMilliseconds) override
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

    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output) override
    {
        const juce::ScopedTryLock lock (engineLock);
        output.clear();

        if (! lock.isLocked() || ! ready.load (std::memory_order_acquire) || ! ownsGlobalInstance)
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

        uint64_t sanitisedInBlock = 0;
        const auto inputFrames = juce::jmin (frames, input.getNumSamples());
        auto outputFrame = 0;

        const auto copyRenderedFrames = [this, &output, &sanitisedInBlock] (const std::vector<float>& source,
                                                                            int sourceOffsetFrames,
                                                                            int outputOffsetFrames,
                                                                            int framesToCopy)
        {
            for (int channel = 0; channel < output.getNumChannels(); ++channel)
            {
                auto* dst = output.getWritePointer (channel, outputOffsetFrames);
                const auto sourceChannel = juce::jmin (channel, numCallbackChannels - 1);

                for (int frame = 0; frame < framesToCopy; ++frame)
                {
                    const auto sample = source[static_cast<size_t> ((sourceOffsetFrames + frame) * numCallbackChannels + sourceChannel)];
                    if (audioSampleNeedsSanitising (sample))
                        ++sanitisedInBlock;

                    dst[frame] = sanitiseAudioSample (sample);
                }
            }
        };

        try
        {
            if (pendingOutputFrames > 0)
            {
                const auto framesToCopy = juce::jmin (frames, pendingOutputFrames);
                copyRenderedFrames (pendingOutput, pendingOutputOffset, 0, framesToCopy);
                pendingOutputOffset += framesToCopy;
                pendingOutputFrames -= framesToCopy;
                outputFrame += framesToCopy;

                if (pendingOutputFrames == 0)
                    pendingOutputOffset = 0;
            }

            while (outputFrame < frames)
            {
                const auto framesNeeded = frames - outputFrame;
                const auto framesToConsumeNow = juce::jmin (framesNeeded, rtcmixRenderQuantum);
                const auto quantumSamples = static_cast<size_t> (rtcmixRenderQuantum) * static_cast<size_t> (numCallbackChannels);
                std::fill (interleavedInput.begin(), interleavedInput.begin() + static_cast<std::ptrdiff_t> (quantumSamples), 0.0f);
                std::fill (interleavedOutput.begin(), interleavedOutput.begin() + static_cast<std::ptrdiff_t> (quantumSamples), 0.0f);

                if (input.getNumChannels() > 0 && inputFrames > outputFrame && numInputChannels > 0)
                {
                    const auto inputFramesToCopy = juce::jmin (framesToConsumeNow, inputFrames - outputFrame);
                    for (int frame = 0; frame < inputFramesToCopy; ++frame)
                        for (int channel = 0; channel < numInputChannels; ++channel)
                        {
                            const auto sample = input.getSample (juce::jmin (channel, input.getNumChannels() - 1), outputFrame + frame);
                            if (audioSampleNeedsSanitising (sample))
                                ++sanitisedInBlock;

                            interleavedInput[static_cast<size_t> (frame * numCallbackChannels + channel)] = sanitiseAudioSample (sample);
                        }
                }

                pushPFields();

                if (rtcmixApi.runAudio (interleavedInput.data(), interleavedOutput.data(), rtcmixRenderQuantum) != 0)
                {
                    renderExceptionCount.fetch_add (1, std::memory_order_relaxed);
                    ready.store (false, std::memory_order_release);
                    output.clear();
                    return;
                }

                copyRenderedFrames (interleavedOutput, 0, outputFrame, framesToConsumeNow);
                outputFrame += framesToConsumeNow;

                if (framesToConsumeNow < rtcmixRenderQuantum)
                {
                    pendingOutput = interleavedOutput;
                    pendingOutputOffset = framesToConsumeNow;
                    pendingOutputFrames = rtcmixRenderQuantum - framesToConsumeNow;
                }
            }
        }
        catch (...)
        {
            renderExceptionCount.fetch_add (1, std::memory_order_relaxed);
            ready.store (false, std::memory_order_release);
            output.clear();
            return;
        }

        if (sanitisedInBlock != 0)
            sanitisedSampleCount.fetch_add (sanitisedInBlock, std::memory_order_relaxed);

        renderedBlockCount.fetch_add (1, std::memory_order_relaxed);
        renderedFrameCount.fetch_add (static_cast<uint64_t> (frames), std::memory_order_relaxed);
    }

    juce::String getCurrentProgram() const override
    {
        const juce::ScopedLock lock (engineLock);
        return currentProgram;
    }

    std::vector<EmbeddedLanguageEngine::ParameterBinding> getCurrentParameterBindings() const override
    {
        const juce::ScopedLock lock (engineLock);
        std::vector<EmbeddedLanguageEngine::ParameterBinding> bindings;
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

    bool setParameterValue (int index, float value) noexcept override
    {
        const auto count = activeParameterCount.load (std::memory_order_acquire);
        if (index < 0 || index >= count || index >= EmbeddedChucKEngine::maximumParameterCount)
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

    bool setParameterValue (const juce::String& name, float value) override
    {
        const juce::ScopedLock lock (engineLock);
        return setParameterValue (getParameterIndexUnlocked (name), value);
    }

    float getParameterValue (int index) const noexcept override
    {
        const auto count = activeParameterCount.load (std::memory_order_acquire);
        if (index < 0 || index >= count || index >= EmbeddedChucKEngine::maximumParameterCount)
            return 0.0f;

        return parameterSlots[static_cast<size_t> (index)].value.load (std::memory_order_relaxed);
    }

    int getParameterIndex (const juce::String& name) const override
    {
        const juce::ScopedLock lock (engineLock);
        return getParameterIndexUnlocked (name);
    }

    int getParameterCount() const noexcept override
    {
        return activeParameterCount.load (std::memory_order_acquire);
    }

    bool isReady() const noexcept override
    {
        return ready.load (std::memory_order_acquire);
    }

    juce::String getLastError() const override
    {
        const juce::ScopedLock lock (engineLock);
        return lastError;
    }

    uint64_t getSilentProcessCount() const noexcept override { return silentProcessCount.load (std::memory_order_relaxed); }
    uint64_t getOversizedBlockCount() const noexcept override { return oversizedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderExceptionCount() const noexcept override { return renderExceptionCount.load (std::memory_order_relaxed); }
    uint64_t getSanitisedSampleCount() const noexcept override { return sanitisedSampleCount.load (std::memory_order_relaxed); }
    uint64_t getSanitisedControlCount() const noexcept override { return sanitisedControlCount.load (std::memory_order_relaxed); }
    uint64_t getInternalErrorCount() const noexcept override { return internalErrorCount.load (std::memory_order_relaxed); }
    uint64_t getRenderedBlockCount() const noexcept override { return renderedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderedFrameCount() const noexcept override { return renderedFrameCount.load (std::memory_order_relaxed); }
    uint64_t getProgramLoadSuccessCount() const noexcept override { return programLoadSuccessCount.load (std::memory_order_relaxed); }
    uint64_t getProgramLoadFailureCount() const noexcept override { return programLoadFailureCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadQueuedCount() const noexcept override { return asyncProgramLoadQueuedCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadCompletedCount() const noexcept override { return asyncProgramLoadCompletedCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadDroppedCount() const noexcept override { return asyncProgramLoadDroppedCount.load (std::memory_order_relaxed); }
    bool isAsyncProgramLoadActive() const noexcept override { return asyncProgramLoadActive.load (std::memory_order_acquire); }

private:
    struct AsyncProgramLoadRequest
    {
        uint64_t requestId = 0;
        juce::String programBody;
        std::vector<EmbeddedLanguageEngine::ParameterBinding> bindings;
    };

    struct ParameterSlot
    {
        juce::String name;
        std::atomic<float> value { 0.0f };
        std::atomic<float> defaultValue { 0.0f };
        std::atomic<float> minimumValue { 0.0f };
        std::atomic<float> maximumValue { 1.0f };
        bool active = false;
    };

    struct Api
    {
        using SetPrintLevelFn = void (*) (int);
        using InitFn = int (*) ();
        using DestroyFn = int (*) ();
        using SetparamsFn = int (*) (float, int, int, int, int);
        using SetAudioBufferFormatFn = int (*) (RTcmix_AudioFormat, int);
        using SetInteractiveFn = void (*) (int);
        using RunAudioFn = int (*) (void*, void*, int);
        using ParseScoreFn = int (*) (char*, int);
        using FlushScoreFn = void (*) ();
        using SetPFieldFn = void (*) (int, float);

        bool isLoaded() const noexcept { return libraryHandle != nullptr; }

        void* libraryHandle = nullptr;
        juce::String loadedPath;
        SetPrintLevelFn setPrintLevel = nullptr;
        InitFn init = nullptr;
        DestroyFn destroy = nullptr;
        SetparamsFn setparams = nullptr;
        SetAudioBufferFormatFn setAudioBufferFormat = nullptr;
        SetInteractiveFn setInteractive = nullptr;
        RunAudioFn runAudio = nullptr;
        ParseScoreFn parseScore = nullptr;
        FlushScoreFn flushScore = nullptr;
        SetPFieldFn setPField = nullptr;
    };

    bool ensureApiLoaded()
    {
        if (rtcmixApi.isLoaded())
            return true;

        auto candidates = getRtcmixLibraryCandidates();
        juce::String loadErrors;

        for (const auto& candidate : candidates)
        {
            dlerror();
            auto* handle = dlopen (candidate.toRawUTF8(), RTLD_NOW | RTLD_LOCAL);

            if (handle == nullptr)
            {
                if (const auto* error = dlerror())
                    loadErrors << "\n" << candidate << ": " << error;
                else
                    loadErrors << "\n" << candidate << ": unknown loader error";

                continue;
            }

            rtcmixApi.libraryHandle = handle;
            rtcmixApi.loadedPath = candidate;

            juce::String symbolError;
            const auto symbolsLoaded =
                loadSymbol (rtcmixApi.setPrintLevel, "RTcmix_setPrintLevel", symbolError)
                && loadSymbol (rtcmixApi.init, "RTcmix_init", symbolError)
                && loadSymbol (rtcmixApi.destroy, "RTcmix_destroy", symbolError)
                && loadSymbol (rtcmixApi.setparams, "RTcmix_setparams", symbolError)
                && loadSymbol (rtcmixApi.setAudioBufferFormat, "RTcmix_setAudioBufferFormat", symbolError)
                && loadSymbol (rtcmixApi.setInteractive, "RTcmix_setInteractive", symbolError)
                && loadSymbol (rtcmixApi.runAudio, "RTcmix_runAudio", symbolError)
                && loadSymbol (rtcmixApi.parseScore, "RTcmix_parseScore", symbolError)
                && loadSymbol (rtcmixApi.flushScore, "RTcmix_flushScore", symbolError)
                && loadSymbol (rtcmixApi.setPField, "RTcmix_setPField", symbolError);

            if (symbolsLoaded)
                return true;

            loadErrors << "\n" << candidate << ": " << symbolError;
            unloadApi();
        }

        lastError = "Could not load the embedded RTcmix runtime library";
        if (loadErrors.isNotEmpty())
            lastError << ":" << loadErrors;

        return false;
    }

    template <typename Function>
    bool loadSymbol (Function& function, const char* symbolName, juce::String& error)
    {
        dlerror();
        function = reinterpret_cast<Function> (dlsym (rtcmixApi.libraryHandle, symbolName));

        if (function != nullptr)
            return true;

        error = juce::String ("missing symbol ") + symbolName;
        if (const auto* loaderError = dlerror())
            error << " (" << loaderError << ")";

        return false;
    }

    void unloadApi() noexcept
    {
        if (rtcmixApi.libraryHandle != nullptr)
            dlclose (rtcmixApi.libraryHandle);

        rtcmixApi = {};
    }

    static void addLibraryCandidate (juce::StringArray& candidates, const juce::String& path)
    {
        if (path.isNotEmpty())
            candidates.addIfNotAlreadyThere (path);
    }

    static juce::File getBundleContentsDirectory()
    {
        const auto executableDirectory = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        if (executableDirectory.getFileName() == "MacOS")
            return executableDirectory.getParentDirectory();

        return {};
    }

    static void addBundledLibraryCandidate (juce::StringArray& candidates, const juce::String& fileName)
    {
        const auto contents = getBundleContentsDirectory();
        if (contents != juce::File())
            addLibraryCandidate (candidates, contents.getChildFile ("Frameworks").getChildFile (fileName).getFullPathName());
    }

    static void addRootLibraryCandidates (juce::StringArray& candidates, const juce::File& root)
    {
        const auto sourceDirectory = root.getChildFile ("third_party")
                                         .getChildFile ("rtcmix")
                                         .getChildFile ("src")
                                         .getChildFile ("rtcmix");

        const auto libDirectory = root.getChildFile ("third_party")
                                      .getChildFile ("rtcmix")
                                      .getChildFile ("lib");

        addLibraryCandidate (candidates, sourceDirectory.getChildFile ("librtcmix_embedded.dylib").getFullPathName());
        addLibraryCandidate (candidates, sourceDirectory.getChildFile ("librtcmix_embedded.so").getFullPathName());
        addLibraryCandidate (candidates, libDirectory.getChildFile ("librtcmix_embedded.dylib").getFullPathName());
        addLibraryCandidate (candidates, libDirectory.getChildFile ("librtcmix_embedded.so").getFullPathName());
    }

    static juce::StringArray getRtcmixLibraryCandidates()
    {
        juce::StringArray candidates;

        if (const auto* envPath = std::getenv ("WELD_RTCMIX_LIBRARY"))
            addLibraryCandidate (candidates, envPath);

        addBundledLibraryCandidate (candidates, "librtcmix_embedded.dylib");
        addLibraryCandidate (candidates, WELD_RTCMIX_DEFAULT_LIBRARY);

        const auto addFromParents = [&candidates] (juce::File start)
        {
            if (start == juce::File())
                return;

            for (;;)
            {
                addRootLibraryCandidates (candidates, start);
                const auto parent = start.getParentDirectory();

                if (parent == start || parent == juce::File())
                    break;

                start = parent;
            }
        };

        addFromParents (juce::File::getCurrentWorkingDirectory());
        addFromParents (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
        addLibraryCandidate (candidates, "librtcmix_embedded.dylib");
        addLibraryCandidate (candidates, "librtcmix_embedded.so");

        return candidates;
    }

    static juce::String getDefaultProgram()
    {
        return R"rtcmix(
bus_config("WAVETABLE", "out 0")
freq = makeconnection("inlet", 1, 220)
gain = makeconnection("inlet", 2, 0.14)
pan = makeconnection("inlet", 3, 0.25)
wave = maketable("wave", 1024, 1, 0.35, 0.18)
WAVETABLE(0, 3600, gain * 32767.0, freq, pan, wave)
)rtcmix";
    }

    bool loadProgramUnlocked (const juce::String& programBody,
                              const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings,
                              bool preparing)
    {
        std::vector<float> values;
        values.reserve (bindings.size());

        for (const auto& binding : bindings)
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

        for (size_t i = 0; i < values.size(); ++i)
            rtcmixApi.setPField (static_cast<int> (i + 1), values[i]);

        rtcmixApi.flushScore();
        pendingOutputFrames = 0;
        pendingOutputOffset = 0;

        const auto score = programBody.toStdString();
        std::vector<char> scoreBuffer (score.begin(), score.end());
        scoreBuffer.push_back ('\0');

        try
        {
            if (rtcmixApi.parseScore (scoreBuffer.data(), static_cast<int> (score.size())) != 0)
            {
                rtcmixApi.flushScore();
                clearParameterSlots();
                currentProgram.clear();
                lastError = "RTcmix score did not parse; the active score was flushed";

                if (! preparing)
                    programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);

                return false;
            }
        }
        catch (const std::exception& e)
        {
            rtcmixApi.flushScore();
            clearParameterSlots();
            currentProgram.clear();
            lastError = juce::String ("RTcmix score load exception: ") + e.what();

            if (! preparing)
                programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);

            return false;
        }
        catch (...)
        {
            rtcmixApi.flushScore();
            clearParameterSlots();
            currentProgram.clear();
            lastError = "Unknown RTcmix score load exception";

            if (! preparing)
                programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);

            return false;
        }

        applyParameterSlots (bindings, values);
        pushPFields();
        currentProgram = programBody;
        programLoadSuccessCount.fetch_add (1, std::memory_order_relaxed);
        lastError.clear();
        return true;
    }

    void releaseUnlocked() noexcept
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

        if (ownsGlobalInstance)
        {
            try
            {
                if (rtcmixApi.flushScore != nullptr)
                    rtcmixApi.flushScore();

                if (rtcmixApi.destroy != nullptr)
                    rtcmixApi.destroy();
            }
            catch (...)
            {
            }

            std::lock_guard<std::mutex> globalLock (rtcmixGlobalMutex());
            if (activeRtcmixRuntime() == this)
                activeRtcmixRuntime() = nullptr;

            ownsGlobalInstance = false;
        }

        unloadApi();

        interleavedInput.clear();
        interleavedOutput.clear();
        pendingOutput.clear();
        pendingOutputFrames = 0;
        pendingOutputOffset = 0;
        clearParameterSlots();
        currentProgram.clear();
        currentSampleRate = 0.0;
        numInputChannels = 0;
        numOutputChannels = 0;
        numCallbackChannels = 0;
        maxBlockSize = 0;
    }

    void resetDiagnostics() noexcept
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

    void stopAsyncProgramLoader() noexcept
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

    void programLoaderLoop() noexcept
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

    void applyParameterSlots (const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings,
                              const std::vector<float>& values)
    {
        const auto count = juce::jmin (static_cast<int> (bindings.size()), EmbeddedChucKEngine::maximumParameterCount);
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
            slot.active = true;
        }

        activeParameterCount.store (count, std::memory_order_release);

        for (int i = count; i < EmbeddedChucKEngine::maximumParameterCount; ++i)
        {
            auto& slot = parameterSlots[static_cast<size_t> (i)];
            slot.name.clear();
            slot.value.store (0.0f, std::memory_order_relaxed);
            slot.defaultValue.store (0.0f, std::memory_order_relaxed);
            slot.minimumValue.store (0.0f, std::memory_order_relaxed);
            slot.maximumValue.store (1.0f, std::memory_order_relaxed);
            slot.active = false;
        }
    }

    void pushPFields()
    {
        const auto count = activeParameterCount.load (std::memory_order_relaxed);

        for (int i = 0; i < count; ++i)
        {
            auto& slot = parameterSlots[static_cast<size_t> (i)];
            if (slot.active)
                rtcmixApi.setPField (i + 1, slot.value.load (std::memory_order_relaxed));
        }
    }

    void clearParameterSlots() noexcept
    {
        activeParameterCount.store (0, std::memory_order_release);

        for (auto& slot : parameterSlots)
        {
            slot.name.clear();
            slot.value.store (0.0f, std::memory_order_relaxed);
            slot.defaultValue.store (0.0f, std::memory_order_relaxed);
            slot.minimumValue.store (0.0f, std::memory_order_relaxed);
            slot.maximumValue.store (1.0f, std::memory_order_relaxed);
            slot.active = false;
        }
    }

    bool preparedStateIsValidFor (int frames) const noexcept
    {
        if (frames < 0
            || maxBlockSize <= 0
            || frames > maxBlockSize
            || numInputChannels < 0
            || numInputChannels > numOutputChannels
            || numOutputChannels <= 0
            || numOutputChannels > EmbeddedChucKEngine::maximumChannelLimit
            || numCallbackChannels != numOutputChannels)
            return false;

        const auto count = activeParameterCount.load (std::memory_order_relaxed);
        if (count < 0 || count > EmbeddedChucKEngine::maximumParameterCount)
            return false;

        for (int i = 0; i < count; ++i)
            if (! parameterSlots[static_cast<size_t> (i)].active)
                return false;

        const auto quantumSamples = static_cast<size_t> (rtcmixRenderQuantum) * static_cast<size_t> (numCallbackChannels);
        return quantumSamples <= interleavedInput.size()
               && quantumSamples <= interleavedOutput.size()
               && quantumSamples <= pendingOutput.size()
               && pendingOutputFrames >= 0
               && pendingOutputFrames <= rtcmixRenderQuantum
               && pendingOutputOffset >= 0
               && pendingOutputOffset <= rtcmixRenderQuantum;
    }

    int getParameterIndexUnlocked (const juce::String& name) const
    {
        const auto count = activeParameterCount.load (std::memory_order_relaxed);
        for (int i = 0; i < count; ++i)
            if (parameterSlots[static_cast<size_t> (i)].name == name)
                return i;

        return -1;
    }

    static bool validateParameterBindings (const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings,
                                           juce::String& error)
    {
        if (bindings.size() > static_cast<size_t> (EmbeddedChucKEngine::maximumParameterCount))
        {
            error = "Too many RTcmix parameter bindings";
            return false;
        }

        for (size_t i = 0; i < bindings.size(); ++i)
        {
            const auto& binding = bindings[i];

            if (! isValidParameterName (binding.name))
            {
                error = "Invalid RTcmix parameter binding name: " + binding.name;
                return false;
            }

            if (! std::isfinite (binding.minimumValue)
                || ! std::isfinite (binding.maximumValue)
                || ! std::isfinite (binding.defaultValue)
                || binding.minimumValue > binding.maximumValue
                || binding.defaultValue < binding.minimumValue
                || binding.defaultValue > binding.maximumValue)
            {
                error = "Invalid range/default for RTcmix parameter binding: " + binding.name;
                return false;
            }

            for (size_t other = i + 1; other < bindings.size(); ++other)
                if (bindings[other].name == binding.name)
                {
                    error = "Duplicate RTcmix parameter binding name: " + binding.name;
                    return false;
                }
        }

        error.clear();
        return true;
    }

    static bool isValidParameterName (const juce::String& name) noexcept
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

    static bool audioSampleNeedsSanitising (float sample) noexcept
    {
        return ! std::isfinite (sample) || sample < -outputSafetyLimit || sample > outputSafetyLimit;
    }

    static bool controlValueNeedsSanitising (float value, float lower, float upper) noexcept
    {
        return ! std::isfinite (value) || value < lower || value > upper;
    }

    static float sanitiseAudioSample (float sample) noexcept
    {
        if (! std::isfinite (sample))
            return 0.0f;

        return juce::jlimit (-outputSafetyLimit, outputSafetyLimit, sample);
    }

    static float sanitiseControlValue (float value, float fallback, float lower, float upper) noexcept
    {
        if (! std::isfinite (value))
            return fallback;

        return juce::jlimit (lower, upper, value);
    }

    mutable juce::CriticalSection engineLock;
    juce::String lastError;
    juce::String currentProgram;

    std::vector<float> interleavedInput;
    std::vector<float> interleavedOutput;
    std::vector<float> pendingOutput;

    double currentSampleRate = 0.0;
    int numInputChannels = 0;
    int numOutputChannels = 0;
    int numCallbackChannels = 0;
    int maxBlockSize = 0;
    int pendingOutputFrames = 0;
    int pendingOutputOffset = 0;
    bool ownsGlobalInstance = false;
    Api rtcmixApi;

    std::array<ParameterSlot, EmbeddedChucKEngine::maximumParameterCount> parameterSlots;
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
#endif

#if WELD_HAS_SUPERCOLLIDER
class SuperColliderRuntime final : public EmbeddedLanguageEngine::Runtime
{
public:
    static constexpr int hostRenderQuantum = 64;

    ~SuperColliderRuntime() override
    {
        release();
    }

    bool prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels) override
    {
        const juce::ScopedLock lock (engineLock);

        releaseUnlocked();
        resetDiagnostics();

        if (sampleRate <= 0.0 || ! std::isfinite (sampleRate))
        {
            lastError = "Invalid SuperCollider audio sample rate";
            return false;
        }

        if (maximumBlockSize <= 0 || maximumBlockSize > EmbeddedChucKEngine::maximumBlockSizeLimit)
        {
            lastError = "Unsupported SuperCollider audio block size";
            return false;
        }

        if (inputChannels < 0
            || inputChannels > EmbeddedChucKEngine::maximumChannelLimit
            || outputChannels <= 0
            || outputChannels > EmbeddedChucKEngine::maximumChannelLimit)
        {
            lastError = "Unsupported SuperCollider audio channel count";
            return false;
        }

        if (! ensureApiLoaded())
            return false;

        if (! ensureLanguageApiLoaded())
            return false;

        currentSampleRate = sampleRate;
        maxBlockSize = maximumBlockSize;
        numInputChannels = inputChannels;
        numOutputChannels = outputChannels;
        interleavedInput.assign (static_cast<size_t> (maxBlockSize + hostRenderQuantum) * static_cast<size_t> (juce::jmax (1, inputChannels)), 0.0f);
        interleavedOutput.assign (static_cast<size_t> (maxBlockSize + hostRenderQuantum) * static_cast<size_t> (outputChannels), 0.0f);
        sharedControls.assign (static_cast<size_t> (EmbeddedChucKEngine::maximumParameterCount), 0.0f);

        worldOptions = WorldOptions();
        worldOptions.mRealTime = true;
        worldOptions.mRendezvous = false;
        worldOptions.mVerbosity = -1;
        worldOptions.mLoadGraphDefs = 0;
        worldOptions.mPreferredSampleRate = static_cast<uint32> (sampleRate);
        worldOptions.mPreferredHardwareBufferFrameSize = hostRenderQuantum;
        worldOptions.mBufLength = hostRenderQuantum;
        worldOptions.mNumInputBusChannels = static_cast<uint32> (inputChannels);
        worldOptions.mNumOutputBusChannels = static_cast<uint32> (outputChannels);
        worldOptions.mNumAudioBusChannels = static_cast<uint32> (juce::jmax (1024, inputChannels + outputChannels + 16));
        worldOptions.mNumSharedControls = static_cast<int> (sharedControls.size());
        worldOptions.mSharedControls = sharedControls.data();
        pluginPath = getSuperColliderPluginPath();
        worldOptions.mUGensPluginPath = pluginPath.isNotEmpty() ? pluginPath.toRawUTF8() : nullptr;

        world = scApi.worldNew (&worldOptions);
        if (world == nullptr)
        {
            lastError = "SuperCollider failed to create an embedded world";
            releaseUnlocked();
            return false;
        }

        applyParameterSlots (EmbeddedChucKEngine::getDefaultParameterBindings());
        currentProgram = getDefaultProgram();
        ready.store (true, std::memory_order_release);
        programLoadSuccessCount.fetch_add (1, std::memory_order_relaxed);
        lastError.clear();
        return true;
    }

    void release() noexcept override
    {
        const juce::ScopedLock lock (engineLock);
        releaseUnlocked();
    }

    bool loadProgram (const juce::String& programBody,
                      const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings) override
    {
        const juce::ScopedLock lock (engineLock);

        if (! ready.load (std::memory_order_acquire) || world == nullptr)
        {
            lastError = "Cannot load a SuperCollider program before the engine is prepared";
            programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        applyParameterSlots (bindings);

        const auto trimmed = programBody.trim();
        if (trimmed.isNotEmpty() && trimmed != getDefaultProgram())
        {
            std::vector<char> packet;
            juce::String error;
            if (trimmed.startsWithIgnoreCase ("osc-hex:"))
            {
                if (! decodeOscHexProgram (trimmed, packet, error))
                {
                    lastError = error;
                    programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
                    return false;
                }

                if (! scApi.worldSendPacket (world, static_cast<int> (packet.size()), packet.data(), nullptr))
                {
                    lastError = "SuperCollider rejected the OSC packet program";
                    programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
                    return false;
                }
            }
            else
            {
                std::vector<char> synthDefBytes;
                if (! compileSourceToSynthDefBytes (trimmed, synthDefBytes, error))
                {
                    lastError = error;
                    programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
                    return false;
                }

                auto sNew = buildSynthNewMessage();
                auto dRecv = buildOscMessage ("/d_recv", { OscArgument::blob (synthDefBytes),
                                                           OscArgument::blob (sNew) });
                if (! scApi.worldSendPacket (world, static_cast<int> (dRecv.size()), dRecv.data(), nullptr))
                {
                    lastError = "SuperCollider rejected the compiled SynthDef";
                    programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
                    return false;
                }
            }
        }

        currentProgram = programBody;
        programLoadSuccessCount.fetch_add (1, std::memory_order_relaxed);
        lastError.clear();
        return true;
    }

    bool loadProgramAsync (const juce::String& programBody,
                           const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings) override
    {
        const auto ok = loadProgram (programBody, bindings);
        asyncProgramLoadQueuedCount.fetch_add (1, std::memory_order_relaxed);
        asyncProgramLoadCompletedCount.fetch_add (1, std::memory_order_relaxed);
        return ok;
    }

    bool waitForAsyncProgramLoads (int) override { return true; }

    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output) override
    {
        const juce::ScopedTryLock lock (engineLock);
        output.clear();

        if (! lock.isLocked() || ! ready.load (std::memory_order_acquire) || world == nullptr)
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

        const auto inputChannels = juce::jmax (1, numInputChannels);
        const auto paddedFrames = ((frames + hostRenderQuantum - 1) / hostRenderQuantum) * hostRenderQuantum;
        std::fill (interleavedInput.begin(), interleavedInput.begin() + paddedFrames * inputChannels, 0.0f);
        std::fill (interleavedOutput.begin(), interleavedOutput.begin() + paddedFrames * numOutputChannels, 0.0f);

        uint64_t sanitisedInBlock = 0;
        const auto inputFrames = juce::jmin (frames, input.getNumSamples());

        for (int frame = 0; frame < inputFrames; ++frame)
            for (int channel = 0; channel < numInputChannels; ++channel)
            {
                const auto sample = input.getSample (juce::jmin (channel, input.getNumChannels() - 1), frame);
                if (audioSampleNeedsSanitising (sample))
                    ++sanitisedInBlock;

                interleavedInput[static_cast<size_t> (frame * inputChannels + channel)] = sanitiseAudioSample (sample);
            }

        pushSharedControls();

        if (! scApi.worldRenderHostAudio (world,
                                          numInputChannels > 0 ? interleavedInput.data() : nullptr,
                                          interleavedOutput.data(),
                                          paddedFrames,
                                          numInputChannels,
                                          numOutputChannels))
        {
            renderExceptionCount.fetch_add (1, std::memory_order_relaxed);
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

    juce::String getCurrentProgram() const override
    {
        const juce::ScopedLock lock (engineLock);
        return currentProgram;
    }

    std::vector<EmbeddedLanguageEngine::ParameterBinding> getCurrentParameterBindings() const override
    {
        const juce::ScopedLock lock (engineLock);
        return currentBindings;
    }

    bool setParameterValue (int index, float value) noexcept override
    {
        if (index < 0 || index >= activeParameterCount.load (std::memory_order_acquire))
            return false;

        const auto& binding = currentBindings[static_cast<size_t> (index)];
        if (! std::isfinite (value) || value < binding.minimumValue || value > binding.maximumValue)
            sanitisedControlCount.fetch_add (1, std::memory_order_relaxed);

        parameterValues[static_cast<size_t> (index)].store (juce::jlimit (binding.minimumValue, binding.maximumValue, std::isfinite (value) ? value : binding.defaultValue),
                                                            std::memory_order_relaxed);
        return true;
    }

    bool setParameterValue (const juce::String& name, float value) override
    {
        return setParameterValue (getParameterIndex (name), value);
    }

    float getParameterValue (int index) const noexcept override
    {
        if (index < 0 || index >= activeParameterCount.load (std::memory_order_acquire))
            return 0.0f;

        return parameterValues[static_cast<size_t> (index)].load (std::memory_order_relaxed);
    }

    int getParameterIndex (const juce::String& name) const override
    {
        const juce::ScopedLock lock (engineLock);
        for (size_t i = 0; i < currentBindings.size(); ++i)
            if (currentBindings[i].name == name)
                return static_cast<int> (i);

        return -1;
    }

    int getParameterCount() const noexcept override { return activeParameterCount.load (std::memory_order_acquire); }
    bool isReady() const noexcept override { return ready.load (std::memory_order_acquire); }

    juce::String getLastError() const override
    {
        const juce::ScopedLock lock (engineLock);
        return lastError;
    }

    uint64_t getSilentProcessCount() const noexcept override { return silentProcessCount.load (std::memory_order_relaxed); }
    uint64_t getOversizedBlockCount() const noexcept override { return oversizedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderExceptionCount() const noexcept override { return renderExceptionCount.load (std::memory_order_relaxed); }
    uint64_t getSanitisedSampleCount() const noexcept override { return sanitisedSampleCount.load (std::memory_order_relaxed); }
    uint64_t getSanitisedControlCount() const noexcept override { return sanitisedControlCount.load (std::memory_order_relaxed); }
    uint64_t getInternalErrorCount() const noexcept override { return internalErrorCount.load (std::memory_order_relaxed); }
    uint64_t getRenderedBlockCount() const noexcept override { return renderedBlockCount.load (std::memory_order_relaxed); }
    uint64_t getRenderedFrameCount() const noexcept override { return renderedFrameCount.load (std::memory_order_relaxed); }
    uint64_t getProgramLoadSuccessCount() const noexcept override { return programLoadSuccessCount.load (std::memory_order_relaxed); }
    uint64_t getProgramLoadFailureCount() const noexcept override { return programLoadFailureCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadQueuedCount() const noexcept override { return asyncProgramLoadQueuedCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadCompletedCount() const noexcept override { return asyncProgramLoadCompletedCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadDroppedCount() const noexcept override { return 0; }
    bool isAsyncProgramLoadActive() const noexcept override { return false; }

private:
    struct Api
    {
        using WorldNewFn = World* (*) (WorldOptions*);
        using WorldCleanupFn = void (*) (World*, bool);
        using WorldSendPacketFn = bool (*) (World*, int, char*, ReplyFunc);
        using WorldRenderHostAudioFn = bool (*) (World*, const float*, float*, int, int, int);

        bool isLoaded() const noexcept { return libraryHandle != nullptr; }

        void* libraryHandle = nullptr;
        WorldNewFn worldNew = nullptr;
        WorldCleanupFn worldCleanup = nullptr;
        WorldSendPacketFn worldSendPacket = nullptr;
        WorldRenderHostAudioFn worldRenderHostAudio = nullptr;
    };

    struct LanguageApi
    {
        using InitialiseFn = bool (*) (const char*, char*, int);
        using ShutdownFn = void (*) ();
        using CompileSynthDefFn = bool (*) (const char*, const char*, const char*, char*, int);

        bool isLoaded() const noexcept { return libraryHandle != nullptr; }

        void* libraryHandle = nullptr;
        InitialiseFn initialise = nullptr;
        ShutdownFn shutdown = nullptr;
        CompileSynthDefFn compileSynthDef = nullptr;
    };

    bool ensureApiLoaded()
    {
        if (scApi.isLoaded())
            return true;

        juce::String loadErrors;
        for (const auto& candidate : getSuperColliderLibraryCandidates())
        {
            dlerror();
            auto* handle = dlopen (candidate.toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr)
            {
                if (const auto* error = dlerror())
                    loadErrors << "\n" << candidate << ": " << error;
                continue;
            }

            scApi.libraryHandle = handle;
            juce::String symbolError;
            const auto symbolsLoaded = loadSymbol (scApi.worldNew, "World_New", symbolError)
                                       && loadSymbol (scApi.worldCleanup, "World_Cleanup", symbolError)
                                       && loadSymbol (scApi.worldSendPacket, "World_SendPacket", symbolError)
                                       && loadSymbol (scApi.worldRenderHostAudio, "World_RenderHostAudio", symbolError);

            if (symbolsLoaded)
                return true;

            loadErrors << "\n" << candidate << ": " << symbolError;
            unloadApi();
        }

        lastError = "Could not load the embedded SuperCollider host-audio runtime library";
        if (loadErrors.isNotEmpty())
            lastError << ":" << loadErrors;

        return false;
    }

    template <typename Function>
    bool loadSymbol (Function& function, const char* symbolName, juce::String& error)
    {
        dlerror();
        function = reinterpret_cast<Function> (dlsym (scApi.libraryHandle, symbolName));
        if (function != nullptr)
            return true;

        error = juce::String ("missing symbol ") + symbolName;
        if (const auto* loaderError = dlerror())
            error << " (" << loaderError << ")";

        return false;
    }

    template <typename Function>
    static bool loadSymbolFrom (void* libraryHandle, Function& function, const char* symbolName, juce::String& error)
    {
        dlerror();
        function = reinterpret_cast<Function> (dlsym (libraryHandle, symbolName));
        if (function != nullptr)
            return true;

        error = juce::String ("missing symbol ") + symbolName;
        if (const auto* loaderError = dlerror())
            error << " (" << loaderError << ")";

        return false;
    }

    bool ensureLanguageApiLoaded()
    {
        if (langApi.isLoaded())
            return true;

        juce::String loadErrors;
        for (const auto& candidate : getSuperColliderLanguageLibraryCandidates())
        {
            dlerror();
            auto* handle = dlopen (candidate.toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr)
            {
                if (const auto* error = dlerror())
                    loadErrors << "\n" << candidate << ": " << error;
                continue;
            }

            langApi.libraryHandle = handle;
            juce::String symbolError;
            const auto symbolsLoaded = loadSymbolFrom (handle, langApi.initialise, "WeldSCLang_Initialise", symbolError)
                                       && loadSymbolFrom (handle, langApi.shutdown, "WeldSCLang_Shutdown", symbolError)
                                       && loadSymbolFrom (handle, langApi.compileSynthDef, "WeldSCLang_CompileSynthDef", symbolError);

            if (symbolsLoaded)
            {
                std::array<char, 4096> errorBuffer {};
                const auto runtimeRoot = getSuperColliderSourceRoot();
                if (langApi.initialise (runtimeRoot.toRawUTF8(), errorBuffer.data(), static_cast<int> (errorBuffer.size())))
                    return true;

                loadErrors << "\n" << candidate << ": " << errorBuffer.data();
            }
            else
            {
                loadErrors << "\n" << candidate << ": " << symbolError;
            }

            unloadLanguageApi();
        }

        lastError = "Could not load the embedded SuperCollider language compiler";
        if (loadErrors.isNotEmpty())
            lastError << ":" << loadErrors;

        return false;
    }

    void releaseUnlocked() noexcept
    {
        ready.store (false, std::memory_order_release);

        if (world != nullptr && scApi.worldCleanup != nullptr)
        {
            try { scApi.worldCleanup (world, false); } catch (...) {}
        }

        world = nullptr;
        unloadApi();
        unloadLanguageApi();
        currentProgram.clear();
        currentBindings.clear();
        activeParameterCount.store (0, std::memory_order_release);
        sharedControls.clear();
        interleavedInput.clear();
        interleavedOutput.clear();
        currentSampleRate = 0.0;
        maxBlockSize = 0;
        numInputChannels = 0;
        numOutputChannels = 0;
    }

    void unloadApi() noexcept
    {
        if (scApi.libraryHandle != nullptr)
            dlclose (scApi.libraryHandle);

        scApi = {};
    }

    void unloadLanguageApi() noexcept
    {
        if (langApi.shutdown != nullptr)
        {
            try { langApi.shutdown(); } catch (...) {}
        }

        if (langApi.libraryHandle != nullptr)
            dlclose (langApi.libraryHandle);

        langApi = {};
    }

    bool compileSourceToSynthDefBytes (const juce::String& source, std::vector<char>& synthDefBytes, juce::String& error)
    {
        if (! langApi.isLoaded() || langApi.compileSynthDef == nullptr)
        {
            error = "SuperCollider language compiler is not loaded";
            return false;
        }

        const auto tempFile = juce::File::createTempFile ("weld-sc.scsyndef");
        tempFile.deleteFile();

        std::array<char, 8192> errorBuffer {};
        if (! langApi.compileSynthDef (source.toRawUTF8(),
                                       tempFile.getFullPathName().toRawUTF8(),
                                       defaultSynthName,
                                       errorBuffer.data(),
                                       static_cast<int> (errorBuffer.size())))
        {
            error = juce::String ("SuperCollider source did not compile: ") + juce::String (errorBuffer.data());
            tempFile.deleteFile();
            return false;
        }

        std::ifstream stream (tempFile.getFullPathName().toStdString(), std::ios::binary);
        if (! stream)
        {
            error = "SuperCollider compiler did not produce SynthDef bytes";
            tempFile.deleteFile();
            return false;
        }

        synthDefBytes.assign (std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char>());
        tempFile.deleteFile();

        if (synthDefBytes.empty())
        {
            error = "SuperCollider compiler produced an empty SynthDef";
            return false;
        }

        error.clear();
        return true;
    }

    void applyParameterSlots (const std::vector<EmbeddedLanguageEngine::ParameterBinding>& bindings)
    {
        currentBindings = bindings;
        if (currentBindings.size() > static_cast<size_t> (EmbeddedChucKEngine::maximumParameterCount))
            currentBindings.resize (EmbeddedChucKEngine::maximumParameterCount);

        for (size_t i = 0; i < currentBindings.size(); ++i)
        {
            const auto& binding = currentBindings[i];
            parameterValues[i].store (juce::jlimit (binding.minimumValue, binding.maximumValue, binding.defaultValue),
                                      std::memory_order_relaxed);
        }

        activeParameterCount.store (static_cast<int> (currentBindings.size()), std::memory_order_release);
        pushSharedControls();
    }

    void pushSharedControls()
    {
        const auto count = juce::jmin (sharedControls.size(), static_cast<size_t> (activeParameterCount.load (std::memory_order_acquire)));
        for (size_t i = 0; i < count; ++i)
            sharedControls[i] = parameterValues[i].load (std::memory_order_relaxed);
    }

    int getParameterIndexUnlocked (const juce::String& name) const
    {
        for (size_t i = 0; i < currentBindings.size(); ++i)
            if (currentBindings[i].name == name)
                return static_cast<int> (i);

        return -1;
    }

    static bool decodeOscHexProgram (const juce::String& programBody, std::vector<char>& packet, juce::String& error)
    {
        auto hex = programBody;
        if (hex.startsWithIgnoreCase ("osc-hex:"))
            hex = hex.fromFirstOccurrenceOf (":", false, false);

        hex = hex.retainCharacters ("0123456789abcdefABCDEF");
        if (hex.length() < 2 || (hex.length() % 2) != 0)
        {
            error = "SuperCollider program loading accepts empty text or osc-hex:<packet-bytes>; sclang source is intentionally not evaluated in-process";
            return false;
        }

        packet.clear();
        packet.reserve (static_cast<size_t> (hex.length() / 2));

        for (int i = 0; i < hex.length(); i += 2)
            packet.push_back (static_cast<char> (hex.substring (i, i + 2).getHexValue32()));

        error.clear();
        return true;
    }

    struct OscArgument
    {
        enum class Type { integer, floating, string, blob };

        static OscArgument integer (int32_t value) { return { Type::integer, value, 0.0f, {}, {} }; }
        static OscArgument floating (float value) { return { Type::floating, 0, value, {}, {} }; }
        static OscArgument string (juce::String value) { return { Type::string, 0, 0.0f, std::move (value), {} }; }
        static OscArgument blob (std::vector<char> value) { return { Type::blob, 0, 0.0f, {}, std::move (value) }; }

        Type type;
        int32_t intValue = 0;
        float floatValue = 0.0f;
        juce::String stringValue;
        std::vector<char> blobValue;
    };

    static void appendPaddedString (std::vector<char>& packet, const juce::String& value)
    {
        const auto utf8 = value.toStdString();
        packet.insert (packet.end(), utf8.begin(), utf8.end());
        packet.push_back ('\0');

        while ((packet.size() % 4) != 0)
            packet.push_back ('\0');
    }

    static void appendInt32 (std::vector<char>& packet, int32_t value)
    {
        packet.push_back (static_cast<char> ((value >> 24) & 0xff));
        packet.push_back (static_cast<char> ((value >> 16) & 0xff));
        packet.push_back (static_cast<char> ((value >> 8) & 0xff));
        packet.push_back (static_cast<char> (value & 0xff));
    }

    static void appendFloat32 (std::vector<char>& packet, float value)
    {
        static_assert (sizeof (float) == sizeof (uint32_t), "Unexpected float size");
        uint32_t bits = 0;
        std::memcpy (&bits, &value, sizeof (bits));
        appendInt32 (packet, static_cast<int32_t> (bits));
    }

    static void appendBlob (std::vector<char>& packet, const std::vector<char>& blob)
    {
        appendInt32 (packet, static_cast<int32_t> (blob.size()));
        packet.insert (packet.end(), blob.begin(), blob.end());

        while ((packet.size() % 4) != 0)
            packet.push_back ('\0');
    }

    static std::vector<char> buildOscMessage (const juce::String& address, const std::vector<OscArgument>& arguments)
    {
        std::vector<char> packet;
        appendPaddedString (packet, address);

        juce::String typeTags = ",";
        for (const auto& argument : arguments)
        {
            switch (argument.type)
            {
                case OscArgument::Type::integer:  typeTags << "i"; break;
                case OscArgument::Type::floating: typeTags << "f"; break;
                case OscArgument::Type::string:   typeTags << "s"; break;
                case OscArgument::Type::blob:     typeTags << "b"; break;
            }
        }

        appendPaddedString (packet, typeTags);

        for (const auto& argument : arguments)
        {
            switch (argument.type)
            {
                case OscArgument::Type::integer:  appendInt32 (packet, argument.intValue); break;
                case OscArgument::Type::floating: appendFloat32 (packet, argument.floatValue); break;
                case OscArgument::Type::string:   appendPaddedString (packet, argument.stringValue); break;
                case OscArgument::Type::blob:     appendBlob (packet, argument.blobValue); break;
            }
        }

        return packet;
    }

    std::vector<char> buildSynthNewMessage() const
    {
        std::vector<OscArgument> arguments;
        arguments.push_back (OscArgument::string (defaultSynthName));
        arguments.push_back (OscArgument::integer (defaultSynthNodeId));
        arguments.push_back (OscArgument::integer (0));
        arguments.push_back (OscArgument::integer (0));

        const auto addControl = [&arguments] (const char* name, float value)
        {
            arguments.push_back (OscArgument::string (name));
            arguments.push_back (OscArgument::floating (value));
        };

        addControl ("out", 0.0f);

        return buildOscMessage ("/s_new", arguments);
    }

    static void addLibraryCandidate (juce::StringArray& candidates, const juce::String& path)
    {
        if (path.isNotEmpty())
            candidates.addIfNotAlreadyThere (path);
    }

    static juce::File getBundleContentsDirectory()
    {
        const auto executableDirectory = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        if (executableDirectory.getFileName() == "MacOS")
            return executableDirectory.getParentDirectory();

        return {};
    }

    static void addBundledLibraryCandidate (juce::StringArray& candidates, const juce::String& fileName)
    {
        const auto contents = getBundleContentsDirectory();
        if (contents != juce::File())
            addLibraryCandidate (candidates, contents.getChildFile ("Frameworks").getChildFile (fileName).getFullPathName());
    }

    static juce::StringArray getSuperColliderLibraryCandidates()
    {
        juce::StringArray candidates;

        if (const auto* envPath = std::getenv ("WELD_SUPERCOLLIDER_LIBRARY"))
            addLibraryCandidate (candidates, envPath);

        addBundledLibraryCandidate (candidates, "libscsynth.dylib");
        addLibraryCandidate (candidates, WELD_SUPERCOLLIDER_DEFAULT_LIBRARY);

        const auto addFromParents = [&candidates] (juce::File start)
        {
            for (;;)
            {
                addLibraryCandidate (candidates, start.getChildFile ("build-supercollider-host")
                                                    .getChildFile ("server")
                                                    .getChildFile ("scsynth")
                                                    .getChildFile ("libscsynth.dylib")
                                                    .getFullPathName());
                addLibraryCandidate (candidates, start.getChildFile ("build-supercollider-host")
                                                    .getChildFile ("server")
                                                    .getChildFile ("scsynth")
                                                    .getChildFile ("libscsynth.so")
                                                    .getFullPathName());

                const auto parent = start.getParentDirectory();
                if (parent == start || parent == juce::File())
                    break;

                start = parent;
            }
        };

        addFromParents (juce::File::getCurrentWorkingDirectory());
        addFromParents (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
        addLibraryCandidate (candidates, "libscsynth.dylib");
        addLibraryCandidate (candidates, "libscsynth.so");
        return candidates;
    }

    static juce::StringArray getSuperColliderLanguageLibraryCandidates()
    {
        juce::StringArray candidates;

        if (const auto* envPath = std::getenv ("WELD_SUPERCOLLIDER_LANG_LIBRARY"))
            addLibraryCandidate (candidates, envPath);

        addBundledLibraryCandidate (candidates, "libweldsclang.dylib");
        addLibraryCandidate (candidates, WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY);

        const auto addFromParents = [&candidates] (juce::File start)
        {
            for (;;)
            {
                addLibraryCandidate (candidates, start.getChildFile ("build-supercollider-host")
                                                    .getChildFile ("lang")
                                                    .getChildFile ("libweldsclang.dylib")
                                                    .getFullPathName());
                addLibraryCandidate (candidates, start.getChildFile ("build-supercollider-host")
                                                    .getChildFile ("lang")
                                                    .getChildFile ("libweldsclang.so")
                                                    .getFullPathName());

                const auto parent = start.getParentDirectory();
                if (parent == start || parent == juce::File())
                    break;

                start = parent;
            }
        };

        addFromParents (juce::File::getCurrentWorkingDirectory());
        addFromParents (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
        addLibraryCandidate (candidates, "libweldsclang.dylib");
        addLibraryCandidate (candidates, "libweldsclang.so");
        return candidates;
    }

    static juce::String getSuperColliderSourceRoot()
    {
        if (const auto* envPath = std::getenv ("WELD_SUPERCOLLIDER_ROOT"))
            return envPath;

        return juce::File::getCurrentWorkingDirectory()
            .getChildFile ("third_party")
            .getChildFile ("supercollider")
            .getFullPathName();
    }

    static juce::String getSuperColliderPluginPath()
    {
        if (const auto* envPath = std::getenv ("WELD_SUPERCOLLIDER_PLUGIN_PATH"))
            return envPath;

        const auto contents = getBundleContentsDirectory();
        if (contents != juce::File())
        {
            const auto bundledPlugins = contents.getChildFile ("Resources")
                                         .getChildFile ("SuperCollider")
                                         .getChildFile ("plugins");
            if (bundledPlugins.isDirectory())
                return bundledPlugins.getFullPathName();
        }

        auto start = juce::File::getCurrentWorkingDirectory();
        for (;;)
        {
            const auto candidate = start.getChildFile ("build-supercollider-host")
                                  .getChildFile ("server")
                                  .getChildFile ("plugins");
            if (candidate.isDirectory())
                return candidate.getFullPathName();

            const auto parent = start.getParentDirectory();
            if (parent == start || parent == juce::File())
                break;

            start = parent;
        }

        return {};
    }

    static juce::String getDefaultProgram()
    {
        return {};
    }

    static bool audioSampleNeedsSanitising (float sample) noexcept
    {
        return ! std::isfinite (sample) || sample < -outputSafetyLimit || sample > outputSafetyLimit;
    }

    static float sanitiseAudioSample (float sample) noexcept
    {
        if (! std::isfinite (sample))
            return 0.0f;

        return juce::jlimit (-outputSafetyLimit, outputSafetyLimit, sample);
    }

    void resetDiagnostics() noexcept
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
    }

    mutable juce::CriticalSection engineLock;
    juce::String lastError;
    juce::String currentProgram;
    juce::String pluginPath;
    std::vector<EmbeddedLanguageEngine::ParameterBinding> currentBindings;
    std::array<std::atomic<float>, EmbeddedChucKEngine::maximumParameterCount> parameterValues;
    std::atomic<int> activeParameterCount { 0 };
    std::vector<float> sharedControls;
    std::vector<float> interleavedInput;
    std::vector<float> interleavedOutput;
    WorldOptions worldOptions;
    World* world = nullptr;
    Api scApi;
    LanguageApi langApi;
    double currentSampleRate = 0.0;
    int maxBlockSize = 0;
    int numInputChannels = 0;
    int numOutputChannels = 0;

    static constexpr const char* defaultSynthName = "weldMain";
    static constexpr int32_t defaultSynthNodeId = 1001;
    static constexpr float outputSafetyLimit = 0.98f;
    std::atomic<bool> ready { false };
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
};
#endif

class UnavailableRuntime final : public EmbeddedLanguageEngine::Runtime
{
public:
    explicit UnavailableRuntime (EmbeddedLanguageEngine::Language languageToReport)
        : language (languageToReport)
    {
        setUnavailableError();
    }

    bool prepare (double, int, int, int) override
    {
        setUnavailableError();
        return false;
    }

    void release() noexcept override
    {
        ready.store (false, std::memory_order_release);
    }

    bool loadProgram (const juce::String&, const std::vector<EmbeddedLanguageEngine::ParameterBinding>&) override
    {
        programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
        setUnavailableError();
        return false;
    }

    bool loadProgramAsync (const juce::String&, const std::vector<EmbeddedLanguageEngine::ParameterBinding>&) override
    {
        programLoadFailureCount.fetch_add (1, std::memory_order_relaxed);
        setUnavailableError();
        return false;
    }

    bool waitForAsyncProgramLoads (int) override
    {
        return true;
    }

    void process (const juce::AudioBuffer<float>&, juce::AudioBuffer<float>& output) override
    {
        output.clear();
        silentProcessCount.fetch_add (1, std::memory_order_relaxed);
    }

    juce::String getCurrentProgram() const override { return {}; }
    std::vector<EmbeddedLanguageEngine::ParameterBinding> getCurrentParameterBindings() const override { return {}; }
    bool setParameterValue (int, float) noexcept override { return false; }
    bool setParameterValue (const juce::String&, float) override { return false; }
    float getParameterValue (int) const noexcept override { return 0.0f; }
    int getParameterIndex (const juce::String&) const override { return -1; }
    int getParameterCount() const noexcept override { return 0; }
    bool isReady() const noexcept override { return ready.load (std::memory_order_acquire); }

    juce::String getLastError() const override
    {
        const juce::ScopedLock lock (errorLock);
        return lastError;
    }

    uint64_t getSilentProcessCount() const noexcept override { return silentProcessCount.load (std::memory_order_relaxed); }
    uint64_t getOversizedBlockCount() const noexcept override { return 0; }
    uint64_t getRenderExceptionCount() const noexcept override { return 0; }
    uint64_t getSanitisedSampleCount() const noexcept override { return 0; }
    uint64_t getSanitisedControlCount() const noexcept override { return 0; }
    uint64_t getInternalErrorCount() const noexcept override { return 0; }
    uint64_t getRenderedBlockCount() const noexcept override { return 0; }
    uint64_t getRenderedFrameCount() const noexcept override { return 0; }
    uint64_t getProgramLoadSuccessCount() const noexcept override { return 0; }
    uint64_t getProgramLoadFailureCount() const noexcept override { return programLoadFailureCount.load (std::memory_order_relaxed); }
    uint64_t getAsyncProgramLoadQueuedCount() const noexcept override { return 0; }
    uint64_t getAsyncProgramLoadCompletedCount() const noexcept override { return 0; }
    uint64_t getAsyncProgramLoadDroppedCount() const noexcept override { return 0; }
    bool isAsyncProgramLoadActive() const noexcept override { return false; }

private:
    void setUnavailableError()
    {
        const juce::ScopedLock lock (errorLock);
        lastError = EmbeddedLanguageEngine::getLanguageName (language)
                    + " backend is not built into this Alchemy binary. Link its native SDK and enable the backend at CMake configure time.";
    }

    EmbeddedLanguageEngine::Language language;
    mutable juce::CriticalSection errorLock;
    juce::String lastError;
    std::atomic<bool> ready { false };
    std::atomic<uint64_t> silentProcessCount { 0 };
    std::atomic<uint64_t> programLoadFailureCount { 0 };
};
}

EmbeddedLanguageEngine::EmbeddedLanguageEngine (Language selectedLanguage)
    : language (selectedLanguage),
      runtime (createRuntime (selectedLanguage))
{
}

EmbeddedLanguageEngine::~EmbeddedLanguageEngine() = default;

EmbeddedLanguageEngine::EmbeddedLanguageEngine (EmbeddedLanguageEngine&&) noexcept = default;

EmbeddedLanguageEngine& EmbeddedLanguageEngine::operator= (EmbeddedLanguageEngine&&) noexcept = default;

std::vector<EmbeddedLanguageEngine::Language> EmbeddedLanguageEngine::getSupportedLanguages()
{
    return
    {
        Language::chuck,
        Language::faust,
        Language::csound,
        Language::supercollider,
        Language::rtcmix
    };
}

juce::String EmbeddedLanguageEngine::getLanguageName (Language languageToName)
{
    switch (languageToName)
    {
        case Language::chuck:    return "ChucK";
        case Language::faust:    return "Faust";
        case Language::csound:   return "Csound";
        case Language::supercollider: return "SuperCollider";
        case Language::rtcmix:   return "RTcmix";
    }

    return "Unknown";
}

juce::String EmbeddedLanguageEngine::getLanguageBuildStatus (Language languageToReport)
{
#if WELD_HAS_RTCMIX
    if (languageToReport == Language::rtcmix)
        return "RTcmix backend is built in with the embedded audio API";
#endif

#if WELD_HAS_CSOUND
    if (languageToReport == Language::csound)
        return "Csound backend is built in with host-implemented audio I/O, direct spin/spout buffers, and dynamic libcsound loading";
#endif

#if WELD_HAS_SUPERCOLLIDER
    if (languageToReport == Language::supercollider)
        return "SuperCollider backend is built in with embedded libscsynth host-pulled audio and no external server";
#endif

    if (isLanguageBuiltIn (languageToReport))
        return getLanguageName (languageToReport) + " backend is built in";

    if (languageToReport == Language::supercollider)
        return "SuperCollider backend is declared but not built in; it requires an in-process scsynth host-buffer backend, not an external server";

    return getLanguageName (languageToReport)
           + " backend is declared but not built in; it must be linked against its native in-process SDK";
}

bool EmbeddedLanguageEngine::isLanguageBuiltIn (Language languageToCheck) noexcept
{
    if (languageToCheck == Language::chuck)
        return true;

#if WELD_HAS_CSOUND
    if (languageToCheck == Language::csound)
        return true;
#endif

#if WELD_HAS_RTCMIX
    if (languageToCheck == Language::rtcmix)
        return true;
#endif

#if WELD_HAS_SUPERCOLLIDER
    if (languageToCheck == Language::supercollider)
        return true;
#endif

    return false;
}

EmbeddedLanguageEngine::Language EmbeddedLanguageEngine::getLanguage() const noexcept
{
    return language;
}

bool EmbeddedLanguageEngine::prepare (double sampleRate, int maximumBlockSize, int inputChannels, int outputChannels)
{
    return runtime->prepare (sampleRate, maximumBlockSize, inputChannels, outputChannels);
}

void EmbeddedLanguageEngine::release() noexcept
{
    runtime->release();
}

bool EmbeddedLanguageEngine::loadProgram (const juce::String& programBody)
{
    return loadProgram (programBody, getCurrentParameterBindings());
}

bool EmbeddedLanguageEngine::loadProgram (const juce::String& programBody, const std::vector<ParameterBinding>& bindings)
{
    return runtime->loadProgram (programBody, bindings);
}

bool EmbeddedLanguageEngine::loadProgramAsync (const juce::String& programBody)
{
    return loadProgramAsync (programBody, getCurrentParameterBindings());
}

bool EmbeddedLanguageEngine::loadProgramAsync (const juce::String& programBody, const std::vector<ParameterBinding>& bindings)
{
    return runtime->loadProgramAsync (programBody, bindings);
}

bool EmbeddedLanguageEngine::waitForAsyncProgramLoads (int timeoutMilliseconds)
{
    return runtime->waitForAsyncProgramLoads (timeoutMilliseconds);
}

void EmbeddedLanguageEngine::process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output)
{
    runtime->process (input, output);
}

juce::String EmbeddedLanguageEngine::getCurrentProgram() const { return runtime->getCurrentProgram(); }
std::vector<EmbeddedLanguageEngine::ParameterBinding> EmbeddedLanguageEngine::getCurrentParameterBindings() const { return runtime->getCurrentParameterBindings(); }
bool EmbeddedLanguageEngine::setParameterValue (int index, float value) noexcept { return runtime->setParameterValue (index, value); }
bool EmbeddedLanguageEngine::setParameterValue (const juce::String& name, float value) { return runtime->setParameterValue (name, value); }
float EmbeddedLanguageEngine::getParameterValue (int index) const noexcept { return runtime->getParameterValue (index); }
int EmbeddedLanguageEngine::getParameterIndex (const juce::String& name) const { return runtime->getParameterIndex (name); }
int EmbeddedLanguageEngine::getParameterCount() const noexcept { return runtime->getParameterCount(); }
bool EmbeddedLanguageEngine::isReady() const noexcept { return runtime->isReady(); }
juce::String EmbeddedLanguageEngine::getLastError() const { return runtime->getLastError(); }

void EmbeddedLanguageEngine::setFrequency (float value)
{
    static_cast<void> (setParameterValue ("hostFreq", value));
}

void EmbeddedLanguageEngine::setGain (float value)
{
    static_cast<void> (setParameterValue ("hostGain", value));
}

void EmbeddedLanguageEngine::setToneBlend (float value)
{
    static_cast<void> (setParameterValue ("hostBlend", value));
}

uint64_t EmbeddedLanguageEngine::getSilentProcessCount() const noexcept { return runtime->getSilentProcessCount(); }
uint64_t EmbeddedLanguageEngine::getOversizedBlockCount() const noexcept { return runtime->getOversizedBlockCount(); }
uint64_t EmbeddedLanguageEngine::getRenderExceptionCount() const noexcept { return runtime->getRenderExceptionCount(); }
uint64_t EmbeddedLanguageEngine::getSanitisedSampleCount() const noexcept { return runtime->getSanitisedSampleCount(); }
uint64_t EmbeddedLanguageEngine::getSanitisedControlCount() const noexcept { return runtime->getSanitisedControlCount(); }
uint64_t EmbeddedLanguageEngine::getInternalErrorCount() const noexcept { return runtime->getInternalErrorCount(); }
uint64_t EmbeddedLanguageEngine::getRenderedBlockCount() const noexcept { return runtime->getRenderedBlockCount(); }
uint64_t EmbeddedLanguageEngine::getRenderedFrameCount() const noexcept { return runtime->getRenderedFrameCount(); }
uint64_t EmbeddedLanguageEngine::getProgramLoadSuccessCount() const noexcept { return runtime->getProgramLoadSuccessCount(); }
uint64_t EmbeddedLanguageEngine::getProgramLoadFailureCount() const noexcept { return runtime->getProgramLoadFailureCount(); }
uint64_t EmbeddedLanguageEngine::getAsyncProgramLoadQueuedCount() const noexcept { return runtime->getAsyncProgramLoadQueuedCount(); }
uint64_t EmbeddedLanguageEngine::getAsyncProgramLoadCompletedCount() const noexcept { return runtime->getAsyncProgramLoadCompletedCount(); }
uint64_t EmbeddedLanguageEngine::getAsyncProgramLoadDroppedCount() const noexcept { return runtime->getAsyncProgramLoadDroppedCount(); }
bool EmbeddedLanguageEngine::isAsyncProgramLoadActive() const noexcept { return runtime->isAsyncProgramLoadActive(); }

std::unique_ptr<EmbeddedLanguageEngine::Runtime> EmbeddedLanguageEngine::createRuntime (Language languageToCreate)
{
    if (languageToCreate == Language::chuck)
        return std::make_unique<ChucKRuntime>();

#if WELD_HAS_RTCMIX
    if (languageToCreate == Language::rtcmix)
        return std::make_unique<RTcmixRuntime>();
#endif

#if WELD_HAS_CSOUND
    if (languageToCreate == Language::csound)
        return std::make_unique<CsoundRuntime>();
#endif

#if WELD_HAS_SUPERCOLLIDER
    if (languageToCreate == Language::supercollider)
        return std::make_unique<SuperColliderRuntime>();
#endif

    return std::make_unique<UnavailableRuntime> (languageToCreate);
}
