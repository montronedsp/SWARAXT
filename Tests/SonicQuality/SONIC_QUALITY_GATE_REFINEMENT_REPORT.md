# SWARA XT sonic-quality gate refinement

Measurements below were taken on Windows MSVC Release from the audit worktree, parent `2400e10`, production sources unchanged from `e555ef5`. Listening review is pending. Nothing in this report is a request to change production DSP.

## 1. Executive findings

No production DSP fault was established.

The previous 20 Hz `NEEDS REVIEW` is the shared-output AC coupling that the simplified schematic omitted. Against the coupling-inclusive linear model the residual at 20 Hz, −60 dBFS, is about **−0.00001 dB**.

Comb + and Comb − at control 0 are the same feedforward sum. With the post-DAC stage removed, a 0.25 sine becomes a 0.50 sine: **+6.017 dB**. That is the zero-delay tap (`lut_res_comb_delays[0] = 1024`, which is 0 modulo the 1024-sample line) added to the dry sample.

A linear cutoff sweep at block 512 differs from block 1 by **0.033 RMS** and **1 sample** of lag. The published cutoff trajectories themselves differ by **210 Hz RMS**, because a parameter is sampled once per host block and then slewed by the modeled CV. The audio difference is smaller than the control difference.

The extreme resonant patch stays at about **−0.001 dBFS** sample peak, with the 4× and 8× estimators agreeing, and with no sample above 1.0. Distortion at maximum controls reaches about **+4.6 dBTP** and does cross full scale. That is effect gain, not a broken safety clamp. The existing non-finite and unbounded-output gates still pass.

Extended result after this refinement: **PASS 273, FAIL 0, OBSERVED 264, NEEDS REVIEW 0, NOT APPLICABLE 18.**

## 2. Current test architecture

`Tests/SonicQuality/` builds `SwaraXTSonicQualityTests`.

Layer A calls `SwaraXtFilter::processInstrumentSample()` at `20e6/510`, then `BoardProcessor::processClassicFx()`. Post-DAC filtering is applied only when an effect is enabled, matching `SwaraXtEngine`. `processSample()` remains a separately labeled diagnostic render.

Layer B constructs `SwaraXtAudioProcessor` and renders MIDI through the voice, IR3109/VCA, optional FX, host SRC and DC blocker.

Quick CTest is the default. Extended is `SwaraXTSonicQualityTests --extended`, registered disabled. Artifacts, including WAVs, are written outside the source tree.

## 3. Corrections to the measurement infrastructure

- Silence and near-silence correlation now stay at lag 0. New self-tests lock that in.
- Maximum-correlation alignment of an inverted sine is tested separately from the zero-lag polarity check, so a half-period lag is not mistaken for a timing fault.
- The open-filter screen now reports both the simplified schematic and a coupling-inclusive linear magnitude (input AC, shared-output AC, VCA current-to-voltage pole, output AC, output pole). The qualified 1 kHz, amplitude `1e-4`, 1.5% gate is unchanged and still uses the simplified schematic.
- Comb stimuli can be sine, saw, impulse or noise, with post-DAC included or omitted.
- Automation comparisons record the cutoff value actually published at each block boundary.
- Headroom rows report the 4×/48-tap estimator and an 8×/96-tap window around the peak. The 8× result is an uncertainty check, not a certified meter.
- Repeated identical plugin renders are an exact-match reproducibility check.

## 4. Previous metrics and new metrics

| Item | Previous extended run | This run |
|---|---|---|
| PASS / FAIL / OBSERVED / NEEDS REVIEW / NOT APPLICABLE | 248 / 0 / 200 / 1 / 18 | 273 / 0 / 264 / 0 / 18 |
| 20 Hz, −60 dBFS gain | 0.817 versus simplified 0.879 | 0.817 versus coupled model, residual about −0.00001 dB |
| Comb + / Comb − at minimum controls | about +6.1 dB versus dry, same for both | +6.017 dB with post-DAC off; +6.110 dB with post-DAC on; still identical |
| Block 1 versus block 512 sweep | about 0.054 RMS, lag 1, on a shorter buffer | 0.033 RMS, lag 1, requested-cutoff difference 210 Hz RMS |
| Extreme patch sample peak | −0.0009 dBFS at 48 kHz | −0.0009 dBFS at 48 kHz; 4× and 8× estimators match; 0 samples above 1 |

## 5. 20 Hz response

Production path: `processInstrumentSample()` → `Ir3109BoardCore::process()`.

At resonance 0 the feedback OTA current is zero. The linear magnitude is the mid-band VCA gain, the four-pole response at maximum CV, the input RF pole, and these coupling magnitudes already present in `Ir3109BoardCore::prepare()`:

- input AC, `kInputTau`
- shared output AC, `kSharedOutputTau` (about 20 ms; this is the term that matters at 20 Hz)
- VCA I/V pole
- output AC, `33000 * 4.7e-6`
- output pole, `kOutputPoleTau`

Small-signal sweep, normal quality, cutoff 20 kHz, resonance 0, 2.5 s warmup below 80 Hz. Coupled-model error is in dB.

