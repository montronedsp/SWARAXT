# Building Swara XT

## Requirements

- CMake 3.22 or newer
- A C++17 compiler (Visual Studio 2022 on Windows; GCC/Clang on Linux/macOS)
- Git
- JUCE 9.0.1 at `.cache/JUCE` (or set `SWARAXT_JUCE_DIR`)
- The pinned Shruthi and avrlib submodules

```bash
git submodule update --init third_party/shruthi-1
git -C third_party/shruthi-1 -c submodule.avrlib.url=https://github.com/pichenettes/avril.git submodule update --init avrlib
git clone --branch 9.0.1 --depth 1 https://github.com/juce-framework/JUCE.git .cache/JUCE
```

The obsolete `git://` URL in the pinned Shruthi revision is overridden only for
the avrlib checkout. The nested Shruthi `tools` submodule is not required to
build or test Swara XT and need not be initialized.

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
