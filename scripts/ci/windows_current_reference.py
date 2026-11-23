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
PACKAGE_SHA256 = '00ed3f5558abdd3741dc49645d2a85ba5057649a7adb9515fc7b78434f58c9ff'
PACKAGE_FILE = 'Tests/References/windows-current-' + PACKAGE_SHA256 + '.json'
# Admitted for fresh qualification, not a claim of native audio acceptance.
# Verified by hosted run 43994991391 (ImageVersion 20261112.366.7).
# Previous admission (run 41700756992, SHA fe251ef5…, 19.44.35229.0) is no longer hosted.
# The immutable package retains its original compiler provenance and 72 values.
ADMITTED_COMPILER_SHA256 = 'e87e78b3ef6580b75aebda843f8a22e874612d9f766344046551399f898eb226'
ADMITTED_COMPILER_VERSION = '19.44.35230.0'
ADMITTED_TOOLSET_VERSION = '14.44.35207'
ALL_TESTS = ('SwaraXTAudioParityTests', 'SwaraXTDspFxIntegratedExactParityTests',
             'SwaraXTHostSrcExactParityTests', 'SwaraXTDualSvfHeadlessTests')
PROFILES = ('msvc-Release', 'msvc-Debug', 'msvc-asan')
DIAGNOSTICS = ('Sanitizer', 'runtime error:', 'JUCE Assertion failure', 'Leaked objects detected:', 'DEADLYSIGNAL')
# Independently pinned reviewed variants. Historical Windows fixtures stay sealed.
PATCH_BRIDGE_HISTORICAL_SHA256 = '53f6a903708ee5d0402a2b55bfda5e987cdf651293d1b56c7ae559753e219eab'
PATCH_BRIDGE_CURRENT_SHA256 = 'c6b8c73388fd1528ade8b710bf04e88ccf88463e31093f44fb93800f623e3505'
CURRENT_RENDER_CONTRACT_HISTORICAL_SHA256 = '1ae25418a1fa9d87d429688b6a66d2efb784301ffe897432ce5649ea3eb4bbcd'
CURRENT_RENDER_CONTRACT_CURRENT_SHA256 = 'd535b59cbbf7673b62e226f8ce098e27002cf207d3da194addeaa591f48faa39'
TEST_ENVIRONMENT_SHA256 = '4be3b0cadcad578a8b8413de4bed27cee3f8fca2c321311c506597cdf278968c'
digest = paths.digest
decode = paths.strict_json


def encode(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode()


def checked(root, relative, expected):
    data = paths.trusted_file(root, relative).read_bytes()
    if not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected) or digest(data) != expected:
        if relative == PACKAGE_FILE:
            # Working-tree checkouts may use CRLF line endings; normalize
            # to LF before hashing to match the immutable reference package.
            normalized = data.replace(b'\r\n', b'\n')
            if digest(normalized) == expected:
                return normalized
        raise RuntimeError('Windows reference checksum mismatch: ' + relative)
    return data


