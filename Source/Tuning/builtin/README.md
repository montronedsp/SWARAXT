# Factory tunings

The catalog order in `cmake/BuiltinTunings.cmake` preserves factory-selection
indices. Sessions restore embedded SWT bytes. Indices 0-5 retain the original files and converted data, including
the historical decimal-comma input in the existing EDO files. Newly authored
cent values use a decimal point and twelve decimal places. Final `2/1` or `3/1`
entries are periods, not additional interior pitches.

## Definitions

| Index | Menu name | Exact definition (tonic 1/1 implicit) |
|---:|---|---|
| 0 | 12-EDO | Retained source and SWT bytes; see `12-edo.scl`. |
| 1 | 19-EDO | Retained source and SWT bytes; see `19-edo.scl`. |
| 2 | 24-EDO | Retained source and SWT bytes; see `24-edo.scl`. |
| 3 | 31-EDO | Retained source and SWT bytes; see `31-edo.scl`. |
| 4 | 5-limit major | Retained source and SWT bytes; see `just-major.scl`. |
| 5 | tritave | Retained source and SWT bytes; see `tritave.scl`. |
| 6 | 5-EDO | 5 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 7 | 7-EDO | 7 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 8 | 9-EDO | 9 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 9 | 10-EDO | 10 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 10 | 11-EDO | 11 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 11 | 13-EDO | 13 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 12 | 15-EDO | 15 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 13 | 17-EDO | 17 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 14 | 22-EDO | 22 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 15 | 29-EDO | 29 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 16 | 41-EDO | 41 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 17 | 53-EDO | 53 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 18 | 72-EDO | 72 equal steps per 2/1; cents = 1200 k/N, k=1..N. |
| 19 | Pythagorean 12 (Eb-G#) | (3/2)^k, k=-3..8, octave reduced and sorted; period 2/1. |
| 20 | Pythagorean major | 9/8, 81/64, 4/3, 3/2, 27/16, 243/128, 2/1 |
| 21 | 5-limit diatonic major | 9/8, 5/4, 4/3, 3/2, 5/3, 15/8, 2/1 |
| 22 | 5-limit minor (9/5 seventh) | 9/8, 6/5, 4/3, 3/2, 8/5, 9/5, 2/1 |
| 23 | Harmonic series 8-16 | 9/8, 5/4, 11/8, 3/2, 13/8, 7/4, 15/8, 2/1 |
| 24 | 1/4-comma meantone | Fifth P - S/4; chain exponents -3..8, octave reduced; period 2/1. |
| 25 | 1/3-comma meantone | Fifth P - S/3; chain exponents -3..8, octave reduced; period 2/1. |
| 26 | 1/6-comma meantone | Fifth P - S/6; chain exponents -3..8, octave reduced; period 2/1. |
| 27 | Werckmeister III | Fifth-chain formula below; period 2/1. |
| 28 | Kirnberger III | Fifth-chain formula below; period 2/1. |
| 29 | Vallotti (1/6 Pyth. comma) | Fifth-chain formula below; period 2/1. |
| 30 | Bohlen-Pierce equal (13-ED3) | 13 equal steps per 3/1; cents = 1200 log2(3) k/13. |
| 31 | Bohlen-Pierce just | 27/25, 25/21, 9/7, 7/5, 75/49, 5/3, 9/5, 49/25, 15/7, 7/3, 63/25, 25/9, 3/1 |
| 32 | 8-ED3 | 8 equal steps per 3/1; cents = 1200 log2(3) k/8. |
| 33 | Gong (Pythagorean model) | 9/8, 81/64, 3/2, 27/16, 2/1 |
| 34 | 5-limit major pentatonic | 9/8, 5/4, 3/2, 5/3, 2/1 |
| 35 | Hirajoshi (Ellis theoretical) | 9/8, 6/5, 3/2, 8/5, 2/1 |
| 36 | Rast (24-EDO model) | 200.0, 350.0, 500.0, 700.0, 900.0, 1050.0, 2/1 |

Let P = 1200 log2(3/2), S = 1200 log2(81/80), and K = 12P - 8400.
The historical circle is C-G-D-A-E-B-F#-C#-G#-Eb-Bb-F-C, rooted at C.
Accumulate the first eleven fifths, reduce modulo 1200, sort, and append 2/1.

- Werckmeister III: narrow C-G, G-D, D-A and B-F# by K/4; all other fifths pure.
- Kirnberger III: narrow C-G, G-D, D-A and A-E by S/4; narrow F#-C# by K-S
  (the schisma); other fifths pure. This explicitly selects the fifth-chain form,
  rather than the different C#/G# placement in the archive's `kirnberger3.scl`.
- Vallotti: narrow F-C-G-D-A-E-B by K/6; remaining six fifths pure. This is the
  Pythagorean-comma form, not the distinct syntonic-comma historical variant.
- Meantones: use fifth P-S/d, d=4,3,6, on exponents -3..8 (Eb through G#).
  The closing wolf is intentional; these are not twelve equal fifths.

The just minor explicitly uses 9/5 for its seventh. Harmonic 8-16 is the
normalized consecutive partial series. These names specify mathematical
systems, without claiming one universal historical performance practice.

Gong is a fifth-generated pentatonic model. Hirajoshi follows Ellis's theoretical
row 110 (rounded cents 204,316,702,814), not the distinct observed rows 111-112.
Rast is an ascending 24-EDO model; contextual intonation, melodic behavior and
the alternative descending seventh are outside this static scale. The EDO and
8-ED3 names make no claim to represent a regional tradition.

## Reference and keyboard contract

All entries use the existing converter defaults: MIDI 69 = 440 Hz, degree 0
assigned to MIDI 60, one scale degree per MIDI key. Thus the reference is degree
9, not necessarily the tonic or a named traditional concert pitch. Existing
INPUT TUNE semantics classify negative MIDI mappings (keys 0-59 with this
mapping) as unmapped; SCALE separately supports signed degrees. QUANTIZED CV
and QUANTIZED MIDI select nearest log-frequency candidates across the existing
playable band, with the existing quantizer/tie policy. Mathematical midpoints can be
rounded toward either neighbour by the retained cache implementation; no
quantizer change is included in this content expansion. Very sparse tunings can exceed the
voice pitch range on high mapped keys; the existing voice-coordinate clamp
still applies. No KBM, parser, quantizer or voice policy is changed here.

All three pitch modes share the embedded catalog. Saved states embed SWT bytes;
the restored selection is identified as Imported, retaining its embedded name
and frequencies under the existing restoration policy. There is no runtime archive download or new asset format. Imported remains a
separate menu entry. Names describe interval relationships, not physical
instrument calibration or full maqam behavior.

## Provenance and redistribution

The 31 added SCL files and their comments are independently authored by
MontroneDSP under GPL-3.0-or-later. No third-party SCL file, archive description,
manual prose or downloaded archive is redistributed. The Scala archive was
consulted as a comparison source only: free download is not taken as permission
to relicense its contents. Existing six files are unchanged repository content.
Mathematical ratios and formulas are expressed independently; this does not
purport to license the third-party publications themselves.

References used to check the definitions:

- [Scala archive and specification](https://www.huygens-fokker.org/scala/downloads.html),
  [SCL format](https://www.huygens-fokker.org/scala/scl_format.html).
- [Bohlen-Pierce scale structures](https://www.huygens-fokker.org/bpsite/scales.html):
  original just sequence and equal division of the tritave.
- [Kontakt Library Scripts manual, pp.52-53](https://www.native-instruments.com/fileadmin/ni_media/downloads/manuals/kontakt/Kontakt_Factory_Library_Scripts_Manual_English_21062021.pdf):
  historical fifth placements; used for facts, not copied wording.
- [Ross Duffin on Vallotti](https://casfaculty.case.edu/ross-duffin/why-i-hate-vallotti-or-is-it-young/2-why-i-hate-vallotti-or-is-it-young-2/):
  distinction between comma forms and fifth chains.
- [Cris Forster, Musical Mathematics: China](https://www.chrysalis-foundation.org/musical-mathematics-pages/china-chin/):
  sanfen-sunyi and fifth-generated ratios.
- [Helmholtz/Ellis, On the Sensations of Tone, 1895 third English edition, p.519](https://archive.org/details/b28141532/page/519/mode/2up):
  theoretical Hiradioshi row 110. Public-domain historical text; no facsimile included.
- [Sami Asmar, ascending maqam intervals](https://www.turath.org/Resources/MaqamTrans.htm):
  Rast half-flat model, with explicitly different descending option.
- [US Copyright Office Circular 31](https://www.copyright.gov/circs/circ31.pdf):
  distinction between mathematical systems and protected expression.
