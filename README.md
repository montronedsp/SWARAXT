# SWARA XT

[![Quality gate status](https://sonarcloud.io/api/project_badges/measure?project=montronedsp_SWARAXT&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=montronedsp_SWARAXT) [![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15241/badge)](https://www.bestpractices.dev/projects/15241)

**SWARA XT** is an open-source synthesizer by MontroneDSP, built around a
Shruthi-derived monophonic voice. Two digital oscillators, nonlinear filtering
and a twelve-slot modulation matrix give it a distinctive range of basses,
leads and evolving textures. Its sixteen-step sequencer and microtuning system
bring rhythm and pitch structure into the instrument itself.

## Features

### Synthesis and filters

The HD voice renderer runs at the native Shruthi rate, with multiple oscillator
models, source mixing, a sub-oscillator and noise. The Classic path combines an
IR3109-inspired nonlinear four-pole low-pass filter and VCA, with **High**,
**Normal** and **Eco** quality settings.

The experimental **Dual SVF** offers Section 1, Serial and Parallel routing,
LP/BP/HP responses, Paper/Eagle calibration and separate Section 2 controls.
Follow, Independent and Relative modes set how the second section relates to
the first. Its front-panel controls are manual and session-local; a fresh
instance starts with Classic. Bank changes use a brief fade through silence.

### Modulation and sequencing

Two envelopes, two LFOs and dedicated filter ENV/LFO controls work alongside
twelve bipolar modulation routes. The paged matrix presents three routes at
a time, with a clear zero-depth indication and sources including envelopes,
LFOs, velocity, note and Mod Wheel.

The sequencer/arpeggiator provides sixteen steps, rests, ties, velocity and
controller values, host synchronization, sequence randomization and
**Sequence Lock**. Pitch modes are **LEGACY**, **LINEAR**, **SCALE** and
**QUANTIZED CV**. Optional MIDI output carries LEGACY notes or the nearest
12-TET notes from QUANTIZED CV; LINEAR and SCALE drive the internal voice.

### Microtuning

The shared tuning system includes **37 factory tunings**: equal divisions,
just intonation, Pythagorean scales, historical temperaments and non-octave
systems such as Bohlen–Pierce. Import Scala **.scl** files with an optional
**.kbm** keyboard mapping, or load **.swt** tables. Imported tuning data travels
with saved projects and user presets.

SCALE sequences play signed scale degrees; QUANTIZED CV snaps continuous
pitch to the active tuning. **INPUT TUNE** applies that same tuning to MIDI
input in TUNED or QUANTIZED mode. Keyboard mappings and the voice's playable
pitch range apply; imports are validated before the active tuning changes.

### Effects, presets and customization

- Sixteen DSP-board effects plus **Off**, including distortion, crush, comb,
  ring modulation, delay and feedback variants, looper and pitch processing.
- **57 factory programs**, including 40 Shruthi patches, and user-preset save/load.
- Five skins, three interface sizes, custom colours, artwork, decorations and
  manufacturer marks. Version 1.3.0 packages include skin examples and decorations
  for direct loading in the synthesizer. The separate **Skin Editor** remains
  available in source but its application is not distributed.
- VST3 and Standalone on Windows and Linux; macOS source builds also include AU.

**New to SWARA XT? Read the [Quick Start Manual](docs/QUICKSTART.md).**

## Quick start

1. Download the current archive from [GitHub Releases](https://github.com/montronedsp/SWARAXT/releases), or build from source.
2. Install the complete VST3 bundle in a VST3-compatible host location and rescan the host, or launch the included Standalone application.
3. Select a factory preset from the preset selector, or save a complete user preset with **SAVE**.
4. Send MIDI notes to the instrument track and route that track to an active audio output.
5. Open **SEQ/ARP** to use the internal sequencer or arpeggiator when desired.

## Linux installation

Install the latest official Linux x86_64 release (per-user, no root):

```bash
curl -fsSL https://raw.githubusercontent.com/montronedsp/SWARAXT/main/scripts/install-linux.sh | bash
```

This installs:

- VST3 → `~/.vst3/Swara XT.vst3`
- Standalone → `~/.local/bin/swara-xt`

Manual archives remain available from [GitHub Releases](https://github.com/montronedsp/SWARAXT/releases).

## Downloads

The latest published release is [1.2.3](https://github.com/montronedsp/SWARAXT/releases/tag/v1.2.3).
The current source targets **1.3.0 (unreleased)**. The feature descriptions above
follow current source; download links still refer to published releases.

### Linux

The official Linux build is available free from the [GitHub Releases](https://github.com/montronedsp/SWARAXT/releases) page.

### Windows

The current public Windows x64 package is available from
[GitHub Releases](https://github.com/montronedsp/SWARAXT/releases).

### macOS

No macOS archive is published with the current 1.2.3 GitHub release; see
[BUILDING.md](BUILDING.md) to build the current source locally.

## Performance

SWARA XT is a real-time software synthesizer. CPU usage depends on sample rate, audio buffer size, active modulation, and host configuration.

Very small audio buffers and high sample rates require substantially more processing power. For reliable real-time use, a modern 64-bit processor is recommended.

## Minimum requirements

- 64-bit processor
- 4 GB RAM
- 100 MB free disk space
- VST3-compatible host for plug-in use

Platform-specific compatibility may depend on the operating system, host, and locally installed system libraries.

## Building

Source builds require CMake 3.22+, a C++17 compiler, Git and the pinned Shruthi
submodules. Use Visual Studio 2022 on Windows, GCC/Clang on Linux or Apple Clang on macOS.

| Platform | JUCE revision |
|---|---|
| Linux | 9.0.4 — `3fb5cbc0340758596dac39f7c8de47d562b4896e` |
| Windows / macOS | 9.0.1 — `e18f7f506c0b96f2c738a0bcd7fe6467a5005ad8` |

Set `SWARAXT_JUCE_DIR` to the matching pristine checkout. See
[BUILDING.md](BUILDING.md) for dependencies, presets and platform instructions.

## License

SWARA XT software is distributed under **GPL-3.0-or-later**.

See:

- [LICENSE](LICENSE)
- [COPYRIGHT](COPYRIGHT)
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

The MontroneDSP and STONES outlined brand SVGs are copyright 2026 Andrea
Montrone and retain their existing CC BY-SA 3.0 grants;
the software GPL notice does not independently license brand assets.

Adapted panel artwork has separate CC BY-SA 3.0 terms and attribution documented in:

[resources/Skin/ATTRIBUTION.md](resources/Skin/ATTRIBUTION.md)

SWARA XT contains software derived from the Shruthi firmware by Emilie Gillet.

SWARA XT is an independent project and is not an official Mutable Instruments product.

## Source

`main` is the canonical SWARA XT source branch.

## Project information

- [Contributing](CONTRIBUTING.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Governance](GOVERNANCE.md)
- [Roadmap](ROADMAP.md)
- [Security Policy](SECURITY.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Building](BUILDING.md)

---

**SWARA XT 1.3.0 candidate — MontroneDSP**
