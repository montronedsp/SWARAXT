# SWARA XT

**SWARA XT** is an open-source monophonic software synthesizer by MontroneDSP, derived in part from the Mutable Instruments Shruthi firmware.

It combines a digital dual-oscillator voice with a resonant four-pole low-pass filter, modulation matrix, envelopes, LFOs, sequencer/arpeggiator, presets, and a scalable themed interface.

## Features

- Two digital oscillator sources with multiple oscillator models
- Source mixer, sub oscillator, and noise
- Resonant four-pole low-pass filter
- Two envelopes and two LFOs
- Free-running and host-synchronised modulation
- Sequencer and arpeggiator
- 12-slot modulation matrix
- Factory and user presets
- Multiple interface skins and sizes
- VST3 and Standalone formats

## Linux installation

Install the latest official Linux x86_64 release (per-user, no root):

```bash
curl -fsSL https://raw.githubusercontent.com/montronedsp/swara-xt/main/scripts/install-linux.sh | bash
```

This installs:

- VST3 → `~/.vst3/Swara XT.vst3`
- Standalone → `~/.local/bin/swara-xt`

Manual archives remain available from [GitHub Releases](https://github.com/montronedsp/swara-xt/releases).

## Downloads

### Linux

The official Linux build is available free from the [GitHub Releases](https://github.com/montronedsp/swara-xt/releases) page.

### Windows & macOS

Official prebuilt Windows and macOS versions are available from:

[**store.montronedsp.com/l/swara**](https://store.montronedsp.com/l/swara)

The complete source code is available in this repository and may also be built locally.

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

Building SWARA XT requires CMake 3.22 or newer, a C++17-compatible compiler, JUCE 7.0.12, Git, and the pinned Shruthi source used by the project.

See [BUILDING.md](BUILDING.md) for build instructions.

## License

SWARA XT software is distributed under **GPL-3.0-or-later**.

See:

- [LICENSE](LICENSE)
- [COPYRIGHT](COPYRIGHT)
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

Adapted panel artwork has separate CC BY-SA 3.0 terms and attribution documented in:

[resources/Skin/ATTRIBUTION.md](resources/Skin/ATTRIBUTION.md)

SWARA XT contains software derived from the Shruthi firmware by Emilie Gillet.

SWARA XT is an independent project and is not an official Mutable Instruments product.

---

**SWARA XT 1.1.2 — MontroneDSP**
