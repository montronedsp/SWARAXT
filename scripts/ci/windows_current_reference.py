# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Immutable current MSVC authority. Acquisition never renders expected audio."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import windows_reference_policy as paths

POLICY = 'windows-current-msvc-v1'
PACKAGE_SHA256 = '3023b0d75c2593f7c328249b8336bd3e5f3cfeed600aff99b9d40f1ab72c8be7'
PACKAGE_FILE = 'Tests/References/windows-current-' + PACKAGE_SHA256 + '.json'
ALL_TESTS = ('SwaraXTAudioParityTests', 'SwaraXTDspFxIntegratedExactParityTests',
             'SwaraXTHostSrcExactParityTests', 'SwaraXTDualSvfHeadlessTests')
PROFILES = ('msvc-Release', 'msvc-Debug', 'msvc-asan')
DIAGNOSTICS = ('Sanitizer', 'runtime error:', 'JUCE Assertion failure', 'Leaked objects detected:', 'DEADLYSIGNAL')
digest = paths.digest
decode = paths.strict_json


def encode(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode()


def checked(root, relative, expected):
    data = paths.trusted_file(root, relative).read_bytes()
    if not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected) or digest(data) != expected:
        raise RuntimeError('Windows reference checksum mismatch: ' + relative)
    return data


def load_package(source):
    package = decode(checked(source, PACKAGE_FILE, PACKAGE_SHA256))
    if (package['schema'] != 1 or package['policy'] != POLICY or package['scope'] != 'full-native-72'
            or set(package['tests']) != set(ALL_TESTS) or sum(len(t['cases']) for t in package['tests'].values()) != 72):
        raise RuntimeError('Wrong Windows reference package or full scope')
    for relative, expected in package['fixtures'].items():
        checked(source, relative, expected)
    return package


def source_identity(source):
    root = Path(source)
    names = ['CMakeLists.txt']
    for folder in ('Source', 'Tests', 'cmake', 'scripts/ci'):
        names += [p.relative_to(root).as_posix() for p in (root / folder).rglob('*')
                  if p.is_file() and '__pycache__' not in p.parts]
    h = hashlib.sha256()
    for name in sorted(names):
        h.update(name.encode() + b'\0' + digest(paths.trusted_file(root, name).read_bytes()).encode() + b'\n')
    return h.hexdigest()


def context(source, candidate_sha, profile, run_id, scope='full-native-72', architecture='x86_64'):
    value = dict(candidate_sha=candidate_sha, candidate_tree_sha256=source_identity(source),
                 profile=profile, run_id=run_id, scope=scope, architecture=architecture)
    validate_context(value)
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != candidate_sha:
        raise RuntimeError('Wrong Windows candidate source SHA')
    return value


def validate_context(value):
    if (not isinstance(value, dict)
            or set(value) != {'candidate_sha', 'candidate_tree_sha256', 'profile', 'run_id', 'scope', 'architecture'}
            or any(not isinstance(v, str) for v in value.values())
            or not re.fullmatch('[0-9a-f]{40}', value['candidate_sha'])
            or not re.fullmatch('[0-9a-f]{64}', value['candidate_tree_sha256'])
            or value['profile'] not in PROFILES or value['architecture'] != 'x86_64'
            or value['scope'] != 'full-native-72' or not re.fullmatch('[A-Za-z0-9_.-]{1,128}', value['run_id'])):
        raise RuntimeError('Wrong Windows source/profile/run/architecture/full scope')


