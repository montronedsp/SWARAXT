# SWARA XT filter quality audit

## Topology

SWARA XT does not use a JUCE stock filter. The production path is a four-pole
IR3109-style OTA ladder implemented as four TPT one-pole stages with an
implicit resonance-feedback solver.

| Item | Production behaviour |
|------|----------------------|
| Class | `SwaraXtFilter` |
| Stages | `Ir3109Pole` × 4 (passive / active / passive / active load traits) |
| Resonance | `ResonanceNetwork`: phase splitter, diode-pair `tanh` limiter, feedback into stage 1 |
| Modes | Low-pass cascade only |
| Solver | Successive relaxation, up to 8 iterations, residual threshold `1e-9` |
| Oversampling | Linear interpolation of the filter input, then average of sub-samples |
| Internal rate | Shruthi native rate `20e6 / 510` Hz (~39.216 kHz), before host SRC |

Oversampling is selected from the **filter** sample rate, not the DAW rate.
Because the filter always runs at the internal rate, High is 4× (~157 kHz).
Host 44.1 / 48 / 96 kHz does not change that factor; the polyphase SRC sits
after the filter.

Cutoff and resonance are updated once per native control block (40 internal
samples, ~980 Hz). Envelope, LFO, key tracking and the modulation matrix all
arrive through that control-rate path. There is no audio-rate filter FM.

## Signal path

```
MIDI / voice
  → Shruthi oscillators and mixer (native block)
  → updateFilterFromShruthi()  (control-rate cutoff / resonance / env)
  → SwaraXtFilter::processSample()  (4× High, implicit solver if resonance > 0)
  → VCA
  → host SRC / output
```

## Cost model (one High sample at the internal rate)

With resonance at zero the solver is skipped (feedback gain is exactly 0).
One host-visible internal sample then costs 4 oversampled cascade passes,
each four TPT stages with two `tanh` nonlinearities.

With resonance engaged, each oversampled sample runs up to 8 solver previews
plus one committed cascade. That is the primary CPU hotspot.

Slew limiting is present in the pole model but inactive at these sample rates
(the per-sample slew allowance exceeds the ±8 V clamp).

## Transparent optimizations

Applied to every quality mode:

1. Skip the implicit solver and resonance mix when feedback gain is 0.
2. Solver previews use pole state snapshots instead of copying stage objects.
3. Skip slew limiting when it cannot affect the clamped voltage range.

These do not change High’s resonant path. Zero-resonance CPU dropped to about
one tenth of the resonant High cost in the isolated bench (0.85 µs vs 8.4 µs
per internal sample, Release, x64).

Rejected as High changes:

* Identity-simplifying the cutoff CV round-trip (`log2`/`pow`) — not bit-identical.
* Reducing High solver iterations or oversampling.
* SIMD across the four serial feedback stages.
* Sample-and-hold coefficients (control rate is already 40-sample blocks).

## Isolated filter CPU (Release, median ns / internal sample)

Internal rate ~39.216 kHz, 16384-sample saw (self-osc: low-level sine), 7 repeats.

| Scene | High 4×/8 | Normal 4×/4 | Eco 2×/4 |
|-------|-----------|-------------|----------|
| Low resonance | 847 | 847 | 427 |
| High resonance | 8407 | 4726 | 2391 |
| High cutoff + resonance | 8524 | 4789 | 2419 |
| Envelope sweep | 8539 | 4852 | 2531 |
| LFO modulation | 8469 | 4848 | 2529 |
| Self-oscillation | 8309 | 4667 | 2351 |

Relative to High at high resonance: Normal ≈ 44% less filter CPU, Eco ≈ 72% less.

Host sample rate does not change this internal-rate cost. Larger host blocks
do not reduce filter work per second of audio.

1× oversampling was measured and rejected: self-oscillation max sample error
vs High was 0.63, and high-cutoff error was clearly larger than Eco 2×.

## Error vs High (same scenes)

| Scene | Normal max / RMS | Eco max / RMS |
|-------|------------------|---------------|
| Low resonance | 0 / 0 | 0.0033 / 0.0010 |
| High resonance | 0.00016 / 0.000085 | 0.0031 / 0.0014 |
| High cutoff + resonance | 0.0043 / 0.0026 | 0.018 / 0.0048 |
| Envelope sweep | 0.00024 / 0.000025 | 0.036 / 0.0022 |
| LFO modulation | 0.00024 / 0.000018 | 0.055 / 0.0036 |
| Self-oscillation | 0.038 / 0.015 | 0.30 / 0.11 |

All candidates stayed finite, with DC on the order of 1e-4.

Normal keeps High’s 4× oversampling and only shortens the implicit solver.
Eco also halves oversampling. Self-oscillation is the most sensitive case;
Eco will not match High’s pitch/phase there.

## Whole-plugin CPU

The filter is the inner loop of every native audio sample. At high resonance,
8.4 µs/sample × 39216 Hz is about 33% of one core for the filter alone.
Normal and Eco scale that inner loop by the table above. Oscillator, mixer
and SRC costs are unchanged, so whole-plugin saving is smaller than the
isolated-filter percentage and is largest on resonant patches.

## Final modes

| Mode | Oversample at internal rate | Solver iterations | Role |
|------|-----------------------------|-------------------|------|
| High | 4× | 8 | Current approved sound (default) |
| Normal | 4× | 4 | Near-High, meaningful CPU cut when resonance is up |
| Eco | 2× | 4 | Cheaper; still the same ladder, more alias/self-osc difference |

Stored as the non-automatable preference `filter_quality` in the existing
Swara XT settings file (`high` / `normal` / `eco`). Missing property → High.
Right-click **Filter Quality**. Changing mode resets filter state.

Public APVTS parameter count and IDs are unchanged.
