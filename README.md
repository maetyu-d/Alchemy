# Alchemy

This is a lean JUCE audio host with ChucK embedded in-process.

Runtime shape:

```text
JUCE audio callback
  -> EmbeddedChucKEngine
  -> ChucK::run(input, output, frames)
  -> JUCE output buffer
```

There is no `chuck` command-line process, no RtAudio stream, and no external audio server. JUCE owns the audio device and advances the ChucK VM directly.

The embedded engine is built as a reusable library target:

```cmake
target_link_libraries(YourPluginOrApp PRIVATE Alchemy::Engine)
```

Consumers include the public umbrella header:

```cpp
#include <WeldChucKEngine.h>
```

The console executable is now only a host/test harness around that library. The `weld_chuck_engine` static archive contains the embedded engine object and exposes its JUCE/ChucK link requirements through CMake, so a plugin or app shell can depend on `Alchemy::Engine` without compiling or linking the console harness.

The public header also exposes `EmbeddedLanguageEngine`, a backend-neutral host facade with the same real-time contract as the ChucK engine:

```cpp
EmbeddedLanguageEngine engine (EmbeddedLanguageEngine::Language::chuck);
engine.prepare (sampleRate, maxBlockSize, inputs, outputs);
engine.loadProgramAsync (programText, parameterBindings);
engine.process (input, output);
```

The language slots are `chuck`, `faust`, `csound`, `supercollider`, and `rtcmix`. ChucK is always built in. Faust is built when a Faust checkout with `faust/dsp/interpreter-dsp-c.h` is available, and it dynamically loads `libfaust` at prepare time. Csound is built as a dynamic in-process backend and loads `libcsound` at prepare time. RTcmix is built when an RTcmix checkout with `RTcmix_API.h` is available, and it dynamically loads an embedded RTcmix runtime library at prepare time. SuperCollider is built when a SuperCollider checkout with the Alchemy host-audio `libscsynth` patch is available, and it dynamically loads that in-process runtime at prepare time. None of these backends spawn external interpreters or open their own audio devices; unavailable builds clear output buffers if asked to render. Use `EmbeddedLanguageEngine::isLanguageBuiltIn()` and `getLanguageBuildStatus()` to check the compiled-in set at runtime.

`EmbeddedPerformanceEngine` sits above the language facade for pieces that move between language states. It keeps prepared language runtimes for every track inside every state, advances a sample-clocked playhead at a global tempo, and mixes overlapping state windows so an outgoing state can keep rendering its tail while the next state starts:

```cpp
EmbeddedPerformanceEngine performance;
performance.prepare (sampleRate, maxBlockSize, inputs, outputs);
performance.setTempoMap ({ { 0.0, 120.0 }, { 96.0, 132.0 } });
performance.setTimeSignatureMap ({ { 0.0, 4, 4 }, { 64.0, 7, 8 } });
performance.setPhaseRotationMap ({ { 0.0, 0.0 }, { 64.0, 0.5 } });

performance.loadSequence ({
    { "opening-chuck",
      EmbeddedLanguageEngine::Language::chuck,
      chuckSource,
      EmbeddedChucKEngine::getDefaultParameterBindings(),
      EmbeddedPerformanceEngine::secondsToBeats (30.0, 120.0),
      EmbeddedPerformanceEngine::secondsToBeats (8.0, 120.0) },

    { "middle-sc",
      EmbeddedLanguageEngine::Language::supercollider,
      superColliderSource,
      EmbeddedChucKEngine::getDefaultParameterBindings(),
      EmbeddedPerformanceEngine::secondsToBeats (20.0, 120.0),
      EmbeddedPerformanceEngine::secondsToBeats (6.0, 120.0) },

    { "ending-rtcmix",
      EmbeddedLanguageEngine::Language::rtcmix,
      rtcmixScore,
      EmbeddedChucKEngine::getDefaultParameterBindings(),
      EmbeddedPerformanceEngine::secondsToBeats (25.0, 120.0),
      EmbeddedPerformanceEngine::secondsToBeats (10.0, 120.0) }
});

performance.start();
performance.process (input, output);
```

