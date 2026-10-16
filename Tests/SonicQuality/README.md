# Production sonic quality gate

This suite measures the shipping SWARA XT signal path. It does not modify oscillators, the IR3109 model, effects, gain, oversampling, host SRC, or presets.

## Paths

Layer A drives `SwaraXtFilter::processInstrumentSample()` at `20e6/510`, then the Classic FX insert (`BoardProcessor::processClassicFx` and, when an effect is enabled, `BoardPostDacFilter`). `processSample()` is measured once and labeled diagnostic. It is not the instrument path.

Layer B constructs `SwaraXtAudioProcessor` and renders MIDI through the voice, mixer, IR3109/VCA, optional FX, host SRC and DC blocker.

## Profiles

`SwaraXTSonicQualityTests` is the quick profile: analytic measurement self-tests and a short production smoke set.

`SwaraXTSonicQualityTests --extended` adds the frequency, distortion, FX, rate, block-size, quality-mode and modulation matrices. CTest registers that profile as `SwaraXTSonicQualityExtended` with `DISABLED TRUE`, so an ordinary `ctest` run does not execute it.

Artifacts are written under the current directory in `sonic-quality-artifacts/`, or to `--output <directory>`. They are build-local and are not source files.

## Results

`PASS` and `FAIL` are reserved for analytic self-tests and explicit processing invariants: non-finite samples, the instrument-path magnitude clamp, documented latency, mono left/right copy, FX Off identity, and the already qualified 1 kHz small-signal schematic check.

`OBSERVED` is a measurement without an approved sonic limit. `NEEDS REVIEW` means a screening bound was crossed or a reference is missing. `NOT APPLICABLE` means the comparison has no meaningful dry equivalent. Enabled FX programs are not transparent: they quantize to 12 bits and then apply the post-DAC filter.

Open-filter gain is compared with the schematic small-signal estimate used by `SwaraXTIR3109BoardTests`. A fully open four-pole OTA model is not expected to be flat.

## True peak

Sample peak and true peak are separate. True peak is a 4x, 48-tap Blackman-windowed sinc with unity DC gain. The self-test requires it to recover a half-sample cosine crest within 0.02 of full scale. It is not a commercial oversampled loudness meter.
