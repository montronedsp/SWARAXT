# Current native reference contract

## Linux

The authenticated package is
`native-current-5f96980660fffedd672bb69b51fe60ef4745a619cbec34d8f5a8e5765c40ed78.json`.
Its SHA-256 and policy `linux-current-phase3-v2` are pinned independently in
`scripts/ci/current_reference.py`. Acquisition copies approved data; it never
executes a candidate to generate expected audio. Checksums alone cannot approve
changed or resealed reference records.

The full scope is `full-native-66` with exactly four native executable identities:

- SwaraXTAudioParityTests: 8 Classic cases (FX-Off host-blocker bypass).
- SwaraXTDspFxIntegratedExactParityTests: 22 integrated cases.
- SwaraXTHostSrcExactParityTests: 30 Host SRC cases.
- SwaraXTDualSvfHeadlessTests: 6 Dual SVF v03 cases.

The focused-current-audio-30 scope remains the first two identities only. A
focused success cannot pass the full gate. Version 2 explicitly replaces the
old GUI-bearing Dual identity; executables are never renamed or aliased in
manifests. Tests/Filter/DualSvfSection2TestsMain.cpp remains available for future
authorized GUI qualification. Historical Paper/Eagle Dual goldens remain sealed
under DualSvfCurrentLinuxGoldens.h / DualSvfCurrentLinuxProvenance.json.

### Classic FX-Off

`CurrentAudioGoldens.h` keeps `kClassicHostDcBlocked` (historical blocked-output
hashes) and `kClassic` (bypass hashes from differential commit
`3048e172ff35707dd6907cae737b663c42a25455`). ClassicDcDifferentialTests replay
both tables. `CurrentAudioReferenceProvenance.json` retains the Phase-3 blocked
authority bytes unchanged.

### Dual SVF v03

Six unique cases (Paper/Eagle calibration dimension retired). Package fingerprints
currently bootstrap from Windows MSVC DualSvfV03Goldens.h /
DualSvfV03WindowsProvenance.json. Hosted Linux qualification must corroborate or
reject cross-platform equality; Windows fingerprints are not silently treated as
Linux-qualified audio acceptance.

## Windows

The authenticated package is
`windows-current-2b82a6f6abb197636dec3c748ae378bf5a2f6781f2cfcb0f51e76b1fe19e71f1.json`,
pinned in `scripts/ci/windows_current_reference.py`, scope `full-native-66`.
Compiler admission and JUCE identity remain separate from audio acceptance.

Historical packages (`native-current-4864fa1b…`, `…505aef6c…`, `…651826bf…`,
`…e4560ecede…`, `windows-current-00ed3f55…`, `…3023b0d7…`, `…a3f0e0f6…`,
`…fbecdcbc…`) stay in-tree as provenance and are never rewritten.

## Trust boundary

Changing any package/pin requires independently justified reference qualification.
A newly rendered buffer alone is insufficient. Negative controls reject fixture
mutations, golden substitutions, historical mixer output, sanitizer diagnostics
and scope drift.
