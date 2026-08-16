# Building Swara XT

## Requirements

- CMake 3.22 or newer
- A C++17 compiler
- Git
- JUCE 7.0.12 at `.cache/JUCE`
- The pinned Shruthi and avrlib submodules

```bash
git submodule update --init third_party/shruthi-1
git -C third_party/shruthi-1 -c submodule.avrlib.url=https://github.com/pichenettes/avril.git submodule update --init avrlib
git clone --branch 7.0.12 --depth 1 https://github.com/juce-framework/JUCE.git .cache/JUCE
```

The obsolete `git://` URL in the pinned Shruthi revision is overridden only for
the avrlib checkout. The nested Shruthi `tools` submodule is not required to
build or test Swara XT and need not be initialized.

## Configure, build, and test

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Windows, the scripts under `scripts/` provide the same repository-relative
workflow. Set `SWARAXT_JUCE_DIR` at configure time if JUCE is stored elsewhere:

```bash
cmake -S . -B build -DSWARAXT_JUCE_DIR=/path/to/JUCE
```

Primary artifacts are `Swara XT.vst3` and the `Swara XT` Standalone executable.