def validate_output(name, text, code, source):
    package = load_package(source)
    if name not in ALL_TESTS:
        raise RuntimeError('Wrong Windows test identity')
    if any(s in text for s in DIAGNOSTICS):
        raise RuntimeError('Windows sanitizer/assertion diagnostic')
    if re.search(r'(^|\s)(?:FAIL(?:ED)?|ERROR)(?:\s|:|$)', text):
        raise RuntimeError('Windows unexpected FAIL')
    if code != 0:
        raise RuntimeError('Windows nonzero process exit')
    entry = package['tests'][name]
    records = {}
    terminals = 0
    for line in text.splitlines():
        if 'GOLDEN' in line:
            match = re.fullmatch(r'GOLDEN (classic|integrated|src) ([A-Za-z0-9 _.-]+) samples=([1-9][0-9]*) hash=([0-9a-f]{16})', line)
            if not match or match[1] != entry['grammar']:
                raise RuntimeError('Malformed Windows audio record')
            key, count, value = match[2], int(match[3]), match[4]
        elif line.startswith('DUAL_SVF_HEADLESS_PASS'):
            if name != ALL_TESTS[3] or line != 'DUAL_SVF_HEADLESS_PASS cases=12 samples=25600 repeats=2 gui=excluded':
                raise RuntimeError('Malformed Windows Dual terminal')
            terminals += 1
            continue
        elif 'DUAL_SVF_HEADLESS' in line:
            if name != ALL_TESTS[3] or not line.startswith('DUAL_SVF_HEADLESS '):
                raise RuntimeError('Malformed Windows Dual record')
            row = decode(line[len('DUAL_SVF_HEADLESS '):])
            if set(row) != {'case', 'samples', 'hash', 'mode', 'calibration', 'finite', 'deterministic', 'parameters_unchanged', 'repeats'}:
                raise RuntimeError('Malformed Windows Dual fields')
            key, count, value = row['case'], row['samples'], row['hash']
            if (not isinstance(key, str) or key not in entry['cases'] or row['finite'] is not True
                    or row['deterministic'] is not True or row['parameters_unchanged'] is not True
                    or type(row['repeats']) is not int or row['repeats'] != 2
                    or row['mode'] != ('Independent' if key.startswith('DualIndependent_') else 'Relative')
                    or row['calibration'] != key.split('_')[1] or type(count) is not int):
                raise RuntimeError('Wrong Windows Dual controls/invariants')
        else:
            continue
        if key in records:
            raise RuntimeError('Duplicate Windows case')
        records[key] = [count, value]
    if name == ALL_TESTS[3] and terminals != 1:
        raise RuntimeError('Missing/duplicate Windows Dual terminal')
    if set(records) != set(entry['cases']):
        raise RuntimeError('Missing/additional Windows case')
    if records != entry['cases']:
        raise RuntimeError('Windows sample count/hash mismatch')
    return {'schema': 1, 'test': name, 'records': records}


def toolchain(build, profile, package):
    data = paths.trusted_file(build, 'compile_commands.json').read_bytes()
    entries = decode(data)
    compilers = set()
    compiler_hashes = {}
    for e in entries:
        if Path(e['file']).suffix.lower() not in ('.c', '.cpp', '.cc', '.cxx'):
            continue
        command = e['command']
        match = re.match(r'"?([^"\n]*?cl\.exe)"?\s', command, re.I)
        if not match or not Path(match[1]).is_file():
            raise RuntimeError('Wrong Windows compiler identity')
        if match[1] not in compiler_hashes:
            compiler_hashes[match[1]] = digest(Path(match[1]).read_bytes())
        compilers.add(compiler_hashes[match[1]])
        if profile == 'msvc-asan' and '/fsanitize=address' not in command:
            raise RuntimeError('Uninstrumented Windows ASan translation unit')
        if profile == 'msvc-Debug' and (re.search(r'(?:/|-)DNDEBUG(?:=1)?(?:\s|$)', command) or '/Od' not in command):
            raise RuntimeError('Wrong Windows build configuration')
        if profile != 'msvc-Debug' and not re.search(r'/O[12x]', command):
            raise RuntimeError('Unoptimized Windows Release configuration')
    if compilers != {package['compiler_sha256']}:
        raise RuntimeError('Unqualified Windows compiler')
    cache = paths.trusted_file(build, 'CMakeCache.txt').read_text()
    expected_config = 'Debug' if profile == 'msvc-Debug' else 'Release'
    if not re.search(r'^CMAKE_BUILD_TYPE:[^=]+=' + expected_config + r'$' , cache, re.M):
        raise RuntimeError('Wrong Windows build configuration')
    match = re.search(r'^SWARAXT_JUCE_DIR:[^=]+=(.+)$', cache, re.M)
    if not match:
        raise RuntimeError('Missing Windows JUCE provenance')
    juce = Path(match[1].strip())
    juce_sha = subprocess.check_output(['git', '-C', str(juce), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(juce), 'status', '--porcelain'], text=True).strip()
    if juce_sha != package['reference_juce_sha'] or dirty:
        raise RuntimeError('Unqualified Windows JUCE checkout')
    return {'compile_commands_sha256': digest(data), 'compiler_sha256': next(iter(compilers)),
            'juce_sha': juce_sha}


def native_pe(data):
    if len(data) < 64 or data[:2] != b'MZ':
        raise RuntimeError('Expected Windows PE executable')
    offset = int.from_bytes(data[60:64], 'little')
    if data[offset:offset+6] != b'PE\0\0\x64\x86':
        raise RuntimeError('Expected native Windows x64 PE executable')


