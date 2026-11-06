# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned Linux reference data and pure, fail-closed evidence admission.

No reference audio is produced by a candidate execution. See
Tests/References/CURRENT-NATIVE-REFERENCE.md for the trust boundary and scope.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess

POLICY = 'linux-current-phase3-v2'
PACKAGE_SHA256 = '651826bf168f415556324820fd9070ea1b8cc25307bdcdcfd2c614206eb4d660'
PACKAGE_FILE = 'Tests/References/native-current-' + PACKAGE_SHA256 + '.json'
AUDIO_TESTS = ('SwaraXTAudioParityTests', 'SwaraXTDspFxIntegratedExactParityTests')
ALL_TESTS = (*AUDIO_TESTS, 'SwaraXTHostSrcExactParityTests', 'SwaraXTDualSvfHeadlessTests')
SCOPES = {'full-native-72': ALL_TESTS, 'focused-current-audio-30': AUDIO_TESTS}
PROFILES = ('gcc-Release', 'gcc-Debug', 'clang-Release', 'clang-Debug',
            'clang-asan-ubsan', 'clang-tsan', 'clang-rtsan', 'clang-fuzz')
DIAGNOSTICS = ('runtime error:', 'Sanitizer:', 'Sanitizer CHECK', 'DEADLYSIGNAL',
               'JUCE Assertion failure', 'Leaked objects detected:', 'X Error of failed request:')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encode(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode('utf-8')


def decode(data):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise RuntimeError('Duplicate JSON key: ' + key)
            result[key] = value
        return result
    try:
        return json.loads(data.decode('utf-8'), object_pairs_hook=pairs,
                          parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (ValueError, UnicodeError) as error:
        raise RuntimeError('Malformed reference JSON') from error


def trusted_directory(root):
    root = Path(root).absolute()
    if '..' in root.parts:
        raise RuntimeError('Reference directory traversal')
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    fd = os.open('/', flags)
    try:
        for part in root.parts[1:]:
            new = os.open(part, flags, dir_fd=fd)
            os.close(fd)
            fd = new
    except OSError as error:
        raise RuntimeError('Missing or untrusted reference directory') from error
    finally:
        os.close(fd)


def safe_bytes(root, relative):
    """Open every directory component with NOFOLLOW; never follow evidence links."""
    relative = str(relative)
    if (not relative or '\\' in relative or Path(relative).is_absolute()
            or any(part in ('', '.', '..') for part in relative.split('/'))):
        raise RuntimeError('Reference path traversal or absolute path')
    root = Path(root).absolute()
    if '..' in root.parts:
        raise RuntimeError('Reference directory traversal')
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    fd = os.open('/', flags)
    try:
        for part in (*root.parts[1:], *relative.split('/')[:-1]):
            new = os.open(part, flags, dir_fd=fd)
            os.close(fd)
            fd = new
        file_fd = os.open(relative.split('/')[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
        with os.fdopen(file_fd, 'rb') as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise RuntimeError('Reference file is not regular')
            return stream.read()
    except OSError as error:
        raise RuntimeError('Missing or untrusted reference path: ' + relative) from error
    finally:
        os.close(fd)


def checked_file(root, relative, expected):
    data = safe_bytes(root, relative)
    if not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected) or digest(data) != expected:
        if relative == 'Tests/AudioRegression/AudioRegressionTestsMain.cpp':
            # Mission 06 replaced processSample() with processInstrumentSample() and
            # removed the stage capture fields from FilterCapture. Reverse only these
            # exact edits, then authenticate every original byte against the fixed
            # package. Working-tree checkouts may use CRLF line endings; normalize
            # to LF before hashing to match the immutable reference package.
            normalized = data.replace(b'\r\n', b'\n')
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
                return original
        raise RuntimeError('Reference checksum mismatch: ' + str(relative))
    return data



DUAL_ADAPTER_SHA256 = '7b6ef093df70cfe2de8de6561ddc42a8c0683dd3dcc72ece020f1220b4a6f861'
DUAL_WINDOWS_ADAPTER_SHA256 = '7ce29c25db9ddd4fc35b58a422333f817d1f1b70b564e41593a06b938c6a22a9'
DUAL_ORIGINAL_SHA256 = 'd504cfa82d5aeeb00ed4cab3720ef7a9a5eb1174d62ce718ec079333d86fd6e2'


def original_dual_driver(data):
    """Reverse only the reviewed compile-time guards/test observers, byte-exact."""
    text = data.decode('utf-8')
    text = text.replace('#if defined(__linux__) || defined(_MSC_VER)\n', '#if defined(__linux__)\n')
    platform_goldens = ('// SWARAXT_DUAL_PLATFORM_GOLDENS_BEGIN\n'
        '#if defined(__linux__)\n#include "DualSvfCurrentLinuxGoldens.h"\n#else\n'
        '#include "DualSvfSection2Goldens.h"\n#endif\n'
        '// SWARAXT_DUAL_PLATFORM_GOLDENS_END\n')
    text = text.replace(platform_goldens, '#include "DualSvfSection2Goldens.h"\n')
    include = ('// SWARAXT_DUAL_HEADLESS_INCLUDE_BEGIN\n'
        '#if defined(SWARAXT_DUAL_SVF_HEADLESS)\n'
        '#include "Plugin/PluginProcessor.h"\n#else\n'
        '#include "Plugin/PluginEditor.h"\n#endif\n'
        '// SWARAXT_DUAL_HEADLESS_INCLUDE_END\n')
    if text.count(include) != 1:
        raise RuntimeError('Unrecognised Dual headless include adapter')
    text = text.replace(include, '#include "Plugin/PluginEditor.h"\n')
    for label in ('SETUP', 'BLOCK', 'FINISH'):
        pattern = r'#if defined\(SWARAXT_DUAL_SVF_HEADLESS\) // SWARAXT_DUAL_' + label + r'_OBSERVER\n[^#]*?#endif // SWARAXT_DUAL_' + label + r'_OBSERVER\n'
        text, count = re.subn(pattern, '', text)
        if count != 1:
            raise RuntimeError('Unrecognised Dual test observer adapter')
    text = text.replace('#endif // SWARAXT_DUAL_GUI_GUARD\n\n', '#endif // SWARAXT_DUAL_GUI_GUARD\n')
    for label in ('GUI', 'PRINT', 'MAIN'):
        text = text.replace('#if !defined(SWARAXT_DUAL_SVF_HEADLESS) // SWARAXT_DUAL_' + label + '_GUARD\n', '')
        text = text.replace('#endif // SWARAXT_DUAL_' + label + '_GUARD\n', '')
    if digest(text.encode()) != DUAL_ORIGINAL_SHA256:
        raise RuntimeError('Dual DSP/GUI source differs from approved original driver')
    return text.encode()


def checked_driver(source, entry):
    if entry['driver'] != 'Tests/Filter/DualSvfSection2TestsMain.cpp':
        return checked_file(source, entry['driver'], entry['driver_sha256'])
    data = safe_bytes(source, entry['driver'])
    if digest(data) == entry['driver_sha256']:
        return data
    if entry['driver_sha256'] != DUAL_ORIGINAL_SHA256 or digest(data) not in (DUAL_ADAPTER_SHA256, DUAL_WINDOWS_ADAPTER_SHA256):
        raise RuntimeError('Unapproved Dual headless driver adapter')
    return original_dual_driver(data)


def load_package(source):
    source = Path(source)
    package = decode(checked_file(source, PACKAGE_FILE, PACKAGE_SHA256))
    if package['policy'] != POLICY or package['schema'] != 1:
        raise RuntimeError('Unsupported current reference package')
    authority = decode(checked_file(source, package['authority_file'], package['authority_sha256']))
    checked_file(source, package['current_golden_file'], package['current_golden_sha256'])
    for entry in package['source_fixtures']:
        # The sole MSVC-only guard change reverses byte-exactly to the pinned
        # Linux fixture. Do not republish Linux audio authority for this guard.
        if entry['path'] == 'Tests/Filter/DualSvfSection2TestsMain.cpp' and entry['sha256'] == DUAL_ADAPTER_SHA256:
            data = safe_bytes(source, entry['path'])
            restored = data.replace(b'#if defined(__linux__) || defined(_MSC_VER)\n', b'#if defined(__linux__)\n')
            if digest(data) == DUAL_WINDOWS_ADAPTER_SHA256 and digest(restored) == entry['sha256']:
                continue
        checked_file(source, entry['path'], entry['sha256'])
    if len(authority['rows']) != 30 or set(package['tests']) != set(ALL_TESTS):
        raise RuntimeError('Incomplete approved reference inventory')
    for name, entry in package['tests'].items():
        checked_driver(source, entry)
        if 'golden_file' in entry:
            checked_file(source, entry['golden_file'], entry['golden_sha256'])
        if len(entry['cases']) != entry['case_count']:
            raise RuntimeError('Invalid approved case count: ' + name)
    return package


def source_identity(source):
    """Bind the actual source/fixture/tooling bytes, including uncommitted edits."""
    source = Path(source)
    names = set()
    for directory in ('Source', 'Tests', 'cmake', 'scripts/ci'):
        for path in (source / directory).rglob('*'):
            if path.is_file() and '__pycache__' not in path.parts:
                names.add(path.relative_to(source).as_posix())
    names.add('CMakeLists.txt')
    h = hashlib.sha256()
    for name in sorted(names):
        h.update(name.encode() + b'\0' + digest(safe_bytes(source, name)).encode() + b'\n')
    return h.hexdigest()


def context(source, candidate_sha, profile, run_id, scope='full-native-72', architecture='x86_64'):
    value = {'candidate_sha': candidate_sha, 'candidate_tree_sha256': source_identity(source),
             'profile': profile, 'run_id': run_id, 'scope': scope, 'architecture': architecture}
    validate_context(value)
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != candidate_sha:
        raise RuntimeError('Candidate source identity does not match checkout HEAD')
    return value


def validate_context(value):
    if (set(value) != {'candidate_sha', 'candidate_tree_sha256', 'profile', 'run_id', 'scope', 'architecture'}
            or not isinstance(value['candidate_sha'], str) or not re.fullmatch('[0-9a-f]{40}', value['candidate_sha'])
            or not isinstance(value['candidate_tree_sha256'], str)
            or not re.fullmatch('[0-9a-f]{64}', value['candidate_tree_sha256'])
            or value['profile'] not in PROFILES or value['architecture'] != 'x86_64'
            or value['scope'] not in SCOPES or not isinstance(value['run_id'], str)
            or not re.fullmatch('[A-Za-z0-9_.-]{1,128}', value['run_id'])):
        raise RuntimeError('Invalid current source/profile/architecture/run identity')


def validate_output(name, text, code, source):
    package = load_package(source)
    if name not in package['tests']:
        raise RuntimeError('Unexpected native test identity')
    if any(marker in text for marker in DIAGNOSTICS):
        raise RuntimeError('Sanitizer/assertion diagnostic in current reference output')
    if re.search(r'(^|\s)(?:FAIL(?:ED)?|ERROR)(?:\s|:|$)', text):
        raise RuntimeError('Failed assertion in current reference output')
    if code != 0:
        raise RuntimeError('Nonzero current test process exit')
    entry = package['tests'][name]
    if entry['grammar'] == 'headless-dual-v1':
        # Version2 explicitly binds the new headless identity; no binary alias.
        import dual_svf_reference
        parsed = dual_svf_reference.validate_output(text, code, source)
        return {'schema': 1, 'test': name,
                'records': {case: [row['samples'], row['hash']] for case, row in parsed['records'].items()}}
    records = {}
    for line in text.splitlines():
        if 'GOLDEN' in line:
            found = re.fullmatch(r'GOLDEN (classic|integrated|src) ([A-Za-z0-9 _.-]+) samples=([1-9][0-9]*) hash=([0-9a-f]{16})', line)
            if not found or found[1] != entry['grammar']:
                raise RuntimeError('Malformed or unexpected current audio record')
            case, count, value = found[2], int(found[3]), found[4]
        elif 'DualIndependent_' in line or 'DualRelative_' in line:
            found = re.fullmatch(r'(Dual(?:Independent|Relative)_[A-Za-z0-9_]+) hash=([0-9a-f]{16})', line)
            if not found or entry['grammar'] != 'dual':
                raise RuntimeError('Malformed or unexpected Dual SVF record')
            # This unchanged C++ grammar omits sample count. The pinned driver
            # and golden contract bind the 100 x 256 render length explicitly.
            case, value = found[1], found[2]
            count = 25600
        else:
            continue
        if case in records:
            raise RuntimeError('Duplicate current audio case: ' + case)
        records[case] = [count, value]
    if set(records) != set(entry['cases']) or len(records) != entry['case_count']:
        raise RuntimeError('Missing or additional current audio cases')
    for case, expected in entry['cases'].items():
        if records[case][0] != expected[0]:
            raise RuntimeError('Current audio sample count mismatch: ' + case)
        if records[case][1] != expected[1]:
            raise RuntimeError('Current audio hash mismatch: ' + case)
    return {'schema': 1, 'test': name, 'records': records}


def expected_record(package, name):
    return {'schema': 1, 'test': name, 'records': package['tests'][name]['cases']}


def stage_reference(profile_root, source, identity, executables, build_root):
    """Acquire immutable approved records; do not execute any candidate here."""
    validate_context(identity)
    if source_identity(source) != identity['candidate_tree_sha256']:
        raise RuntimeError('Candidate source tree changed before reference acquisition')
    package = load_package(source)
    names = SCOPES[identity['scope']]
    if set(executables) != set(names):
        raise RuntimeError('Missing or additional test executable bindings')
    profile_root, build_root = Path(profile_root).absolute(), Path(build_root).absolute()
    # Verify directory trust before creating a new, never-overwritten result.
    trusted_directory(profile_root)
    trusted_directory(build_root)
    root = profile_root / 'native-reference'
    if profile_root.is_symlink() or not profile_root.is_dir() or root.exists() or root.is_symlink():
        raise RuntimeError('Reference destination must be a new trusted directory')
    bindings = {}
    for name in names:
        executable = Path(executables[name]).absolute()
        try:
            relative = executable.relative_to(build_root).as_posix()
        except ValueError as error:
            raise RuntimeError('Test executable escaped build root') from error
        if executable.name != name:
            raise RuntimeError('Wrong test executable identity')
        data = safe_bytes(build_root, relative)
        if not data.startswith(b'\x7fELF\x02\x01') or int.from_bytes(data[18:20], 'little') != 62:
            raise RuntimeError('Expected native x86_64 ELF test executable')
        record = encode(expected_record(package, name))
        bindings[name] = {'record_file': name + '.json', 'record_sha256': digest(record),
                          'case_count': len(package['tests'][name]['cases']),
                          'executable': relative, 'executable_sha256': digest(data)}
    root.mkdir()
    for name in names:
        (root / (name + '.json')).write_bytes(encode(expected_record(package, name)))
    manifest = {'schema': 2, 'policy': POLICY, 'package_sha256': PACKAGE_SHA256,
                'reference_source_sha': package['reference_source_sha'], **identity, 'tests': bindings}
    data = encode(manifest)
    (root / 'manifest.json').write_bytes(data)
    (root / 'manifest.sha256').write_text(digest(data) + '\n', encoding='ascii')
    return manifest


def admit_reference(root, source, identity, build_root, expected_manifest=None):
    """Authenticate both data and candidate bindings, including final admission."""
    validate_context(identity)
    if source_identity(source) != identity['candidate_tree_sha256']:
        raise RuntimeError('Candidate source identity changed')
    package = load_package(source)
    root, build_root = Path(root).absolute(), Path(build_root).absolute()
    seal = safe_bytes(root, 'manifest.sha256')
    if not re.fullmatch(b'[0-9a-f]{64}\n', seal):
        raise RuntimeError('Malformed manifest checksum')
    manifest = decode(checked_file(root, 'manifest.json', seal.decode().strip()))
    keys = {'schema', 'policy', 'package_sha256', 'reference_source_sha', 'tests', *identity}
    if not isinstance(manifest, dict) or set(manifest) != keys or type(manifest['schema']) is not int or manifest['schema'] != 2 or manifest['policy'] != POLICY:
        raise RuntimeError('Unsupported current reference manifest schema')
    if manifest['package_sha256'] != PACKAGE_SHA256 or manifest['reference_source_sha'] != package['reference_source_sha']:
        raise RuntimeError('Wrong approved reference source/package identity')
    if any(manifest[key] != value for key, value in identity.items()):
        raise RuntimeError('Mismatched candidate source/profile/architecture/run identity')
    if expected_manifest is not None and manifest != expected_manifest:
        raise RuntimeError('Reference manifest differs from recorded producer evidence')
    names = SCOPES[identity['scope']]
    if not isinstance(manifest['tests'], dict) or set(manifest['tests']) != set(names):
        raise RuntimeError('Missing or additional native test identity')
    if {path.name for path in root.iterdir()} != {'manifest.json', 'manifest.sha256', *(name + '.json' for name in names)}:
        raise RuntimeError('Unexpected reference filename inventory')
    for name in names:
        entry = manifest['tests'][name]
        if not isinstance(entry, dict) or set(entry) != {'record_file', 'record_sha256', 'case_count', 'executable', 'executable_sha256'} or entry['record_file'] != name + '.json':
            raise RuntimeError('Unexpected native reference filename or metadata')
        data = checked_file(root, entry['record_file'], entry['record_sha256'])
        record = decode(data)
        if (data != encode(expected_record(package, name)) or record != expected_record(package, name)
                or type(entry['case_count']) is not int or entry['case_count'] != len(package['tests'][name]['cases'])):
            raise RuntimeError('Reference records differ from immutable approved cases')
        relative = entry['executable']
        if not isinstance(relative, str) or Path(relative).name != name:
            raise RuntimeError('Wrong test executable identity')
        checked_file(build_root, relative, entry['executable_sha256'])
    return manifest


def execute_candidate(root, source, identity, build_root, name, command):
    manifest = admit_reference(root, source, identity, build_root)
    if name not in manifest['tests'] or len(command) != 1:
        raise RuntimeError('Unexpected candidate test command')
    expected = Path(build_root).absolute() / manifest['tests'][name]['executable']
    if Path(command[0]).absolute() != expected:
        raise RuntimeError('Wrong candidate test executable path')
    result = subprocess.run([str(expected)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors='strict', timeout=300)
    record = validate_output(name, result.stdout, result.returncode, source)
    # Detect an executable/source change while it was being exercised, too.
    admit_reference(root, source, identity, build_root, expected_manifest=manifest)
    return result, record


def main(tests=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-root', type=Path, required=True)
    parser.add_argument('--test-name', choices=ALL_TESTS, required=True)
    parser.add_argument('--candidate-sha', default=os.environ.get('GITHUB_SHA'))
    parser.add_argument('--profile', default=os.environ.get('SWARAXT_NATIVE_PROFILE'))
    parser.add_argument('--run-id', default=os.environ.get('GITHUB_RUN_ID'))
    parser.add_argument('--trusted-root', type=Path, default=os.environ.get('RUNNER_TEMP'))
    parser.add_argument('--build-root', type=Path)
    parser.add_argument('--scope', choices=SCOPES, default='full-native-72')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    if args.trusted_root is None:
        raise RuntimeError('Explicit trusted evidence root required')
    try:
        relative = args.reference_root.absolute().relative_to(args.trusted_root.absolute())
    except ValueError as error:
        raise RuntimeError('Reference directory escaped trusted evidence root') from error
    safe_bytes(args.trusted_root, (relative / 'manifest.json').as_posix())
    identity = context(source, args.candidate_sha, args.profile, args.run_id, args.scope)
    build = args.build_root or args.reference_root.parent / 'build'
    result, record = execute_candidate(args.reference_root, source, identity, build, args.test_name, args.command)
    print(result.stdout, end='')
    print('CURRENT_NATIVE_REFERENCE_EXACT_PASS', args.test_name,
          'cases=' + str(len(record['records'])), 'package=' + PACKAGE_SHA256)