The performance engine appends optional state controls to each track's binding set: `hostStateGate`, `hostStateGain`, `hostTempoBpm`, `hostStateBeat`, and `hostGlobalBeat`. It also exposes per-track controls such as `hostTrackGain`, `hostTrackTempoBpm`, `hostTrackTimeSigNumerator`, `hostTrackTimeSigDenominator`, and `hostTrackPhaseRotation`. A language program can ignore them and still be host-faded during its tail, or read them for its own release envelope and local timing behavior. ChucK is the score language for this layer: its score commands drive the transport/state scheduler while sound-producing tracks remain free to be ChucK, SuperCollider, Csound, RTcmix, or Faust.

Tempo events are expressed at global beat positions and are integrated into sample positions when the sequence timeline is built. Time signatures do not move state boundaries by themselves, but they describe the score grid exposed to state programs as `hostTimeSigNumerator`, `hostTimeSigDenominator`, `hostBarBeat`, and `hostBarPhase`. Phase rotations are exposed as `hostPhaseRotation` and rotate the bar/cycle phase used for those bar controls; they are intended for things like displaced downbeats, rotated patterns, and score-level polymetric gestures without disturbing the host audio clock.

## Build

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The executable will be at:

```sh
build/Alchemy_artefacts/Release/Alchemy
```

Run it from a terminal. It starts the default audio device, runs the embedded ChucK program inside the JUCE callback, and quits when you press return.

The GUI shell builds as a separate full-screen JUCE app:

```sh
cmake --build build --target AlchemyGui --config Release
open "build/AlchemyGui_artefacts/Release/Alchemy.app"
```

Its top third keeps the score/state visualization: state nodes, inlet/outlet-style transition cords, state add/remove controls, and a compact transport strip. ChucK is the score editor in this area; edit the score script and press `Run` to compile it in an embedded ChucK VM that drives the score/state machine through a host command bridge. The score bridge captures command sample frames from ChucK's own `now`, builds exact tempo, meter, phase, stop, and track gain schedules, then starts playback from that prepared audio timeline instead of relying on the GUI timer. Press `Sync` to regenerate the ChucK score from the current GUI states/tracks, `Template` to restore the starter score, or `Clear` to empty the score pane. The lower two thirds switch between per-state track tabs, an arrangement view during count-in/playback, and a mixer view. Each state tab has track add/remove controls. Each track stores an accepted language program and defaults to tight global timing while allowing local tempo, meter, and phase overrides. The File menu can save/open `.alchemy` projects, open the bundled example projects, and export WAVs as all stems, current-state stems, a master mix, or stems plus master with sample-rate, bit-depth, channel, duration, naming, mute, tail, and headroom options. If an optional native backend cannot prepare or rejects a program, the GUI logs the native failure and retries that sequence with ChucK fallback tracks so the arrangement remains audible.

The examples are copied into the app bundle at build time and are also available in the repository `Examples/` folder.

For a repeatable macOS release zip:

```sh
scripts/package_macos_release.sh 0.1.1
```

The package script rebuilds the GUI, copies the example projects, copies any local optional Csound/RTcmix/SuperCollider runtime libraries it can find into `Alchemy.app/Contents/Frameworks`, bundles the SuperCollider class library and Faust standard libraries when present, writes a dependency manifest, and creates `dist/Alchemy-v<version>-macOS.zip`.

To check that embedded language backends can run representative online/manual-style examples:

```sh
build/Alchemy_artefacts/Release/Alchemy --language-example-test
```

This renders a small compatibility corpus for ChucK, Faust, Csound, SuperCollider, and RTcmix through the actual in-process backends and checks for finite, non-silent output. The corpus is adapted from the official/manual examples and references for ChucK UGens, Faust `stdfaust.lib`, Csound opcodes, SuperCollider example functions, and RTcmix instruments/PFields:

- https://chuck.stanford.edu/doc/examples/
- https://chuck.cs.princeton.edu/doc/program/ugen.html
- https://faustlibraries.grame.fr/standardFunctions/
- https://faustlibraries.grame.fr/libs/oscillators/
- https://csound.com/docs/manual/
- https://supercollider.github.io/examples
- https://rtcmix.org/reference/instruments/WAVETABLE.html
- https://rtcmix.org/reference/instruments/FMINST.html
- https://rtcmix.org/reference/instruments/AMINST.html

