# Current Linux native reference contract, version 2

The authenticated package is
`native-current-651826bf168f415556324820fd9070ea1b8cc25307bdcdcfd2c614206eb4d660.json`.
Its SHA-256 and policy `linux-current-phase3-v2` are pinned independently in
`scripts/ci/current_reference.py`. Acquisition copies approved data; it never
executes a candidate to generate expected audio. Checksums alone cannot approve
changed or resealed reference records.

The full scope has exactly four native executable identities and 72 cases:

- SwaraXTAudioParityTests: 8 Classic cases.
- SwaraXTDspFxIntegratedExactParityTests: 22 integrated cases.
- SwaraXTHostSrcExactParityTests: 30 Host SRC cases.
- SwaraXTDualSvfHeadlessTests: 12 Dual SVF cases.

The focused-current-audio-30 scope remains the first two identities only. A
focused success cannot pass the full gate. Version 2 explicitly replaces the
old GUI-bearing Dual identity; executables are never renamed or aliased in
manifests. Tests/Filter/DualSvfSection2TestsMain.cpp remains available for future
authorized GUI qualification, with Linux current goldens selected by platform.
Windows continues using its own historical header and policy prerequisite.

## Current Dual SVF authority

The approved product maps public Sub/Noise 0–127 into firmware 0–63 with
`public >> 1`. Factory0 Saw bass explicitly selects public Sub64 and Noise0.
The current internal values are32 and0. The previous twelve hashes represented
the obsolete direct Sub64 firmware assignment. A private diagnostic changing
only those historical mixer assignments restores all twelve old hashes exactly.
This control is not included in production or this repository.

DualSvfCurrentLinuxProvenance.json records the95 public input parameters, exact
source/dependency identity, case inventory, old/new hashes, independent raw Q20
rehashing, first differing samples, deterministic repeats, cross-compiler raw
identity and production DSP invariants. Current positive runs pass both the
twelve exact references and the original shared DSP/control/rate tests. No
production DSP algorithm or gain law changed during reference renewal.

DualSvfCurrentLinuxGoldens.h is a Linux-only current contract. The historical
DualSvfSection2Goldens.h and the original content-addressed version1 package
remain unchanged as provenance. Existing Classic/FX authority, Host SRC data,
CurrentAudioGoldens.h and CurrentAudioReferenceProvenance.json are unchanged.
The new package authenticates current drivers, related source fixtures, golden
headers, provenance, sample counts and fingerprints. Changing any package/pin
requires independently justified reference qualification; a newly rendered
buffer alone is insufficient.

The new test excludes editor includes, guiAndStress(), the old main and snapshot
code at compile time, while reusing the original render/control functions.
Linux constructor traps reject Component construction. It checks selected
mode/calibration, serial routing,25600 samples, finite audio, unchanged parameter
values, repeatability and the independently specified mixer conversion. The
original GUI-capable target remains available; GUI tests were not run.

The source/binary-bound separate Dual validator is also used by the full Linux
parser. Admission requires the exact four executable identities/hashes, actual
source-tree fingerprint, approved package, profile, architecture, run and full
case inventory. It checks trusted non-symlinked paths, JSON/checksum integrity,
strict output records, nonzero exits and sanitizer/assertion diagnostics. Unit
and native negative controls reject historical mixer output, changed hashes,
missing/duplicate/additional cases, metadata changes, tampering and scope drift.

## Windows and review boundaries

Linux fingerprints are not an MSVC authority. Windows frozen-source identities
are checked before reference construction, execution and final admission.
Current drivers fail that historical predicate with
WINDOWS_NATIVE_QUALIFICATION_REQUIRED. Native Windows qualification remains
required; simulated policy tests cannot qualify Windows audio. Windows/macOS
JUCE selection remains9.0.1.

This is an engineering-review branch, not production or release approval.
Previously recorded cache roundtrip fuzz FAIL, analyzer/tidy INCOMPLETE/OPEN and
SDK64 UNSUPPORTED remain open. No GUI/DAW, installation, release, merge or PR is
implied by successful headless reference admission.