def stage_reference(profile_root, source, identity, executables, build_root):
    validate_context(identity)
    package = load_package(source)
    if source_identity(source) != identity['candidate_tree_sha256'] or set(executables) != set(ALL_TESTS):
        raise RuntimeError('Windows source/test inventory changed')
    provenance = toolchain(build_root, identity['profile'], package)
    root = Path(profile_root) / 'native-reference'
    if root.exists() or root.is_symlink():
        raise RuntimeError('Windows reference destination already exists')
    if decode(paths.trusted_file(profile_root, 'profile.json').read_bytes()) != identity:
        raise RuntimeError('Wrong Windows profile anchor')
    bindings = {}
    for name in ALL_TESTS:
        relative = Path(executables[name]).absolute().relative_to(Path(build_root).absolute()).as_posix()
        if Path(relative).name != name + '.exe':
            raise RuntimeError('Wrong Windows executable filename')
        binary = paths.trusted_file(build_root, relative).read_bytes();native_pe(binary)
        record = encode({'schema': 1, 'test': name, 'records': package['tests'][name]['cases']})
        bindings[name] = dict(record_file=name+'.json', record_sha256=digest(record), executable=relative,
                              executable_sha256=digest(binary))
    root.mkdir()
    for name in ALL_TESTS:
        (root/(name+'.json')).write_bytes(encode({'schema': 1, 'test': name, 'records': package['tests'][name]['cases']}))
    manifest = dict(schema=1, policy=POLICY, package_sha256=PACKAGE_SHA256, **identity, toolchain=provenance, tests=bindings)
    data=encode(manifest);(root/'manifest.json').write_bytes(data);(root/'manifest.sha256').write_bytes((digest(data)+'\n').encode('ascii'))
    return manifest


def admit_reference(root, source, identity, build_root, expected_manifest=None):
    validate_context(identity);package=load_package(source)
    actual_sha = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual_sha != identity['candidate_sha'] or source_identity(source)!=identity['candidate_tree_sha256']:
        raise RuntimeError('Windows candidate source identity changed')
    seal=paths.trusted_file(root,'manifest.sha256').read_bytes()
    if not re.fullmatch(b'[0-9a-f]{64}\n',seal):raise RuntimeError('Malformed Windows manifest checksum')
    manifest=decode(checked(root,'manifest.json',seal.decode().strip()))
    if (set(manifest)!={'schema','policy','package_sha256','toolchain','tests',*identity}
            or type(manifest['schema']) is not int or manifest['schema']!=1 or manifest['policy']!=POLICY
            or manifest['package_sha256']!=PACKAGE_SHA256):raise RuntimeError('Wrong Windows reference package/schema')
    if any(manifest[k]!=v for k,v in identity.items()):raise RuntimeError('Wrong Windows source/profile/run identity')
    if manifest['toolchain']!=toolchain(build_root,identity['profile'],package):raise RuntimeError('Wrong Windows toolchain binding')
    if expected_manifest is not None and manifest!=expected_manifest:raise RuntimeError('Windows producer manifest changed')
    if set(manifest['tests'])!=set(ALL_TESTS):raise RuntimeError('Incomplete Windows full scope')
    if {p.name for p in Path(root).iterdir()}!={'manifest.json','manifest.sha256',*(n+'.json' for n in ALL_TESTS)}:raise RuntimeError('Unexpected Windows reference files')
    for name,entry in manifest['tests'].items():
        if set(entry)!={'record_file','record_sha256','executable','executable_sha256'} or entry['record_file']!=name+'.json':raise RuntimeError('Wrong Windows record filename')
        record=decode(checked(root,entry['record_file'],entry['record_sha256']))
        if record!={'schema':1,'test':name,'records':package['tests'][name]['cases']}:raise RuntimeError('Unauthorized Windows reference mutation')
        if Path(entry['executable']).name!=name+'.exe':raise RuntimeError('Wrong Windows executable identity')
        native_pe(checked(build_root,entry['executable'],entry['executable_sha256']))
    return manifest


def main():
    p=argparse.ArgumentParser();p.add_argument('--reference-root',type=Path,required=True);p.add_argument('--test-name',choices=ALL_TESTS,required=True)
    p.add_argument('--candidate-sha',default=os.environ.get('GITHUB_SHA'));p.add_argument('--profile',default=os.environ.get('SWARAXT_NATIVE_PROFILE'))
    p.add_argument('--run-id',default=os.environ.get('GITHUB_RUN_ID'));p.add_argument('--build-root',type=Path);p.add_argument('--scope',default='full-native-72')
    p.add_argument('command',nargs=argparse.REMAINDER);a=p.parse_args();source=Path(__file__).resolve().parents[2]
    build=a.build_root or a.reference_root.parent/'build';identity=context(source,a.candidate_sha,a.profile,a.run_id,a.scope)
    manifest=admit_reference(a.reference_root,source,identity,build)
    expected=Path(build).absolute()/manifest['tests'][a.test_name]['executable']
    if len(a.command)!=1 or Path(a.command[0]).absolute()!=expected:raise RuntimeError('Wrong Windows executable command')
    result=subprocess.run([str(expected)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=300)
    print(result.stdout,end='');validate_output(a.test_name,result.stdout,result.returncode,source)
    admit_reference(a.reference_root,source,identity,build,manifest)
    print('WINDOWS_CURRENT_REFERENCE_PASS',a.test_name)