def checked_fixture(root, relative, expected):
    data = paths.trusted_file(root, relative).read_bytes()
    normalized = data.replace(b'\r\n', b'\n')
    if digest(normalized) == expected:
        return data
    if relative == 'Tests/Filter/DualSvfIntegrationAuditMain.cpp':
        # Linux current-header dispatch and absolute output paths do not change
        # the approved Windows render/check body. Reverse only these exact
        # edits, then authenticate every original byte against the fixed package.
        original = normalized.replace(b'#if defined(_MSC_VER) || defined(__linux__)\n',
                                      b'#if defined(_MSC_VER)\n')
        original = original.replace(
            b'    const auto out=std::filesystem::absolute(argc>1 ? argv[1] : "dual-svf-integration-audit");\n',
            b'    const std::filesystem::path out=argc>1 ? argv[1] : "dual-svf-integration-audit";\n')
        if digest(original) == expected:
            return data
    if relative == 'Tests/AudioRegression/AudioRegressionTestsMain.cpp':
        # Mission 06 replaced processSample() with processInstrumentSample() and
        # removed the stage capture fields from FilterCapture. Reverse only these
        # exact edits, then authenticate every original byte against the fixed
        # package. Working-tree checkouts may use CRLF line endings; normalize
        # to LF before hashing to match the immutable reference package.
        original = normalized.replace(
            b'struct FilterCapture {\n    std::vector<float> input;\n    std::vector<float> output;\n};',
            b'struct FilterCapture {\n    std::vector<float> input;\n    std::vector<float> stage1;\n    std::vector<float> stage2;\n    std::vector<float> stage3;\n    std::vector<float> stage4;\n    std::vector<float> output;\n};'
        )
        original = original.replace(
            b'    FilterCapture capture;\n    capture.input = source;\n    capture.output.reserve(source.size());\n    for (float s : source)\n    {\n        capture.output.push_back(filter.processInstrumentSample(s, 1.0f));\n    }\n    return capture;\n',
            b'    FilterCapture capture;\n    capture.input = source;\n    capture.stage1.reserve(source.size());\n    capture.stage2.reserve(source.size());\n    capture.stage3.reserve(source.size());\n    capture.stage4.reserve(source.size());\n    capture.output.reserve(source.size());\n    for (float s : source)\n    {\n        const float y = filter.processSample(s);\n        capture.stage1.push_back(filter.lastStageOutput(0));\n        capture.stage2.push_back(filter.lastStageOutput(1));\n        capture.stage3.push_back(filter.lastStageOutput(2));\n        capture.stage4.push_back(filter.lastStageOutput(3));\n        capture.output.push_back(y);\n    }\n    return capture;\n'
        )
        original = original.replace(
            b'    writeMetricsRow(metrics, "filter_input", measure(lowFilter.input, swaraxt::SwaraXtEngine::kInternalSampleRate));\n    writeMetricsRow(metrics, "post_filter_low_resonance", measure(lowFilter.output, swaraxt::SwaraXtEngine::kInternalSampleRate));\n',
            b'    writeMetricsRow(metrics, "filter_input", measure(lowFilter.input, swaraxt::SwaraXtEngine::kInternalSampleRate));\n    writeMetricsRow(metrics, "filter_stage_1_output", measure(lowFilter.stage1, swaraxt::SwaraXtEngine::kInternalSampleRate));\n    writeMetricsRow(metrics, "filter_stage_2_output", measure(lowFilter.stage2, swaraxt::SwaraXtEngine::kInternalSampleRate));\n    writeMetricsRow(metrics, "filter_stage_3_output", measure(lowFilter.stage3, swaraxt::SwaraXtEngine::kInternalSampleRate));\n    writeMetricsRow(metrics, "filter_stage_4_output", measure(lowFilter.stage4, swaraxt::SwaraXtEngine::kInternalSampleRate));\n    writeMetricsRow(metrics, "post_filter_low_resonance", measure(lowFilter.output, swaraxt::SwaraXtEngine::kInternalSampleRate));\n'
        )
        if digest(original) == expected:
            return data
    if (relative == 'Source/Engine/PatchBridge.cpp'
            and expected == PATCH_BRIDGE_HISTORICAL_SHA256
            and digest(normalized) == PATCH_BRIDGE_CURRENT_SHA256):
        # CL-32 jlimit(1,4) is identity for historical arp.octaves=1 captures.
        before = b'seq->arp_range = static_cast<uint8_t>(ParameterCache::loadInt(cache.arpOctaves));'
        after = b'seq->arp_range = static_cast<uint8_t>(juce::jlimit(1, 4, ParameterCache::loadInt(cache.arpOctaves)));'
        if normalized.count(after) == 1:
            original = normalized.replace(after, before, 1)
            if original != normalized and digest(original) == expected:
                return data
    if (relative == 'Tests/Support/CurrentRenderContract.h'
            and expected == CURRENT_RENDER_CONTRACT_HISTORICAL_SHA256
            and digest(normalized) == CURRENT_RENDER_CONTRACT_CURRENT_SHA256):
        # MSVC _dupenv_s matches getenv for this capture: unset yields nullptr,
        # a set value is copied into juce::String before the next lookup.
        try:
            helper = paths.trusted_file(root, 'Tests/Support/TestEnvironment.h').read_bytes()
        except (RuntimeError, OSError) as error:
            raise RuntimeError('Windows reference checksum mismatch: Tests/Support/TestEnvironment.h') from error
        if digest(helper.replace(b'\r\n', b'\n')) != TEST_ENVIRONMENT_SHA256:
            raise RuntimeError('Windows reference checksum mismatch: Tests/Support/TestEnvironment.h')
        include_after = b'#include "TestEnvironment.h"\n'
        include_before = b'#include <cstdlib>\n'
        call_after = b'environment("SWARA_CURRENT_REFERENCE_DIR")'
        call_before = b'std::getenv("SWARA_CURRENT_REFERENCE_DIR")'
        if normalized.count(include_after) == 1 and normalized.count(call_after) == 2:
            original = normalized.replace(include_after, include_before, 1).replace(call_after, call_before)
            if original != normalized and digest(original) == expected:
                return data
    raise RuntimeError('Windows reference checksum mismatch: ' + relative)


def load_package(source):
    package = decode(checked(source, PACKAGE_FILE, PACKAGE_SHA256))
    if (package['schema'] != 1 or package['policy'] != POLICY or package['scope'] != 'full-native-72'
            or set(package['tests']) != set(ALL_TESTS) or sum(len(t['cases']) for t in package['tests'].values()) != 72):
        raise RuntimeError('Wrong Windows reference package or full scope')
    for relative, expected in package['fixtures'].items():
        checked_fixture(source, relative, expected)
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


def require_compiler(actual):
    if actual != ADMITTED_COMPILER_SHA256:
        raise RuntimeError('Unqualified Windows compiler')


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
    if compilers != {ADMITTED_COMPILER_SHA256}:
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
