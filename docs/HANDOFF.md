# SWARA XT PR #50 handover

## Identity

| Item | Value |
| --- | --- |
| Branch | `investigation/fx-off-dc-blocker` |
| Source SHA | `3458b62fa1e977e55933dabd3245434cb64dc8ca` |
| PR | https://github.com/montronedsp/SWARAXT/pull/50 |
| Status | Draft until hosted Windows + Linux qualification are green |
| Author identity | MontroneDSP `<152441454+montronedsp@users.noreply.github.com>` |

## Completed analog circuit work (frozen)

- One Dual ownership board: `DualSvfAnalogBoard` + pure `ShruthiSvf2p` core.
- V2164 audio cell before C14/R12 output low-pass.
- Paper/Eagle Dual calibration dimension retired; six unique Dual v03 cases.
- Classic FX-Off host 0.7 Hz DC blocker bypass (qualified differential).
- Dual FX-Off and any FX-On retain the host 0.7 Hz blocker.
- No FX redesign, latency, SIMD, or warning-cleanup campaign in this PR.

## DC protection architecture

- Classic FX-Off with clean history: bypass host `DcBlocker`.
- Dual selected, FX ≠ Off, board audio required, SRC contamination, panic/drain: retain blocker.
- Historical blocked Classic hashes preserved in `kClassicHostDcBlocked` and differential tests.
- Current Classic parity uses `kClassic` bypass hashes.

## Reference authority

| Platform | Package | Scope |
| --- | --- | --- |
| Windows | `windows-current-2b82a6f6abb197636dec3c748ae378bf5a2f6781f2cfcb0f51e76b1fe19e71f1.json` | `full-native-66` |
| Linux | `native-current-5f96980660fffedd672bb69b51fe60ef4745a619cbec34d8f5a8e5765c40ed78.json` | `full-native-66` |

Inventory: 8 Classic + 22 integrated + 30 Host SRC + 6 Dual v03 = 66.

Historical packages remain in `Tests/References/` unchanged. Dual v03 fingerprints currently bootstrap from Windows MSVC provenance; Linux Dual audio acceptance still requires hosted Linux corroboration.

See `Tests/References/CURRENT-NATIVE-REFERENCE.md`.

## Local preflight (this SHA)

Windows (`scripts/ci`):

```text
python -m unittest test_common test_native_reference test_windows_artifacts
→ Ran 168 tests, OK (skipped=62)
```

Also: `test_windows_current_reference` → Ran 33 tests, OK (skipped=24).

Linux suite must be run on genuine Linux with `core.autocrlf=false`:

```bash
cd scripts/ci
python -m unittest test_common test_native_reference test_dual_svf_reference test_windows_reference_policy
```

## Hosted qualification

Dispatch only against `3458b62fa1e977e55933dabd3245434cb64dc8ca`:

- Windows VST3 qualification
- Linux VST3 qualification

Do not rerun obsolete failures `46183351287` / `46183390091` (SHA `8a7b458`).

Results: *pending — fill after Actions complete.*

## Exact build commands (reuse existing trees when inputs unchanged)

Windows Release example (audit worktree):

```powershell
cmake --build Z:/SWARAXT-DC-AUDIT-builds/win-release --config Release --parallel 2
ctest --test-dir Z:/SWARAXT-DC-AUDIT-builds/win-release -C Release --output-on-failure
```

Prior local Release CTest: 161/161 at `2e6d2af` sources (DSP frozen since; reference-only change at `3458b62`).

## Remaining limitations / blockers

- Hosted Windows + Linux qualification not yet green for `3458b62`.
- Linux Dual v03 hashes are Windows-bootstrapped until Linux hosted admission corroborates them.
- macOS not qualified.
- Debug / ASan / pluginval-10 / Steinberg validator may still be outstanding locally; hosted gates are authoritative for merge readiness.
- Cache roundtrip fuzz FAIL, analyzer/tidy OPEN, SDK64 UNSUPPORTED remain historical open items outside this PR scope.

## Outstanding engineering (future missions — not this PR)

1. FX output circuit and comprehensive DC qualification.
2. Complete factory preset note-on/note-off DC analysis.
3. Arithmetic correctness and warning elimination.
4. Architectural simplification.
5. Latency reduction.
6. CPU optimization.

## Merge rule

Do not merge, publish packages, or start FX modernization without maintainer authorization after both platform qualifications pass. Mark the PR ready for review only when those gates are green.