Languages that are declared but not built into the current binary are reported as skipped. SuperCollider examples from `supercollider.github.io/examples` that use `{ ... }.play` should be pasted into Alchemy as the function body only, returning the audio signal; Alchemy owns the synth lifetime and output routing.

The score VM exposes `score`, `state`, and `track` APIs inside ChucK. String arguments are marshalled to the host bridge, while timing, loops, functions, and conditionals are ordinary ChucK:

```chuck
score.clear();
tempo(120);
meter(4, 4);
state.add("Intro", 16, 4);
state.add("Verse A", 12, 4);
state.add("Verse B", 12, 4);
state.connect(1, 2, 50);
state.connect(1, 3, 50);
track.add(1, "Lead", "chuck");
track.gain(1, 1, 0.7);
play();

2::bar => now;
tempo(132);
track.gain(1, 1, 0.45);

for (0 => int i; i < 4; i++)
{
    1::bar => now;
    track.phase(1, 1, i * 0.25);
}

stop();
```

Supported score calls include `score.clear()`, `tempo(bpm)`, `meter(numerator, denominator)`, `phase(beats)`, `play()`, `stop()`, `mixer(visible)`, `state.add(name, durationBeats, tailBeats)`, `state.remove(index)`, `state.name(index, name)`, `state.duration(index, beats)`, `state.tail(index, beats)`, `state.select(index)`, `state.connect(fromState, toState, weight)`, `state.disconnect(fromState, toState)`, `state.clearConnections(stateIndex)`, `track.add(stateIndex, name, language)`, `track.remove(stateIndex, trackIndex)`, `track.name(...)`, `track.language(...)`, `track.gain(...)`, `track.mute(...)`, `track.solo(...)`, `track.sync(...)`, `track.tempo(...)`, `track.meter(...)`, `track.phase(...)`, `track.code(...)`, `track.template(...)`, and `track.clear(...)`. Transition weights are relative, so `50/50`, `1/1`, and `0.5/0.5` describe the same branch probability.

To audition the multi-state performance path through the default output device:

```sh
build/Alchemy_artefacts/Release/Alchemy --quick-performance-demo
build/Alchemy_artefacts/Release/Alchemy --performance-demo
```

The quick demo compresses the ChucK -> SuperCollider -> RTcmix sequence into a short check. The full demo uses the longer ChucK, SuperCollider, RTcmix, and coda states, with outgoing tails left running across transitions. The GUI demo starts from a branching state graph: State 1 has weighted 70/30 outgoing paths, and State 2 has weighted 50/50 outgoing paths.

Run the headless engine self-test with:

```sh
build/Alchemy_artefacts/Release/Alchemy --self-test
build/Alchemy_artefacts/Release/Alchemy --stress-test
build/Alchemy_artefacts/Release/Alchemy --callback-test
build/Alchemy_artefacts/Release/Alchemy --fuzz-test
build/Alchemy_artefacts/Release/Alchemy --program-test
build/Alchemy_artefacts/Release/Alchemy --parameter-test
build/Alchemy_artefacts/Release/Alchemy --async-program-test
build/Alchemy_artefacts/Release/Alchemy --score-script-test
build/Alchemy_artefacts/Release/Alchemy --boundary-test
build/Alchemy_artefacts/Release/Alchemy --concurrency-test
build/AlchemyEngineBoundaryTest
/Users/user/.local/opt/cmake/CMake.app/Contents/bin/ctest --test-dir build --output-on-failure
```

Or run the build-system check target:

```sh
cmake --build build --target check
```

### RTcmix Backend

Build RTcmix's embedded runtime before configuring Alchemy if you want the RTcmix backend active:

```sh
git clone https://github.com/RTcmix/RTcmix.git third_party/rtcmix
cd third_party/rtcmix
git checkout 671e0da83d3acacc78e46f787b7f7e23c461c9fc
git apply ../patches/rtcmix-embedded-weld.patch
./configure --without-jack --without-alsa --without-osc --without-midi
make BUILDTYPE=OSXEMBEDDED AUDIODRIVER=EMBEDDEDAUDIO RTLIBTYPE=DYNAMIC
```