| Hz | Measured gain dB | Simplified-estimate error dB | Coupled-model error dB |
|---:|---:|---:|---:|
| 10 | −3.268 | −2.147 | −0.000007 |
| 15 | −2.202 | −1.080 | −0.000004 |
| 20 | −1.760 | −0.639 | −0.000010 |
| 40 | −1.290 | −0.168 | 0.000041 |
| 100 | −1.149 | −0.027 | 0.000144 |
| 1000 | −1.178 | 0.004 | 0.012 |

The 0.64 dB gap is the omitted shared-output high-pass. It is expected model behavior. The 1 kHz row remains inside the existing 1.5% hard gate when the amplitude is `1e-4`. The −60 dBFS row above is the separate sweep at amplitude 0.001 and is reported as `OBSERVED`.

No new hard threshold is proposed. The coupling formula is taken from the same component time constants as the model, so it is an explanation, not an independent oracle.

## 6. Comb gain

`BoardEffects::comb()` stores `sample >> 4` into an 8-bit delay and later adds `delayed * 16` to the dry sample. `lut_res_comb_delays[0]` is 1024. The line length mask is 1023, so that delay is zero and the read sees the sample just written. The output is approximately twice the input, before 12-bit clipping.

Measured on a 0.25 sine, post-DAC omitted: gain **+6.017 dB**, peak **0.500**. Comb + and Comb − match at feedback 0, because the sign applies only to the feedback term.

With the production post-DAC stage included, the same case is **+6.110 dB**. Feedback and shorter delays change the gain, as a comb should. At time CV 254 and feedback 255, Comb + reaches peak **1.042** (110 samples above full scale). A 0.9 sine through the zero-delay sum also crosses full scale. Those are 12-bit clip events inside the effect, recorded as `OBSERVED`.

Control 0 is not bypass. FX Off remains the identity path and is still a hard `PASS`.

## 7. Automation timing

The host publishes cutoff once per `processBlock`. `Ir3109BoardCore` then slews cutoff, resonance and VCA with their component time constants. The test does not tap those CV states.

For one linear law from 180 Hz to 6 kHz over 8192 samples at 48 kHz, the published stair-step versus a per-sample update has this requested-cutoff RMS, and the audio difference after best integer alignment is:

| Block | Requested-cutoff RMS | Audio RMS difference | Lag |
|---:|---:|---:|---:|
| 32 | 13 Hz | 0.0041 | 0 |
| 64 | 26 Hz | 0.0057 | 0 |
| 127 | 52 Hz | 0.011 | 0 |
| 256 | 105 Hz | 0.016 | 0 |
| 512 | 210 Hz | 0.033 | 1 |

The requests are not the same signal. The analog slew removes most of the stair-step from the audio. Two identical block-64 renders match exactly (`plugin.repeat.block64`, max difference 0).

## 8. Extreme-patch true peak

4×, 48-tap Blackman-sinc, compared with 8×, 96-tap on a window around the peak. Neither is a certified true-peak meter. On the self-test cosine they agree within 0.01 and both sit within 0.02 of the analytic crest.

Extreme patch: master 1, resonance 0.95, cutoff 1500 Hz, sub 127, noise 63, second saw. Post Mixer 0 dB.

| Rate | Sample peak dBFS | 4× dBTP | 8× dBTP | Samples above 1 |
|---:|---:|---:|---:|---:|
| 44.1 kHz | −0.011 | −0.001 | −0.001 | 0 |
| 48 kHz | −0.0009 | −0.0009 | −0.0009 | 0 |
| 96 kHz | −0.0011 | −0.0011 | −0.0011 | 0 |
| 192 kHz | −0.0012 | −0.0012 | −0.0012 | 0 |

Post Mixer −18 dB on that patch lowers the peak only to about **−1.11 dBFS**. The trim is before the filter, and the resonant path restores level. On a non-resonant saw the same control measures **−17.58 dB**, which is the quick-profile post-mixer check.

Distortion at parameter maximum, same oscillators, reaches about **+4.2 to +4.6 dBFS** sample peak and about **+4.6 dBTP**, with samples above full scale at every rate tested. The 4× and 8× estimates differ by up to about 0.3 dB on that dense clipped waveform, which is the estimator uncertainty to quote there. Output stays finite and inside the existing unbounded-output gate.

## 9. Measurement uncertainty

- Coherent rectangular projection of an exact bin-centred sine recovers amplitude to about `1e-6` in the self-test.
- Hann projection of a 1000.3 Hz sine at 48 kHz stays within 0.01 of the amplitude.
- THD and THD+N are separate. THD+N is reported only for coherent rectangular windows.
- True-peak self-test tolerance is 0.02. The 4× versus 8× difference on the cosine is within 0.01. On clipped distortion it can reach a few tenths of a dB.
- Coupling-model residuals from 10 Hz to 200 Hz are below 0.001 dB on this run. That does not make the formula an external calibration.
- Full-buffer DC includes the note onset. It is not a settled DC measurement.

