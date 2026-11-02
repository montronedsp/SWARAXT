# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows policy preflight. Current MSVC references require native qualification.

No Linux fingerprint is a Windows authority. The legacy path is admissible only
for its exact frozen drivers/headers; a current tree with changed fixtures fails
before cloning/building/executing that reference path.
"""
import hashlib
import json
from pathlib import Path
import re

BASELINE_SHA = 'acf08d6b8d0041d594ce0bf069038e91317b2670'
FROZEN_FILES = {
    'Tests/AudioRegression/AudioRegressionTestsMain.cpp': '78b1c856dcdf86ad8cf08155bb3434277c564994d143762762280e33c7b0bd78',
    'Tests/Filter/DualSvfSection2TestsMain.cpp': 'd504cfa82d5aeeb00ed4cab3720ef7a9a5eb1174d62ce718ec079333d86fd6e2',
    'Tests/Parity/HostSrcExactParityTestsMain.cpp': '455a164544acc91934d6f02add0c229a6b1d9f8213701db4323fbd8695928178',
    'Tests/Parity/DspFxIntegratedExactParityTestsMain.cpp': 'b7f0ca5aca426675a01665797757df4c64b24e49677ca0af20c7ef4c0826ff44',
    'Tests/Parity/ExactParityGoldens.h': '8dd39766b2154d33829a9525c1e81882526d70b25058dfe9b1de693a5d6312cf',
    'Tests/Filter/DualSvfSection2Goldens.h': '84977a4da4b656f0bb991e2782fc3e30ce7adbe139dd143da2170becaa7641a7',
}
TEST_NAMES = ('SwaraXTAudioParityTests', 'SwaraXTDualSvfSection2Tests',
              'SwaraXTHostSrcExactParityTests', 'SwaraXTDspFxIntegratedExactParityTests')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def strict_json(data):
    def pairs(items):
        value = {}
        for key, item in items:
            if key in value:
                raise RuntimeError('Duplicate Windows manifest key')
            value[key] = item
        return value
    try:
        return json.loads(data, object_pairs_hook=pairs,
                          parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (ValueError, UnicodeError) as error:
        raise RuntimeError('Malformed Windows manifest JSON') from error


def source_divergence(source):
    differences = []
    for relative, expected in FROZEN_FILES.items():
        path = Path(source) / relative
        actual = digest(path.read_bytes()) if path.is_file() and not path.is_symlink() else None
        if actual != expected:
            differences.append({'file': relative, 'expected': expected, 'actual': actual})
    return differences


def require_frozen_source(source):
    differences = source_divergence(source)
    if differences:
        raise RuntimeError('WINDOWS_NATIVE_QUALIFICATION_REQUIRED: frozen driver/header identity is stale: '
                           + ', '.join(item['file'] for item in differences))


def trusted_file(root, relative):
    """Portable Windows path checks, including directory links/junctions."""
    if not isinstance(relative, str) or '\\' in relative or ':' in relative or Path(relative).is_absolute() or any(part in ('', '.', '..') for part in relative.split('/')):
        raise RuntimeError('Windows reference path escape')
    root = Path(root).absolute()
    if '..' in root.parts:
        raise RuntimeError('Windows reference path escape')
    path = root / relative
    for part in (root, *root.parents, path, *path.parents):
        if part.is_symlink() or getattr(part, 'is_junction', lambda: False)():
            raise RuntimeError('Untrusted Windows reference link/junction')
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(root.resolve(strict=True)) or not resolved.is_file():
        raise RuntimeError('Windows reference path escape')
    return resolved


def admit_manifest(root, manifest, identity):
    if manifest.get('baseline_sha') != BASELINE_SHA:
        raise RuntimeError('Wrong Windows baseline source identity')
    for key in ('candidate_sha', 'profile', 'run_id', 'architecture'):
        if manifest.get(key) != identity[key]:
            raise RuntimeError('Wrong Windows source/profile/run/architecture identity')
    if identity['architecture'] != 'x86_64' or set(manifest.get('tests', {})) != set(TEST_NAMES):
        raise RuntimeError('Incomplete Windows manifest test identity')
    for name in TEST_NAMES:
        entry = manifest['tests'][name]
        if entry.get('record_file') != name + '.json':
            raise RuntimeError('Wrong Windows reference filename')
        path = trusted_file(root, entry['record_file'])
        if digest(path.read_bytes()) != entry.get('record_sha256'):
            raise RuntimeError('Windows reference checksum mismatch')
        strict_json(path.read_bytes())
    return manifest


def validate_current_output(text, code, expected=None):
    """Strict future current-policy primitive; no native Windows data supplied here.

Tests may pass explicitly synthetic records. Production must obtain an
independent native authority first; there is intentionally no Linux fallback.
"""
    if expected is None:
        raise RuntimeError('WINDOWS_NATIVE_QUALIFICATION_REQUIRED: no independently qualified current MSVC reference')
    if any(value in text for value in ('Sanitizer:', 'runtime error:', 'JUCE Assertion failure', 'Leaked objects detected:')):
        raise RuntimeError('Windows current sanitizer/assertion diagnostic')
    if 'FAIL:' in text:
        raise RuntimeError('Windows current failed assertion')
    if code != 0:
        raise RuntimeError('Windows current nonzero process exit')
    records = {}
    for line in text.splitlines():
        if 'GOLDEN' not in line:
            continue
        match = re.fullmatch(r'GOLDEN (?:classic|integrated|src) ([A-Za-z0-9 _.-]+) samples=([1-9][0-9]*) hash=([0-9a-f]{16})', line)
        if not match:
            raise RuntimeError('Malformed Windows current audio record')
        if match[1] in records:
            raise RuntimeError('Duplicate Windows current case')
        records[match[1]] = [int(match[2]), match[3]]
    if set(records) != set(expected):
        raise RuntimeError('Missing/additional Windows current case')
    if records != expected:
        raise RuntimeError('Windows current sample/hash mismatch')
    return records
