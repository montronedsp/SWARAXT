#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact native comparison against separately built frozen production source.

Historical MSVC hashes remain unchanged. Only their identified hash assertions
may differ on a native toolchain; every other test assertion must still pass.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from common import architecture
from pathlib import Path
import re
import subprocess
import sys

BASELINE_SHA = 'acf08d6b8d0041d594ce0bf069038e91317b2670'
TESTS = {
    'SwaraXTAudioParityTests': 'Tests/AudioRegression/AudioRegressionTestsMain.cpp',
    'SwaraXTDualSvfSection2Tests': 'Tests/Filter/DualSvfSection2TestsMain.cpp',
    'SwaraXTHostSrcExactParityTests': 'Tests/Parity/HostSrcExactParityTestsMain.cpp',
    'SwaraXTDspFxIntegratedExactParityTests': 'Tests/Parity/DspFxIntegratedExactParityTestsMain.cpp',
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate_output(name, text, code, source):
    if code not in (0, 1):
        raise RuntimeError('Native reference crashed or failed to execute')
    if any(marker in text for marker in ('runtime error:', 'AddressSanitizer:', 'ThreadSanitizer:', 'JUCE Assertion failure')):
        raise RuntimeError('Sanitizer/assertion diagnostic in native reference')
    records = {}
    for case, count, value in re.findall(r'^GOLDEN (?:classic|src|integrated) (.+) samples=(\d+) hash=([0-9a-f]{16})$', text, re.M):
        if case in records:
            raise RuntimeError('Duplicate native audio case')
        records[case] = [int(count), value]
    failures = Counter(re.findall(r'^FAIL: (.+)$', text, re.M))
    expected_failures = Counter()
    if name == 'SwaraXTAudioParityTests':
        definitions = (source / TESTS[name]).read_text()
        expected = {case: [44100, value.lower().zfill(16)] for case, value in
                    re.findall(r'\{ "[^"\n]+", "([^"\n]+)"[^\n]+0x([0-9a-fA-F]+)ull', definitions)}
    elif name == 'SwaraXTDualSvfSection2Tests':
        if records:
            raise RuntimeError('Unexpected dual reference record format')
        for case, value in re.findall(r'^(Dual(?:Independent|Relative)_\S+) hash=([0-9a-f]{16})$', text, re.M):
            if case in records:
                raise RuntimeError('Duplicate dual native audio case')
            records[case] = [25600, value]
        definitions = (source / 'Tests/Filter/DualSvfSection2Goldens.h').read_text()
        expected = {}
        for mode, symbol, labels, message in (
                ('Relative', 'dualRelativeGoldens', ['Minus12', 'Center64', 'Plus12'], 'separate Relative audio references unchanged'),
                ('Independent', 'dualIndependentGoldens', ['Cut40_Res32', 'Cut90_Res32', 'Cut90_Res63'], 'separate Independent audio references unchanged')):
            body = re.search(symbol + r'[^=]*=\s*\{(.*?)\};', definitions, re.S)
            if body is None:
                raise RuntimeError('Historical dual reference declaration changed')
            values = re.findall(r'0x([0-9a-fA-F]+)', body[1])
            if len(values) != 6:
                raise RuntimeError('Incomplete historical dual reference')
            for index, calibration in enumerate(('Paper', 'Eagle')):
                group = []
                for offset, label in enumerate(labels):
                    key = 'Dual' + mode + '_' + calibration + '_' + label
                    expected[key] = [25600, values[index * 3 + offset].lower().zfill(16)]
                    group.append(key)
                if any(records.get(key) != expected[key] for key in group):
                    expected_failures[message] += 1
    else:
        symbol = 'kSrc' if name == 'SwaraXTHostSrcExactParityTests' else 'kIntegrated'
        definitions = (source / 'Tests/Parity/ExactParityGoldens.h').read_text()
        body = re.search(r'inline constexpr Entry ' + symbol + r'\[\] = \{(.*?)\};', definitions, re.S)
        if body is None:
            raise RuntimeError('Historical reference declaration changed')
        expected = {case: [int(count), value.lower().zfill(16)] for case, value, count in
                    re.findall(r'\{"([^"]+)", 0x([0-9a-fA-F]+)ull, (\d+)\}', body[1])}
    if not expected or set(records) != set(expected):
        raise RuntimeError('Missing or additional native audio cases')
    for case in expected:
        if records[case][0] != expected[case][0]:
            raise RuntimeError('Audio sample count differs from the frozen contract')
        if name != 'SwaraXTDualSvfSection2Tests' and records[case] != expected[case]:
            expected_failures[case] += 1
    if failures != expected_failures:
        raise RuntimeError('A non-hash invariant failed, or historical mismatch accounting changed')
    if (code == 0) != (not failures):
        raise RuntimeError('Test exit status disagrees with its assertion results')
    return {'records': records, 'historical_hash_mismatches': sum(expected_failures.values())}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference-root', type=Path, required=True)
    p.add_argument('--test-name', choices=TESTS, required=True)
    p.add_argument('command', nargs=argparse.REMAINDER)
    args = p.parse_args()
    root = args.reference_root.resolve()
    manifest = json.loads((root / 'manifest.json').read_text())
    source = Path(__file__).resolve().parents[2]
    if manifest['baseline_sha'] != BASELINE_SHA or manifest['candidate_sha'] != os.environ.get('GITHUB_SHA'):
        raise RuntimeError('Native reference source provenance mismatch')
    if manifest['architecture'] != architecture() or not args.command:
        raise RuntimeError('Native reference architecture or command missing')
    if manifest['profile'] != os.environ.get('SWARAXT_NATIVE_PROFILE') or manifest['run_id'] != os.environ.get('GITHUB_RUN_ID'):
        raise RuntimeError('Native reference profile or workflow run mismatch')
    command = Path(args.command[0]).resolve()
    if command.stem != args.test_name:
        raise RuntimeError('Unexpected native regression executable')
    entry = manifest['tests'][args.test_name]
    record_file = root / entry['record_file']
    if sha(record_file) != entry['record_sha256']:
        raise RuntimeError('Native reference record checksum changed')
    result = subprocess.run(args.command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end='')
    candidate = validate_output(args.test_name, result.stdout, result.returncode, source)
    baseline = json.loads(record_file.read_text())
    if candidate['records'] != baseline['records']:
        raise RuntimeError('Actual native audio differs from independently built frozen upstream source')
    print('NATIVE_BASELINE_EXACT_PASS', args.test_name, 'cases=' + str(len(candidate['records'])),
          'baseline=' + BASELINE_SHA, 'historical_mismatches=' + str(candidate['historical_hash_mismatches']))


if __name__ == '__main__':
    main()
