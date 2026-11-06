# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native Windows proof export and portable, same-workflow artifact admission.

The producer is trusted through successful workflow jobs and same-run artifact
transport, NOT through a self-signed checksum. This is not protection against a
compromised producer. Native compiler/JUCE/PE checks cannot be repeated on Linux.
"""
import os
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

import windows_current_reference as current

SCHEMA = 1
POLICY = 'windows-native-artifact-v1'
MAX_FILE = 128 * 1024 * 1024
MAX_TOTAL = 256 * 1024 * 1024
BASE_FILES = {
    'report.json': 'qualification-report', 'profile.json': 'source-profile',
    'ctest.xml': 'ctest-results', 'ctest-inventory.json': 'ctest-inventory',
    'actual-VST3.log': 'native-host', 'reference-negative-control.log': 'reference-negative',
    'build-provenance.json': 'native-build-proof',
    'native-reference/manifest.json': 'reference-manifest',
    'native-reference/manifest.sha256': 'reference-seal',
    **{'native-reference/' + n + '.json': 'reference-record' for n in current.ALL_TESTS},
}
REPORT_FIELDS = {
    'source_sha', 'run_id', 'platform', 'compiler', 'profile', 'decision',
    'test_count', 'commands', 'actual_native_vst3', 'native_reference_negative_control',
    'native_audio_reference', 'native_audio_reference_manifest_sha256',
}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def files_for(profile):
    require(profile in current.PROFILES, 'Unexpected native profile')
    files = dict(BASE_FILES)
    if profile == 'msvc-Release':
        files.update({'windows-native-reference-controls.log': 'native-negative-controls',
                      'pluginval.log': 'pluginval', 'validator-extensive.log': 'vst3-validator'})
    if profile == 'msvc-asan':
        files['asan-negative-control.log'] = 'sanitizer-sensitivity'
    return files


def read(root, name, limit=MAX_FILE):
    path = current.paths.trusted_file(root, name)
    require(path.stat().st_size <= limit, 'Oversized evidence: ' + name)
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, 'Oversized evidence: ' + name)
    return data


def canonical(data):
    value = current.decode(data)
    require(current.encode(value) == data, 'Noncanonical artifact JSON')
    return value


def valid_hash(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def validate_results(report, payload, identity, source):
    """Independent execution evidence checks, shared by producer and consumer."""
    profile = identity['profile']
    fields = REPORT_FIELDS | ({'pluginval_source', 'vst3sdk_tool', 'external_validators'}
                             if profile == 'msvc-Release' else set())
    if profile == 'msvc-asan':
        fields |= {'sanitizer_negative_controls'}
    require(set(report) == fields, 'Unexpected qualification report fields')
    require(report['source_sha'] == identity['candidate_sha'] and report['run_id'] == identity['run_id']
            and report['platform'] == 'Windows' and report['compiler'] == 'msvc'
            and 'msvc-' + report['profile'] == profile, 'Wrong report execution identity')
    require(report['decision'] == 'PASS' and report['actual_native_vst3'] == 'PASS'
            and report['native_reference_negative_control'] == 'PASS', 'Native qualification did not pass')
    commands = {}
    for entry in report['commands']:
        require(set(entry) == {'name', 'exit_code'} and isinstance(entry['name'], str)
                and entry['name'] not in commands and type(entry['exit_code']) is int,
                'Duplicate/malformed native command')
        commands[entry['name']] = entry['exit_code']
    required = ['harness-tests', 'configure', 'actual-VST3', 'ctest', 'build-regressions']
    require(all(commands.get(n) == 0 for n in required), 'Missing successful native command')
    count = report['test_count']
    require(type(count) is int and count >= 90, 'Incomplete full native CTest coverage')
    require(b'\0' not in payload['ctest.xml'] and b'<!DOCTYPE' not in payload['ctest.xml']
            and b'<!ENTITY' not in payload['ctest.xml'],
            'Unsupported XML declarations')
    cases = list(ET.fromstring(payload['ctest.xml'].decode('utf-8')).iter('testcase'))
    names = [c.get('name') for c in cases]
    require(len(cases) == count and len(set(names)) == count and all(names), 'Wrong CTest inventory')
    inventory = canonical(payload['ctest-inventory.json'])
    require(set(inventory) == {'tests'} and all(set(t) == {'name', 'properties'}
            and all(set(p) == {'name', 'value'} and p['name'] == 'DISABLED'
                    for p in t['properties']) for t in inventory['tests']), 'Malformed portable CTest inventory')
    require(set(t['name'] for t in inventory['tests']) == set(names)
            and len(inventory['tests']) == count, 'CTest inventory disagreement')
    require(not any(p['name'] == 'DISABLED' and p['value']
                    for t in inventory['tests'] for p in t.get('properties', [])), 'Disabled native test')
    for case in cases:
        require(case.get('status') == 'run' and not any(list(case.iter(t))
                for t in ('failure', 'error', 'skipped')), 'Failing/skipped native CTest')
        output = '\n'.join(x.text or '' for x in case.iter('system-out'))
        require(not any(d in output for d in current.DIAGNOSTICS), 'CTest runtime diagnostic')
        if case.get('name') in current.ALL_TESTS:
            current.validate_output(case.get('name'), output, 0, source)
    require(set(current.ALL_TESTS).issubset(names), 'Missing native reference execution')
    host = payload['actual-VST3.log'].decode('utf-8')
    require('PASS: actual plugin two-instance audio/editor/state/reopen lifecycle' in host
            and 'BOOLEAN_NORMALIZATION_PASS' in host
            and not any(m in host for m in (*current.DIAGNOSTICS, 'FAIL:', 'X Error of failed request:')),
            'Missing/failed native host evidence')
    negative = payload['reference-negative-control.log'].decode('utf-8')
    require(commands.get('reference-negative-control') == 1
            and 'Unauthorized Windows reference mutation' in negative, 'Missing native reference negative proof')
    if profile == 'msvc-Release':
        require(commands.get('windows-native-reference-controls') == 0
                and re.search(r'Ran 24 tests\b', payload['windows-native-reference-controls.log'].decode())
                and re.search(r'\nOK\s*$', payload['windows-native-reference-controls.log'].decode()),
                'Missing native negative-control proof')
        # Retain the existing full gate's validator source and result contracts.
        from common import SDK_SHA
        from pluginval_source import (SOURCE_SHA, VENDOR_SHA, FIX_SHA,
                                      BEFORE_SHA256, AFTER_SHA256, PORTABILITY_FILES)
        tool, sdk, external = report['pluginval_source'], report['vst3sdk_tool'], report['external_validators']
        require(sdk['source_sha'] == SDK_SHA and sdk['binary_sha256'] == external['vst3sdk_binary_sha256'],
                'Wrong SDK validator provenance')
        require(tool['source_sha'] == SOURCE_SHA and tool['vendor_juce_sha'] == VENDOR_SHA
                and tool['upstream_fix_sha'] == FIX_SHA and tool['before_sha256'] == BEFORE_SHA256
                and tool['after_sha256'] == AFTER_SHA256 and tool['compiler_portability'] == PORTABILITY_FILES
                and tool['binary_sha256'] == external['sha256'], 'Wrong pluginval provenance')
        required = ['configure-pluginval', 'build-pluginval', 'pluginval-tool-selftest', 'pluginval',
                    'configure-validator', 'build-validator', 'validator-selftest', 'validator-extensive']
        require(all(commands.get(n) == 0 for n in required), 'Missing successful validator command')
        log = payload['pluginval.log'].decode('utf-8')
        require(all(m in log for m in ('Num plugins found: 1', 'Strictness level: 10', 'v1.3.1', 'SUCCESS')),
                'Incomplete pluginval validation')
        result = re.search(r'Result: (\d+) tests passed, (\d+) tests failed', payload['validator-extensive.log'].decode())
        require(result is not None and int(result[1]) >= 100 and int(result[2]) == 0,
                'Incomplete extensive VST3 validation')
    if profile == 'msvc-asan':
        require(report['sanitizer_negative_controls'] == 'PASS'
                and commands.get('asan-control-build') == 0
                and commands.get('asan-negative-control') not in (None, 0)
                and 'AddressSanitizer' in payload['asan-negative-control.log'].decode(),
                'Missing sanitizer sensitivity proof')


def native_proof(source, identity, build, manifest):
    """Must execute on the producing Windows machine; no portable substitute."""
    require(os.name == 'nt', 'Native attestation requires Windows')
    current.validate_context(identity)
    package = current.load_package(source)
    provenance = current.toolchain(build, identity['profile'], package)
    commands = current.decode(read(build, 'compile_commands.json'))
    cpp = [e for e in commands if Path(e['file']).suffix.lower() in ('.cpp', '.cc', '.cxx')]
    require(cpp and all(re.search(r'(?:^|\s)/EHsc(?:\s|$)', e['command']) for e in cpp),
            'Missing native exception unwinding')
    return dict(**provenance, cmake_cache_sha256=current.digest(read(build, 'CMakeCache.txt')),
                configuration='Debug' if identity['profile'] == 'msvc-Debug' else 'Release',
                translation_units=len(commands), cpp_exception_unwinding=True,
                asan_all_units=identity['profile'] == 'msvc-asan', juce_pristine=True,
                executables={n: dict(sha256=e['executable_sha256'], machine='x86_64',
                                    file=n + '.exe') for n, e in manifest['tests'].items()})


def export(profile_root, source, identity, build):
    """Export only after all native checks succeed. Never exports a partial PASS."""
    root = Path(profile_root)
    manifest = current.admit_reference(root / 'native-reference', source, identity, build)
    proof = native_proof(source, identity, build, manifest)
    data = current.decode(read(root, 'report.json'))
    require(data.get('decision') == 'PASS', 'Native qualification is incomplete; no attestation exported')
    profile = identity['profile']
    fields = REPORT_FIELDS | ({'pluginval_source', 'vst3sdk_tool', 'external_validators'}
                             if profile == 'msvc-Release' else set())
    if profile == 'msvc-asan':
        fields |= {'sanitizer_negative_controls'}
    report = {k: data[k] for k in fields}
    report['commands'] = [{k: e[k] for k in ('name', 'exit_code')} for e in data['commands']]
    payload = {n: read(root, n) for n in files_for(profile) if n not in ('report.json', 'build-provenance.json')}
    payload['report.json'] = current.encode(report)
    payload['build-provenance.json'] = current.encode(proof)
    inventory = current.decode(payload['ctest-inventory.json'])
    # Original command/backtrace paths are native diagnostics, not portable
    # file references. Retain exactly the names and disabled flags being checked.
    payload['ctest-inventory.json'] = current.encode({'tests': [dict(name=t['name'],
        properties=[p for p in t.get('properties', []) if p['name'] == 'DISABLED'])
        for t in inventory['tests']]})
    validate_results(report, payload, identity, source)
    current.admit_reference(root / 'native-reference', source, identity, build, manifest)
    index = {n: dict(role=files_for(profile)[n], size=len(b), sha256=current.digest(b)) for n, b in payload.items()}
    attestation = dict(schema=SCHEMA, policy=POLICY, platform='Windows', **identity,
                       reference_package_sha256=current.PACKAGE_SHA256,
                       reference_manifest_sha256=current.digest(payload['native-reference/manifest.json']),
                       native_validation='PASS', native_proof=proof, index_sha256=current.digest(current.encode(index)))
    destination = root / 'windows-artifact'
    require(not destination.exists(), 'Artifact destination already exists')
    # All semantic verification precedes writing a new package; the final name
    # becomes visible only after the complete bounded files have been verified.
    temporary = root / 'windows-artifact.partial'
    require(not temporary.exists(), 'Partial artifact already exists')
    temporary.mkdir()
    try:
        for n, b in {**payload, 'index.json': current.encode(index),
                     'attestation.json': current.encode(attestation)}.items():
            p = temporary / n
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b)
        admit_profile(temporary, source, identity)
        temporary.rename(destination)
    except BaseException:
        shutil.rmtree(temporary)
        raise
    return attestation


def admit_profile(root, source, identity):
    """Pure portable admission. No compiler, build tree, PE or JUCE access."""
    current.validate_context(identity)
    require(current.source_identity(source) == identity['candidate_tree_sha256'], 'Wrong source-tree digest')
    package = current.load_package(source)
    a = canonical(read(root, 'attestation.json', 1024 * 1024))
    require(isinstance(a, dict) and set(a) == {'schema', 'policy', 'platform', *identity,
            'reference_package_sha256', 'reference_manifest_sha256', 'native_validation', 'native_proof',
            'index_sha256'}, 'Malformed attestation fields')
    require(type(a['schema']) is int and a['schema'] == SCHEMA and a['policy'] == POLICY
            and a['platform'] == 'Windows' and a['native_validation'] == 'PASS', 'Wrong attestation schema/result')
    require(all(a[k] == v for k, v in identity.items()), 'Wrong attestation execution identity')
    require(a['reference_package_sha256'] == current.PACKAGE_SHA256, 'Wrong reference package identity')
    index_bytes = read(root, 'index.json', 1024 * 1024)
    require(current.digest(index_bytes) == a['index_sha256'], 'Changed evidence index')
    index = canonical(index_bytes)
    expected = files_for(identity['profile'])
    require(set(index) == set(expected), 'Missing/extra evidence member')
    inventory = set()
    for p in Path(root).rglob('*'):
        require(not p.is_symlink() and not getattr(p, 'is_junction', lambda: False)(), 'Untrusted artifact link')
        if p.is_file():
            inventory.add(p.relative_to(root).as_posix())
        else:
            require(p.relative_to(root).as_posix() == 'native-reference', 'Unexpected artifact directory')
    require(inventory == {*expected, 'index.json', 'attestation.json'}, 'Missing/extra artifact file')
    payload = {}
    total = 0
    for n, role in expected.items():
        entry = index[n]
        require(set(entry) == {'role', 'size', 'sha256'} and entry['role'] == role
                and type(entry['size']) is int and 0 <= entry['size'] <= MAX_FILE
                and valid_hash(entry['sha256']), 'Malformed evidence index entry')
        b = read(root, n)
        total += len(b)
        require(total <= MAX_TOTAL and len(b) == entry['size'] and current.digest(b) == entry['sha256'],
                'Changed evidence file: ' + n)
        payload[n] = b
    require(canonical(payload['profile.json']) == identity, 'Wrong profile anchor')
    manifest_bytes = payload['native-reference/manifest.json']
    require(current.digest(manifest_bytes) == a['reference_manifest_sha256']
            and payload['native-reference/manifest.sha256'] == (a['reference_manifest_sha256'] + '\n').encode(),
            'Changed reference manifest')
    manifest = canonical(manifest_bytes)
    require(set(manifest) == {'schema', 'policy', 'package_sha256', 'toolchain', 'tests', *identity}
            and type(manifest['schema']) is int and manifest['schema'] == 1
            and manifest['policy'] == current.POLICY and manifest['package_sha256'] == current.PACKAGE_SHA256
            and all(manifest[k] == v for k, v in identity.items()), 'Wrong reference manifest identity')
    proof = canonical(payload['build-provenance.json'])
    require(proof == a['native_proof'] and set(proof) == {'compile_commands_sha256', 'compiler_sha256',
            'juce_sha', 'cmake_cache_sha256', 'configuration', 'translation_units', 'cpp_exception_unwinding',
            'asan_all_units', 'juce_pristine', 'executables'}, 'Missing native build-provenance proof')
    require(proof['compiler_sha256'] == current.ADMITTED_COMPILER_SHA256
            and proof['juce_sha'] == package['reference_juce_sha'] and proof['juce_pristine'] is True
            and proof['cpp_exception_unwinding'] is True and type(proof['translation_units']) is int
            and proof['translation_units'] >= 10 and valid_hash(proof['cmake_cache_sha256'])
            and valid_hash(proof['compile_commands_sha256']), 'Wrong native toolchain proof')
    require(proof['configuration'] == ('Debug' if identity['profile'] == 'msvc-Debug' else 'Release')
            and proof['asan_all_units'] is (identity['profile'] == 'msvc-asan'), 'Wrong native instrumentation/configuration')
    require(manifest['toolchain'] == {k: proof[k] for k in ('compile_commands_sha256', 'compiler_sha256', 'juce_sha')},
            'Native manifest/toolchain disagreement')
    require(set(proof['executables']) == set(manifest['tests']) == set(current.ALL_TESTS), 'Missing native executable identity')
    for n in current.ALL_TESTS:
        entry = manifest['tests'][n]
        require(set(entry) == {'record_file', 'record_sha256', 'executable', 'executable_sha256'}
                and entry['record_file'] == n + '.json' and valid_hash(entry['executable_sha256']), 'Malformed executable identity')
        # The original native location is metadata only, never opened on Linux.
        relative = entry['executable']
        require(isinstance(relative, str) and all(p not in ('.', '..') for p in relative.split('/'))
                and re.fullmatch(r'Tests/(?:[A-Za-z0-9_. -]+/)*' + re.escape(n) + r'\.exe', relative),
                'Nonportable executable metadata')
        require(proof['executables'][n] == dict(sha256=entry['executable_sha256'], machine='x86_64', file=n + '.exe'),
                'Wrong executable SHA metadata')
        record_bytes = payload['native-reference/' + n + '.json']
        require(current.digest(record_bytes) == entry['record_sha256'] and canonical(record_bytes)
                == {'schema': 1, 'test': n, 'records': package['tests'][n]['cases']}, 'Unauthorized reference mutation')
    report = canonical(payload['report.json'])
    require(report['native_audio_reference'] == manifest
            and report['native_audio_reference_manifest_sha256'] == a['reference_manifest_sha256'], 'Report/reference disagreement')
    validate_results(report, payload, identity, source)
    return report


def admit_matrix(evidence, source, candidate_sha, run_id, job_results):
    require(job_results == {'preflight': 'success', 'native': 'success'}, 'Trusted native jobs did not succeed')
    actual = current.subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    require(actual == candidate_sha, 'Wrong checkout candidate SHA')
    tree = current.source_identity(source)
    names = {'native-windows-' + p + '-' + candidate_sha: p for p in current.PROFILES}
    require({p.name for p in Path(evidence).iterdir()} == set(names), 'Missing/duplicate/unexpected profile directory')
    result = {}
    for folder, profile in names.items():
        root = Path(evidence) / folder
        require(root.is_dir() and not root.is_symlink(), 'Untrusted profile directory')
        identity = dict(candidate_sha=candidate_sha, candidate_tree_sha256=tree, profile=profile,
                        run_id=run_id, scope='full-native-72', architecture='x86_64')
        admit_profile(root, source, identity)
        result[profile] = 'PASS'
    return dict(source_sha=candidate_sha, run_id=run_id, platform='Windows',
                decision='WINDOWS_PORTABLE_GATE_PASS', profiles=result, distribution_approval=False)