The Alchemy CMake configure step looks for `third_party/rtcmix/src/rtcmix/RTcmix_API.h` and records a runtime library hint for `librtcmix_embedded.dylib` or `librtcmix_embedded.so`. You can override that path with `-DWELD_RTCMIX_LIBRARY=/path/to/librtcmix_embedded.dylib` or by setting the `WELD_RTCMIX_LIBRARY` environment variable before running the host.

### Faust Backend

Build Faust's interpreter libfaust before configuring Alchemy if you want the Faust backend active:

```sh
git clone --depth 1 https://github.com/grame-cncm/faust.git third_party/faust
git -C third_party/faust submodule update --init --depth 1 libraries
cmake -S third_party/faust/build -B build-faust \
  -C third_party/faust/build/backends/interp.cmake \
  -C third_party/faust/build/targets/interp.cmake \
  -DINCLUDE_LLVM=OFF -DUSE_LLVM_CONFIG=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-faust --target dynamiclib
```

The Alchemy CMake configure step looks for `third_party/faust/architecture/faust/dsp/interpreter-dsp-c.h` and records a runtime hint for `third_party/faust/build/lib/libfaust.dylib` or `.so`. You can override that path with `-DWELD_FAUST_LIBRARY=/path/to/libfaust.dylib` or by setting the `WELD_FAUST_LIBRARY` environment variable before running the host. Faust source loading is in-process and host-pulled. Alchemy compiles the DSP from a source string, swaps the prepared instance into the render path, feeds host audio buffers to `compute()`, and maps host parameters to Faust controls whose labels match the binding names:

```faust
import("stdfaust.lib");
hostFreq = hslider("hostFreq", 220, 30, 4000, 1);
hostGain = hslider("hostGain", 0.14, 0, 0.4, 0.001);
process = os.osc(hostFreq) * hostGain <: _, _;
```

### Csound Backend

Build Csound from source before configuring Alchemy if you want the Csound backend to render locally:

```sh
git clone --depth 1 https://github.com/csound/csound.git third_party/csound
cmake -S third_party/csound -B build-csound \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_CSOUND_COMMAND=OFF \
  -DBUILD_CSBEATS=OFF -DBUILD_TESTS=OFF -DBUILD_UTILITIES=OFF \
  -DBUILD_PLUGINS=OFF -DUSE_PORTAUDIO=OFF -DUSE_PORTMIDI=OFF \
  -DUSE_JACK=OFF -DUSE_COREMIDI=OFF -DUSE_AUDIOUNIT=OFF \
  -DUSE_LIBSNDFILE=OFF -DUSE_LIBSAMPLERATE=OFF -DUSE_CURL=OFF \
  -DUSE_GETTEXT=OFF
cmake --build build-csound --target CsoundLib64
```

On macOS, Apple's `/usr/bin/bison` may be too old for current Csound source. Build or install a newer GNU Bison and pass `-DBISON_EXECUTABLE=/path/to/bison` to the Csound configure command if parser generation fails.

The Alchemy CMake configure step records a runtime hint for `build-csound/CsoundLib64.framework/CsoundLib64` on macOS or `build-csound/libcsound64.so`/`.dylib` on Unix-style builds. You can override that path with `-DWELD_CSOUND_LIBRARY=/path/to/libcsound64.dylib` or by setting the `WELD_CSOUND_LIBRARY` environment variable before running the host.

Csound source loading is in-process and host-pulled. Alchemy injects `sr`, `ksmps = 1`, `nchnls`, `nchnls_i`, and `0dbfs = 1` around the orchestra body, creates a private Csound instance off the audio callback, starts it with host-implemented audio I/O, schedules `i 1 0 -1`, then swaps the prepared instance into the render path. The audio callback writes directly to Csound's `spin` buffer, calls `csoundPerformKsmps()` once per host sample, reads directly from `spout`, and maps host parameters to Csound control channels such as:

```csound
instr 1
    kfreq chnget "hostFreq"
    kgain chnget "hostGain"
    aleft oscili kgain, kfreq, 1
    aright oscili kgain, kfreq * 1.01, 1
    outs aleft, aright
endin
```

