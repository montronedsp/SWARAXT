# Dual v03 DC measurement

This pass repeats the Phase II method on Analog-SVF-v03. It does not replace the Phase I or Phase II figures in `FX_OFF_DC_MEASUREMENT_VALIDATION.md`, which describe `cb6198c` and `bd88113`.

**Source:** `f36cb5eb4e76aad8d8c662cdf352c5a7d860d696`  
**Build:** Windows x64, MSVC 19.38.33145, Release  
**Harness:** `SwaraXTFxOffDcBlockerAuditPhase2` with argument `v03`  
**Artifacts:** `Z:/SWARAXT-DC-AUDIT-builds/phase2-v03/`  
**Window:** 8 s sustain and 4 s release at 48 kHz, except the slow-release case. Decision metric is the signed cumulative mean at the end of sustain. The first 200 ms is reported separately so capacitor settlement is not called persistent DC. Note-off uses the non-dormant release cumulative mean. An empty quiet window is not evidence of DC.

The earlier Phase II label `dual_fxoff_serial` rendered Section 1. Serial routing is now actually selected.

The same binary, without the `v03` argument, repeated the published Phase II case list. Classic FX-Off end-of-sustain means matched that document: sub −87.2, saw −96.2, noise −114.1, square −87.3, max resonance −99.4, minimum cutoff −113.7, 96 kHz −85.9, distortion −26.1, all dBFS. The constant-input IR3109 residual remained −194.3 dBFS. The old Dual rows did not: the 30 s saw moved from −33.7 to −110.5 dBFS, and the 10 s serial case, now really serial, ended at −99.5 dBFS. Classic pre-blocker and post-blocker means stay within a few millionths, which is the FX-Off bypass. Dual post-blocker means stay small because the 0.7 Hz blocker is still applied.

## Sustain cumulative mean, pre-blocker

Ordinary LP/BP/HP combinations, note 48, cutoff 8 kHz, resonance 0.35, end near −117 to −156 dBFS. Section 1 LP ends at −123.0 dBFS after a −64.9 dBFS onset. Parallel LP+LP is the largest ordinary case, −117.0 dBFS. None of these is the old stable −33.7 dBFS bias.

| Case | Onset | 1 s | End of sustain | Release |
| --- | ---: | ---: | ---: | ---: |
| Section 1 LP | −64.9 | −89.0 | −123.0 | −102.5 |
| Serial LP>LP | −65.0 | −88.7 | −122.3 | −101.2 |
| Parallel LP+LP | −58.9 | −83.0 | −117.0 | −97.5 |
| Serial max resonance, note 24 | −44.3 | −59.3 | −79.2 | −65.8 |
| Low note 12 | −59.2 | −92.7 | −106.8 | −78.0 |
| Sub-oscillator | −62.6 | −75.0 | −91.5 | −79.6 |
| Noise | −66.7 | −90.8 | −137.7 | −101.9 |
| Slow VCA attack | −84.6 | −80.9 | −104.0 | −101.6 |
| Filter-envelope modulation | −64.9 | −89.4 | −123.8 | −103.5 |
| Factory preset 0 / 4 / 8 | −64.9 / −68.2 / −67.4 | −88.8 / −82.2 / −77.5 | −122.3 / −109.3 / −104.9 | −101.6 / −85.3 / −94.7 |

Serial max resonance is still moving at 8 s (−72.5 dBFS at 5 s, −79.2 at 8 s). Its complete-period mean is −84.2 dBFS. That is a long settlement, not a constant offset. The −80 dBFS Dual-bypass line is not met by this one case, and the host blocker stays on Dual regardless.

Post-blocker sustain means for the FX-Off cases sit at or below the pre-blocker means. Distortion is the exception that still needs the blocker: pre-blocker sustain mean −27.4 dBFS, post-blocker −58.3 dBFS. That is the existing FX distortion offset. Downstream FX code was not changed. This pass does not claim final-output DC safety. The panic-path raw SRC residue stays open.
