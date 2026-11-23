# Cutoff knob: sub-semitone path (Classic filter)

## Problem
The `filter.cutoff` parameter is continuous (10 Hz - 20 kHz) but `patch.filter_cutoff` is a
7-bit semitone code and the firmware hands the filter an 8-bit CV (24 steps per octave).
Automating the knob therefore moved the Classic filter in 100-cent steps. Measured on a
300 -> 6000 Hz sweep (48 kHz, host block 64), deviation of the rendered audio from the same
filter driven by an ideal continuous CV:

| resonance | sweep | before | after |
|---|---|---|---|
| 0.2 | 1 s / 4 s | -34 / -33 dB | -53 / -65 dB |
| 0.5 | 1 s / 4 s | -24 / -22 dB | -42 / -54 dB |
| 0.7 | 1 s / 4 s | -9 / -6 dB | -21 / -30 dB |

Maximum CV error against the ideal trajectory fell from about 1.0-1.1 byte to 0.17 byte
(1 s sweep) and 0.043 byte (4 s sweep), i.e. one host block of travel.

## Change
`SwaraXtEngine::applyParameters` computes the part of the knob the semitone code cannot
represent (`shruthiCutoffTrimBytes`, within +-1 CV byte). `updateFilterFromShruthi` adds it to
the firmware cutoff byte for the Classic filter only. Matrix, envelope and LFO modulation keep
their firmware resolution; the Dual SVF still consumes the unmodified byte; resonance is
unchanged.

A cutoff within 0.2 cent of a firmware code yields exactly 0.0, so every patch that stores its
cutoff as a code (all 40 imported Shruthi patches) renders bit-identically to before.

## Intentional sonic differences
* A cutoff that is not on the semitone grid now sounds as set, up to 50 cents away from the
  previous rounded value. 15 of the 57 factory programs (native and authored APVTS programs)
  move by at most 0.63 CV byte (31 cents); measured level change at most 0.2 dB peak, 0.6 dB RMS.
* Saved sessions and automation with off-grid cutoff values change by the same bound.

## Pinned references
`SwaraXTAudioParityTests` (`Tests/AudioRegression`) hashes for cases 04, 05, 06 (1200, 900,
650 Hz; off-grid) change by construction; cases 01-03, 07, 08 (20000 Hz, 5000 Hz, noise,
silence) are unchanged. Verified on Linux/GCC: the original engine passes 8/8, this change
passes 5/8. The pinned references are NOT updated here: requalifying them requires the
project's independent reference process.

## Tests
`Tests/Smoothing/SmoothingAuditTestsMain.cpp`:
* `testCutoffKnobSubSemitoneResolution`: nine cutoffs 10 cents apart inside one semitone cell
  must give nine distinct, strictly brightening renders at host blocks 64 and 1024
  (0/8 distinct before the change).
* `testKnobTrimGridAndRange`: all 128 codes have zero trim, trim stays within one byte and is
  monotonic across a cell, imported Shruthi factory patches have zero trim.

## Not covered
Firmware CV saturates at byte 255 (about 14.6 kHz for note 60), so the knob has no further
effect above that point; this is pre-existing. Resonance still has 64 firmware steps.