### SuperCollider Backend

Build SuperCollider's host-audio `libscsynth`, embedded language bridge, and the UGen plugins your source uses before configuring Alchemy if you want the SuperCollider backend active:

```sh
git clone https://github.com/supercollider/supercollider.git third_party/supercollider
cd third_party/supercollider
git checkout ae4ddb74ba5baacc94509fb671913458b8a07299
git apply ../patches/supercollider-host-weld.patch
cd ../..
cmake -S third_party/supercollider -B build-supercollider-host \
  -DLIBSCSYNTH=ON -DAUDIOAPI=host -DNO_LIBSNDFILE=ON \
  -DSUPERNOVA=OFF -DSCLANG_SERVER=OFF -DSC_EL=OFF -DSC_QT=OFF \
  -DSC_IDE=OFF -DSC_ED=OFF -DSC_VIM=OFF -DNATIVE=OFF
cmake --build build-supercollider-host --target libscsynth weldsclang
cmake --build build-supercollider-host --target OscUGens LFUGens IOUGens MulAddUGens BinaryOpUGens UnaryOpUGens
```

The Alchemy CMake configure step looks for `third_party/supercollider/include/server/SC_WorldOptions.h` and records runtime hints for `build-supercollider-host/server/scsynth/libscsynth.dylib` and `build-supercollider-host/lang/libweldsclang.dylib`. You can override those paths with `-DWELD_SUPERCOLLIDER_LIBRARY=/path/to/libscsynth.dylib` and `-DWELD_SUPERCOLLIDER_LANG_LIBRARY=/path/to/libweldsclang.dylib`, or with the matching environment variables before running the host.

SuperCollider source loading is in-process: Alchemy asks the embedded `libsclang` bridge to compile source text into SynthDef bytes off the audio callback, sends `/d_recv` and `/s_new` directly into the private `libscsynth` world, then renders by host-pulling audio. A source body may evaluate to a `SynthDef` named `weldMain`, or more conveniently to a `Function`; function arguments receive the performance control bus in binding order, starting with `hostFreq`, `hostGain`, `hostBlend`, `hostStateGate`, `hostStateGain`, and continuing through the track tempo, meter, phase, and gain controls:

```supercollider
{ |freq = 440, gain = 0.1, blend = 0.5, stateGate = 1, stateGain = 1, tempoBpm = 120,
   stateBeat = 0, globalBeat = 0, timeSigNumerator = 4, timeSigDenominator = 4,
   barBeat = 0, barPhase = 0, phaseRotation = 0, trackGain = 1|
    SinOsc.ar(freq) * gain * stateGain * trackGain
}
```

## Notes

The first prototype disables ChucK MIDI, HID, serial, shell, and on-the-fly network server support so the embed stays tight. The ChucK VM, compiler, scheduler, standard UGens, and audio synthesis run inside the JUCE app.

The multi-language facade keeps the same tightness boundary for future language backends:

- Faust is embedded through libfaust's interpreter factory API. Alchemy compiles a candidate DSP instance off the audio callback, binds host controls to Faust UI zones such as `hslider("hostFreq", ...)`, and calls `compute()` from the host-owned render path after commit.
- Csound is embedded through `libcsound` with host-implemented audio I/O. Alchemy compiles an orchestra body into a private candidate instance, forces `ksmps = 1`, maps controls through Csound software channels, drives `spin`/`spout` directly, and performs one Csound k-cycle per host sample so parameter and audio exchange are sample-tight.
- SuperCollider is embedded as `libscsynth` plus an in-process `libsclang` bridge, not by launching `scsynth`/`sclang` or using OSC to an external server. Alchemy's host-pulled audio driver/world adapter lets JUCE own the callback, feeds SC input/output buses directly, compiles SC source to SynthDef bytes off the audio callback, and commits server packets directly into the private world.
- RTcmix is embedded through its `EMBEDDEDAUDIO` C API. Alchemy loads the embedded runtime in-process, configures float interleaved buffers, maps host parameters to `makeconnection("inlet", ...)` pfields, and calls `RTcmix_runAudio()` from the host render path. RTcmix-owned audio threads or command-line `CMIX` execution do not meet this repository's tightness rule.

