# Building Swara XT

## Requirements

- CMake 3.22 or newer
- A C++17 compiler (Visual Studio 2022 on Windows; GCC/Clang on Linux/macOS)
- Git
- The exact platform JUCE pin below at `.cache/JUCE` (or set `SWARAXT_JUCE_DIR`)
- The pinned Shruthi and avrlib submodules

```bash
git submodule update --init third_party/shruthi-1
git -C third_party/shruthi-1 -c submodule.avrlib.url=https://github.com/pichenettes/avril.git submodule update --init avrlib
```

The obsolete `git://` URL in the pinned Shruthi revision is overridden only for
the avrlib checkout. The nested Shruthi `tools` submodule is not required to
build or test Swara XT and need not be initialized.

Use a separate immutable JUCE checkout for each selected platform:

| Platform | JUCE | Required revision |
| --- | --- | --- |
| Linux | 9.0.4 | `3fb5cbc0340758596dac39f7c8de47d562b4896e` |
| Windows/macOS | 9.0.1 | `e18f7f506c0b96f2c738a0bcd7fe6467a5005ad8` |

For Linux, obtain the source with:

```bash
git clone --branch 9.0.4 --depth 1 https://github.com/juce-framework/JUCE.git .cache/JUCE
git -C .cache/JUCE rev-parse HEAD
```

Windows/macOS use the same clone command with `--branch 9.0.1`. Check the
reported revision against the table before configuring. Linux CMake rejects
unexpected revisions and source drift, and generates the complete ChannelType
and signed WAVE-sentinel corrections inside the build directory. Keep the input
checkout pristine; the MIDI player reserve overlay is also build-owned. Linux
VST3 links isolate only the SharedCode archive's bundled implementation symbols.
Use separate out-of-source trees for compilers, build types and sanitizers.

Public CMake presets contain no machine-specific paths. Local overrides belong in
an ignored `CMakeUserPresets.json` or in environment/`-D` cache variables.

## Release vs development

| Preset | Tests | Purpose |
|--------|-------|---------|
| `*-release` | `SWARA_BUILD_TESTS=OFF` | Product plugin/standalone only |
| `*-dev` | `SWARA_BUILD_TESTS=ON` | Development + test executables |

Each preset uses its own directory under `build/` (for example `build/win-release`).
Do not reuse a Windows build tree for Linux or another configuration.

## Windows

```powershell
cmake --preset win-release
cmake --build --preset win-release
```

Or:

```powershell
.\scripts\build_release.ps1
```

Development with tests:

```powershell
cmake --preset win-dev
cmake --build --preset win-dev
```

## Linux

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

Or:

```bash
./scripts/build_release.sh
```

## macOS

Official formats in this tree are VST3 and Standalone (no AU target in CMake).

```bash
cmake --preset mac-release
cmake --build --preset mac-release
```

## Tests

Tests are opt-in. Configure a `*-dev` preset (or pass `-DSWARA_BUILD_TESTS=ON`),
then build/run only when explicitly requested:

```bash
ctest --test-dir build/win-dev -C Debug --output-on-failure
```

Do not run CTest, pluginval, or other validators as part of a normal release build.

## Artifacts

Primary artifacts (multi-config generators use a `Release`/`Debug` subdirectory):

- `build/<preset>/SwaraXT_artefacts/.../VST3/Swara XT.vst3`
- `build/<preset>/SwaraXT_artefacts/.../Standalone/Swara XT`

Set `SWARAXT_JUCE_DIR` at configure time if JUCE is stored outside `.cache/JUCE`.
