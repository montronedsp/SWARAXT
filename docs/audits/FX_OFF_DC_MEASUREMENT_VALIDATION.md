# FX-Off DC measurement validation (Phase II)

**Branch:** `investigation/fx-off-dc-blocker`  
**Baseline:** `bd88113`  
**Phase I commit:** `cb6198c`  
**Artifacts:** `Z:/SWARAXT-DC-AUDIT-builds/phase2/`

## Critical review of Phase I

`Tests/DcAudit/FxOffDcBlockerAuditMain.cpp` treated finite-window arithmetic means as persistent DC. Independent review and reanalysis identified:

| Problem | Effect on Phase I |
| --- | --- |
| A. MIDI-note period windows ignore Classic/HD sub increment (`set_increment(inc >> 1)`) | `classic_fxoff_sub` used 32.5 sub periods → incomplete-cycle bias |
| B. 250 ms / 500 ms quiet windows | Misread C12-class coupling transients as DC |
| C. Noise quiet gate | Single finite quiet window without uncertainty |
| D. `quiet_n = 0` | Inferred Dual “cannot become silent”; dormancy/ truncation also yield empty quiet windows |
| E. Replay compare over `min(host, postDc)` only | Prefix equality ≠ complete timeline proof |
| F. ~20 short cases | Not comprehensive qualification |

### Synthetic ground truth

| Test | Result |
| --- | --- |
| Known +0.25 DC | Detected |
| Zero-mean saw + sub, 32.5 sub-period window | **False DC ≈ −47 dBFS** |
| Exact combined-period zero-mean buffer | No persistent DC (≪ −120 dBFS) |
| C12-like exponential, Phase-I quiet window | **False DC ≈ −44.5 dBFS**; late window → 0 |
| IR3109 constant input | −194.3 dBFS |

Phase I’s −61.7 dBFS Classic verdict is therefore **not trustworthy** as persistent DC.

## Reanalysis of original captures

Preserved under `Z:/SWARAXT-DC-AUDIT-builds/dc-artifacts/captures/`.

| Case | Phase I reported | Corrected evidence | Artifact? | Origin | Verdict |
| --- | --- | --- | --- | --- | --- |
| `classic_fxoff_sub` | −61.7 dBFS | Cumulative mean → **−1.3e−6 (−118 dBFS)** by end of sustain; quiet −112 dBFS | **Yes** (period/transient) | IR3109 AC settle + incomplete sub periods | Not persistent DC |
| `classic_fxoff_noise` | −77.4 quiet | Cumulative end **−104 dBFS**; late release −143 | Mostly artifact | Stochastic / short quiet gate | Not proven persistent DC |
| `classic_fxoff_saw_48k` | −70 raw window | Cumulative end **−95 dBFS**; 1 s trailing window was period residue | **Yes** for raw window | Musical incomplete periods | Not persistent DC |
| `dual_fxoff_saw_48k` | −33.7 | Cumulative mean **stable −0.0206 (−33.7 dBFS)** | **No** | Dual path lacks Classic AC coupling | Genuine sustain DC |
| `dual_fxoff_maxres` | −33.4 | Same stable ≈ −0.021 | **No** | Dual systematic bias | Genuine sustain DC |
| `classic_fxon_dist` | −26.1 | Cumulative **+0.0495 (−26.1 dBFS)** | **No** | Integer FX distortion | Genuine; blocker required |

Plots: `Z:/SWARAXT-DC-AUDIT-builds/phase2/reanalysis/*_overview.png`.

## Corrected methods

1. **Signed cumulative mean** through sustain (primary asymptotic test).
2. **Autocorr / assumed-fund complete-period means** (sub fund = MIDI f0 / 2 when sub mix > 0).
3. **Host-timeline tap** (`SWARAXT_ENABLE_DC_AUDIT_TAPS`) immediately around `DcBlocker::process`, including dormancy rows.
4. **Synthetic false-positive suite** before SWARA cases.
5. Quiet residuals only as supporting evidence with longer skip/need; empty quiet ≠ proof of DC.

## Phase II long-duration results (pre-blocker)

Decision metric: `|signed cumulative mean|` at end of sustain.

| Case | cmean end | dBFS | Notes |
| --- | --- | --- | --- |
| `classic_fxoff_sub_30s` | −4.4e−5 | −87.2 | Converging |
| `classic_fxoff_saw_30s` | +1.5e−5 | −96.2 | |
| `classic_fxoff_noise_30s` | +2.0e−6 | −114.1 | |
| `classic_fxoff_square_10s` | +4.3e−5 | −87.3 | |
| `classic_fxoff_maxres_10s` | −1.1e−5 | −99.4 | |
| `classic_fxoff_mincut_10s` | −2.1e−6 | −113.7 | |
| `classic_fxoff_96k_10s` | +5.1e−5 | −85.9 | Worst Classic |
| `dual_fxoff_saw_30s` | −2.06e−2 | **−33.7** | Stable bias |
| `dual_fxoff_serial_10s` | −2.05e−2 | **−33.8** | |
| `classic_fxon_dist_10s` | +4.97e−2 | **−26.1** | |

**Classic FX-Off qualifies** (worst −85.9 dBFS < −80).  
**Dual FX-Off does not** (≈ −33.7 dBFS persistent).

## Classic vs Dual forensics (summary)

- Classic IR3109 already implements C10/C13/C12 AC coupling; constant-input residual −194 dBFS.
- Musical “DC” in Phase I was dominated by incomplete periods and coupling transients.
- Dual SVF is a digital SVF + VCA without that AC network; LP output passes a stable negative mean (~−0.021 at tested gains). That is a systematic Dual characteristic / missing coupling relative to Analog-SVF-v03 — **not** solved by deleting Classic’s host pole. Dual architecture correction is a separate topic.

## Acceptance criterion

The −80 dBFS target remains the documented engineering gate for **steady-state cumulative mean** during FX-Off sustain. It is not a loudspeaker-safety standard. Phase II uses that gate on the corrected metric only after rejecting known false-positive mechanisms.

## Limitations

- Not every oscillator algorithm × Dual route × factory preset was exercised.
- Noise uncertainty is summarized via long cumulative means, not full multi-seed confidence intervals.
- Dual DC root-cause localization beyond “no Classic-style AC coupling / stable LP mean” is incomplete.
