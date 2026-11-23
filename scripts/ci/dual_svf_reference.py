# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Strict Dual admission shared by the explicit version2 full Linux gate.

The immutable package remains the authority for the twelve fingerprints. No
candidate execution generates expected data. The historical version1 identity is never silently aliased. Version2 explicitly
binds this source, new current Linux goldens and this executable identity.
"""
from pathlib import Path
import re
import current_reference as current

TEST_NAME = 'SwaraXTDualSvfHeadlessTests'
DRIVER = 'Tests/Filter/DualSvfHeadlessTestsMain.cpp'
# Updated only by explicit source review; never derived from candidate output.
DRIVER_SHA256 = '2768b0f7696968243c7acd01b9c5133ef7f2eff3ef6578b490a1dcfd05e1b577'
PREFIX = 'DUAL_SVF_HEADLESS '
TERMINAL = 'DUAL_SVF_HEADLESS_PASS cases=12 samples=25600 repeats=2 gui=excluded'
FIELDS = {'case', 'mode', 'calibration', 'samples', 'hash', 'finite',
          'deterministic', 'parameters_unchanged', 'repeats'}


def authority(source):
    current.checked_file(source, DRIVER, DRIVER_SHA256)
    package = current.load_package(source)
    entry = package['tests'][TEST_NAME]
    if entry['case_count'] != 12:
        raise RuntimeError('Unapproved Dual case count')
    return entry['cases']


def validate_output(text, code, source):
    expected = authority(source)
    if any(marker in text for marker in current.DIAGNOSTICS):
        raise RuntimeError('Dual sanitizer/assertion diagnostic')
    if 'FAIL:' in text:
        raise RuntimeError('Dual failed assertion')
    if code != 0:
        raise RuntimeError('Dual unexpected process failure')
    records = {}
    terminal = 0
    for line in text.splitlines():
        if line.startswith('DUAL_SVF_HEADLESS_PASS'):
            if line != TERMINAL:
                raise RuntimeError('Dual incorrect terminal case/sample count')
            terminal += 1
        elif 'DUAL_SVF_HEADLESS' in line:
            if not line.startswith(PREFIX):
                raise RuntimeError('Malformed Dual output')
            row = current.decode(line[len(PREFIX):].encode())
            if not isinstance(row, dict) or set(row) != FIELDS:
                raise RuntimeError('Malformed Dual record fields')
            name = row['case']
            if not isinstance(name, str) or name not in expected:
                raise RuntimeError('Unexpected Dual case')
            if name in records:
                raise RuntimeError('Duplicate Dual case')
            mode = 'Independent' if name.startswith('DualIndependent_') else 'Relative'
            calibration = name.split('_')[1]
            if row['mode'] != mode:
                raise RuntimeError('Wrong Dual Section2 mode')
            if row['calibration'] != calibration:
                raise RuntimeError('Wrong Dual calibration')
            if type(row['samples']) is not int or row['samples'] != expected[name][0]:
                raise RuntimeError('Invalid Dual sample count')
            if not isinstance(row['hash'], str) or not re.fullmatch('[0-9a-f]{16}', row['hash']) or row['hash'] != expected[name][1]:
                raise RuntimeError('Changed Dual audio hash')
            if row['finite'] is not True:
                raise RuntimeError('Nonfinite Dual audio')
            if row['deterministic'] is not True or type(row['repeats']) is not int or row['repeats'] != 2:
                raise RuntimeError('Nondeterministic Dual render')
            if row['parameters_unchanged'] is not True:
                raise RuntimeError('Unexpected Dual parameter change')
            records[name] = row
    if set(records) != set(expected):
        raise RuntimeError('Missing Dual case')
    if terminal != 1:
        raise RuntimeError('Missing/duplicate Dual terminal record')
    return {'test_name': TEST_NAME, 'scope': 'separate-dual-headless-12',
            'package_sha256': current.PACKAGE_SHA256, 'records': records}


def admit(source, binary, binary_sha256, source_tree_sha256, text, code):
    """Bind separate evidence to the exact executable and effective source tree."""
    binary = Path(binary).absolute()
    if binary.name != TEST_NAME:
        raise RuntimeError('Wrong Dual executable identity')
    current.checked_file(binary.parent, binary.name, binary_sha256)
    if current.source_identity(source) != source_tree_sha256:
        raise RuntimeError('Wrong Dual source identity')
    return validate_output(text, code, source)