## 10. Hard invariants and observations

Hard failures remain: non-finite samples, instrument-path magnitude above 8, unbounded plugin magnitude, broken latency, left/right mismatch, FX Off not identical to its input, silence on a note that should sound, unsupported-rate contract, the 1 kHz 1.5% schematic check, and exact repeat of an identical render.

Analog harmonics, open-filter rolloff, comb summation, block-sized automation, resonance coloration and effect peaks above full scale stay `OBSERVED`. Enabled FX stays `NOT APPLICABLE` as a dry path.

## 11. Listening package

Directory:

`D:/MontroneDSP_Studio/build-worktrees/scratch/swaraxt-sonic-quality/sonic-quality-extended/listening/`

Float32 WAV. `*-raw.wav` is unscaled evidence. `*-matched.wav` is scaled in the export only, toward peak 0.25. The manifest is `listening_manifest.csv` beside that folder's parent report.

| File stem | Role | Raw peak | Listening gain |
|---|---|---:|---:|
| `A-open-filter-1k` | 1 kHz open IR3109 sine | 0.0436 | 5.728 |
| `B-open-filter-20` | 20 Hz open IR3109 sine | 0.0408 | 6.126 |
| `reference-fx-off` | plugin saw, FX off | 0.536 | 0.466 |
| `candidate-comb-plus` | plugin saw, Comb +, controls 0 | 1.048 | 0.239 |
| `candidate-comb-minus` | plugin saw, Comb −, controls 0 | 1.048 | 0.239 |
| `automation-linear-block-1` | sweep, block 1 | 0.500 | 0.500 |
| `automation-linear-block-64` | sweep, block 64 | 0.500 | 0.500 |
| `automation-linear-block-512` | sweep, block 512 | 0.496 | 0.504 |
| `automation-jump-block-64` | cutoff jump | 0.520 | 0.481 |
| `automation-lfo-cutoff` | filter LFO | 0.559 | 0.447 |
| `headroom-extreme` | resonant patch | 1.004 | 0.249 |

Questions, unanswered until someone listens:

- After level match, does the 20 Hz tone differ from the 1 kHz tone by more than loudness?
- After level match, do Comb + and Comb − differ from FX Off, and from each other, at control 0?
- Do the three sweep block sizes differ as zipper, or only as timing of the same opening filter?
- Does the raw extreme file sound clamped? Use the raw file for headroom. The matched file is tone only.

No listening conclusion is claimed here.

## 12. Test counts and reproducibility

Release extended: PASS 273, FAIL 0, OBSERVED 264, NEEDS REVIEW 0, NOT APPLICABLE 18, about 20 seconds.

Release quick: PASS 14, FAIL 0, OBSERVED 9, NEEDS REVIEW 0, NOT APPLICABLE 2, about 1.2 seconds.

Debug quick: the same quick counts, exit 0.

`plugin.repeat.block64` max absolute difference is 0.

## 13. Existing regression results

Re-run on this tree, Release, exit 0, goldens unchanged:

- `SwaraXTAudioParityTests`
- `SwaraXTIR3109BoardTests`
- `SwaraXTHostSrcTests`
- `SwaraXTHostSrcExactParityTests`
- `SwaraXTDspFxNativeExactParityTests`
- `SwaraXTPresetStateRestoreTests`
- `SwaraXTFilterMultirateTests`

## 14. Remaining qualification limits

Linux, macOS and sanitizer builds were not run. The 4× estimator is not an ITU true-peak meter. Internal CV waveforms are not tapped. Solver iteration counters remain compiled out of production. Factory presets were not swept one by one; the plugin cases use one explicit saw patch on top of the factory initial state.

## 15. Suggested future tolerances

| Metric | Proposal | Reference | Uncertainty | Justification | False-positive risk | Platform dependence | Recommendation |
|---|---|---|---|---|---|---|---|
| 1 kHz gain, amplitude 1e-4, resonance 0 | relative error 0.015 | existing `SwaraXTIR3109BoardTests` schematic | self-test projection about 1e-6 | already qualified | low at this single point | low | keep, already a hard gate |
| 10–200 Hz versus coupling-inclusive model | none yet | same component time constants as the model | residuals here below 0.001 dB | explains the old 20 Hz gap | high if treated as an external oracle | low on this formula | defer |
| Comb control-0 gain | none | feedforward sum in `BoardEffects::comb` | 0.25 sine measured +6.017 dB without post-DAC | intended dry-plus-delayed sum | would fail a real comb | low | reject as a failure limit |
| Block-size sweep RMS | none | published cutoff stair-step plus CV slew | depends on sweep length | requests are not equivalent | high | host block timing | defer |
| True peak above 0 dBTP | none | 4× versus 8× estimators | up to a few tenths of a dB when clipped | effect gain can exceed full scale by design | would flag distortion and comb peaks | moderate | defer |

## 16. Source diff

This refinement stays inside `Tests/SonicQuality/`. Production sources are not modified. The diff is the follow-up commit on `audit/production-sonic-quality-gate` whose parent is `2400e10`.
