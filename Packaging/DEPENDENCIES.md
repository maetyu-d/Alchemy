# Alchemy Runtime Dependencies

Alchemy always includes the embedded ChucK engine. The other language backends are loaded at runtime when their libraries are present.

The macOS packaging script copies any local optional runtime libraries it can find into:

```text
Alchemy.app/Contents/Frameworks
```

The engine also checks the app bundle at launch, so bundled libraries are preferred before system-wide fallbacks.

Optional libraries:

- Csound: `CsoundLib64.framework/CsoundLib64`, `libcsound64.dylib`, or `libcsound.dylib`
- RTcmix: `librtcmix_embedded.dylib`
- SuperCollider server: `libscsynth.dylib`
- SuperCollider language bridge: `libweldsclang.dylib`

SuperCollider UGen plugins are not a single library. If they are available locally, keep them alongside the app release notes and set:

```sh
export WELD_SUPERCOLLIDER_PLUGIN_PATH=/path/to/plugins
```

If a backend is unavailable, Alchemy keeps the project loadable and uses the existing silent/fallback behavior instead of opening a separate audio server.