RTcmix's public embedded API is a process-global runtime, so Alchemy allows only one active RTcmix engine at a time. Score loading is off the audio callback and fails silent, but it is not as transactional as the ChucK VM swap because stock RTcmix does not expose separate candidate worlds for parse/commit/rollback.

The performance sequencer is host-clocked rather than language-clocked: state starts, state ends, and tail windows are converted from musical beats to absolute sample frames through the active tempo map. The time-signature map and phase-rotation map describe the score grid that states and controllers see, without letting meter metadata disturb the audio callback clock. ChucK score commands drive this scheduler with ChucK-style timing while the sound-producing states remain free to be ChucK, SuperCollider, Csound, RTcmix, or Faust.

Control values are stored in a fixed-size parameter binding table on the JUCE side and copied directly into ChucK globals from the audio callback before each `ChucK::run()` call. Indexed parameter writes are lock-free and bounded; name-based writes are available for non-real-time callers. That avoids spawning messages, allocating global update requests, or taking a UI lock on the audio thread.

ChucK program loading is transactional. New program bodies are compiled into a separate candidate ChucK VM away from the audio callback path. Parameter bindings inject `global float` declarations into the candidate program, so loaded program bodies can use named host controls without declaring boilerplate. If the candidate compiles, starts, and binds its globals, the engine performs a short locked swap. If compilation or binding fails, the current VM keeps playing and the failure is counted.

Program loading can also be queued asynchronously. `loadProgramAsync()` validates the binding request, publishes only the latest pending program to a background loader thread, and returns immediately so UI/control callers do not wait for ChucK compilation. Rapid async requests are coalesced: an older pending request can be dropped before compilation if a newer one replaces it, while the currently playing VM stays untouched until a candidate commits successfully. `waitForAsyncProgramLoads()` is available for tests and controlled shutdown paths, not for the audio callback.

The default binding set preserves the original controls: `hostFreq`, `hostGain`, and `hostBlend`. Callers can replace that with any validated binding set up to the fixed maximum: names must be ChucK-safe identifiers, duplicates are rejected, ranges/defaults must be finite and consistent, and invalid binding transactions leave the last good VM and binding table in place.

The host also preallocates conservative scratch buffers, shares the same hard block-size limit as the embedded engine, clears host outputs before rendering, clears every host output pointer before rejecting unsupported layouts, checks null channel arrays and channel pointers, suppresses denormals in the callback, sanitises non-finite control/audio values, limits output samples to a safe range, and fails silent if the device ever asks for a block larger than the prepared safety size. The callback has a final exception-containment layer so unexpected host-side failures are counted and muted instead of escaping the audio callback.

Prepare-time exceptions are caught and reported as startup failure. Render-time exceptions from ChucK are contained by muting the engine instead of allowing an exception to escape the real-time callback.

The engine rejects unsupported block sizes and channel counts before allocation, validates its prepared buffer invariants before every render, tears down ChucK defensively during release, and contains render exceptions by muting the engine. It also exposes lock-free diagnostic counters for silent callbacks, oversized blocks, render exceptions, sanitised samples, sanitised controls, internal invariant failures, rendered blocks, rendered frames, program load successes, program load failures, queued async loads, completed async loads, and coalesced async drops; the console host prints the most important counters on exit.

The tests cover cold and released engines, repeated prepare/release cycles, short and wide buffers, null and mixed-null callback channels, oversized callbacks, rejected device sample rates and block sizes, malformed input samples, deterministic fuzzed buffer/control combinations, transactional good/bad ChucK program loads, async good/bad/coalesced program loads, custom/empty/invalid parameter binding transactions, ChucK score-script compilation/timing/host-bridge commands, indexed control writes racing with reloads, reload storms while rendering, maximum accepted block/channel boundaries, a separate engine-library consumer target, diagnostic counter accuracy, performance-state tail overlap, callback exception containment staying dormant, and render calls racing against prepare/release/control updates.

ChucK is included under its dual MIT/GPL licensing. See `third_party/chuck/LICENSE.MIT` and `third_party/chuck/LICENSE.GPL`.
