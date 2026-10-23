# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Negative controls for native audio evidence admission."""
import inspect
import io
import json
import os
from contextlib import redirect_stdout
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from common import sha
from desktop import (PLUGINVAL_AFTER, PLUGINVAL_BEFORE, PLUGINVAL_FIX_SHA, PLUGINVAL_JUCE_SHA,
                     PLUGINVAL_PORTABILITY, PLUGINVAL_SHA, SDK_SHA, WINDOWS_PROFILES,
                     admit_reference_record, contained_runner_path, gate)
from native_reference import BASELINE_SHA, TESTS, validate_output


class NativeReferenceTests(unittest.TestCase):
    def setUp(self):
        self.source = Path(__file__).resolve().parents[2]
        text = (self.source / TESTS['SwaraXTAudioParityTests']).read_text()
        self.cases = re.findall(r'\{ "[^"\n]+", "([^"\n]+)"[^\n]+0x([0-9a-fA-F]+)ull', text)
        self.output = '\n'.join('GOLDEN classic ' + name + ' samples=44100 hash=' + value.zfill(16)
                                for name, value in self.cases) + '\n'

    def check_output(self, text, code=0):
        return validate_output('SwaraXTAudioParityTests', text, code, self.source)

    def test_original_legacy_match(self):
        self.assertEqual(len(self.check_output(self.output)['records']), 8)

    def test_identified_historical_hash_difference_is_explicit(self):
        name, value = self.cases[0]
        changed = ('1' if value[0] == '0' else '0') + value[1:]
        result = self.check_output(self.output.replace(value, changed) + 'FAIL: ' + name + '\n', 1)
        self.assertEqual(result['historical_hash_mismatches'], 1)

    def test_missing_case_fails(self):
        with self.assertRaises(RuntimeError):
            self.check_output('\n'.join(self.output.splitlines()[1:]))

    def test_changed_sample_count_fails(self):
        with self.assertRaises(RuntimeError):
            self.check_output(self.output.replace('samples=44100', 'samples=44101', 1))

    def test_duplicate_case_fails(self):
        with self.assertRaises(RuntimeError):
            self.check_output(self.output + self.output.splitlines()[0] + '\n')

    def test_non_hash_safety_failure_fails(self):
        with self.assertRaises(RuntimeError):
            self.check_output(self.output + 'FAIL: final render finite\n', 1)

    def test_sanitizer_failure_cannot_be_admitted(self):
        with self.assertRaises(RuntimeError):
            self.check_output(self.output + 'runtime error: invalid enum\n', 1)

    def test_exit_status_must_agree(self):
        with self.assertRaises(RuntimeError):
            self.check_output(self.output, 1)


class ReferencePathTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.reference = self.root / 'native-reference'
        self.reference.mkdir()
        self.name = 'SwaraXTAudioParityTests'
        self.record = self.reference / (self.name + '.json')
        self.record.write_bytes(b'{"records":{}}\n')
        self.digest = sha(self.record)

    def tearDown(self):
        self.tmp.cleanup()

    def entry(self, record_file=None, digest=None):
        return {
            'record_file': self.name + '.json' if record_file is None else record_file,
            'record_sha256': self.digest if digest is None else digest,
        }

    def symlink(self, target, link):
        try:
            os.symlink(target, link)
        except OSError as error:
            self.skipTest('symlink unavailable: ' + str(error))

    def test_valid_expected_reference_file(self):
        admit_reference_record(self.root, self.name, self.entry())

    def test_relative_traversal_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'Unexpected native reference filename'):
            admit_reference_record(self.root, self.name, self.entry('../secret.json'))

    def test_absolute_path_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'Unexpected native reference filename'):
            admit_reference_record(self.root, self.name, self.entry(str(self.record.resolve())))

    def test_windows_separator_traversal_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'Unexpected native reference filename'):
            admit_reference_record(self.root, self.name, self.entry('..\\secret.json'))

    def test_symlink_escape_fails(self):
        outside = self.root / 'outside'
        outside.mkdir()
        secret = outside / 'secret.json'
        secret.write_bytes(b'escaped')
        self.record.unlink()
        self.symlink(secret, self.record)
        with self.assertRaisesRegex(RuntimeError, 'untrusted'):
            admit_reference_record(self.root, self.name, self.entry(digest=sha(secret)))
        self.record.unlink()
        self.record.write_bytes(b'{"records":{}}\n')
        linked = self.root / 'linked-reference'
        self.reference.rename(linked)
        try:
            os.symlink(linked, self.reference, target_is_directory=True)
        except OSError as error:
            self.skipTest('directory symlink unavailable: ' + str(error))
        with self.assertRaisesRegex(RuntimeError, 'trusted directory'):
            admit_reference_record(self.root, self.name, self.entry())

    def test_missing_record_fails(self):
        self.record.unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing or untrusted'):
            admit_reference_record(self.root, self.name, self.entry())

    def test_unexpected_filename_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'Unexpected native reference filename'):
            admit_reference_record(self.root, self.name, self.entry('notes.txt'))

    def test_modified_record_checksum_fails(self):
        self.record.write_bytes(b'changed')
        with self.assertRaisesRegex(RuntimeError, 'Changed native reference record'):
            admit_reference_record(self.root, self.name, self.entry())

    def test_manifest_reference_to_unrelated_file_fails(self):
        other_name = 'SwaraXTHostSrcExactParityTests.json'
        other = self.reference / other_name
        other.write_bytes(b'other')
        with self.assertRaisesRegex(RuntimeError, 'Unexpected native reference filename'):
            admit_reference_record(self.root, self.name, self.entry(other_name, sha(other)))

    def test_duplicate_or_inconsistent_reference_metadata_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'Unexpected native reference filename'):
            admit_reference_record(self.root, 'SwaraXTHostSrcExactParityTests', self.entry())
        with self.assertRaisesRegex(RuntimeError, 'Changed native reference record'):
            admit_reference_record(self.root, self.name, self.entry(digest='0' * 64))

    def test_gate_rejects_paths_outside_runner_temp(self):
        with tempfile.TemporaryDirectory() as runner, tempfile.TemporaryDirectory() as outside:
            previous = os.environ.get('RUNNER_TEMP')
            os.environ['RUNNER_TEMP'] = runner
            try:
                evidence = Path(runner) / 'evidence'
                evidence.mkdir()
                self.assertEqual(contained_runner_path(evidence, True), evidence.resolve())
                with self.assertRaisesRegex(RuntimeError, 'escaped RUNNER_TEMP'):
                    gate('Windows', Path(outside))
            finally:
                if previous is None:
                    os.environ.pop('RUNNER_TEMP', None)
                else:
                    os.environ['RUNNER_TEMP'] = previous


class GateReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.runner = Path(self.tmp.name)
        self.evidence = self.runner / 'evidence'
        self.evidence.mkdir()
        self.previous = {name: os.environ.get(name) for name in ('RUNNER_TEMP', 'GITHUB_SHA', 'GITHUB_RUN_ID')}
        os.environ['RUNNER_TEMP'] = str(self.runner)
        os.environ['GITHUB_SHA'] = 'c' * 40
        os.environ['GITHUB_RUN_ID'] = '39935259841'
        for compiler, profile in WINDOWS_PROFILES:
            self.write_profile(compiler, profile)

    def tearDown(self):
        for name, value in self.previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        self.tmp.cleanup()

    def write_profile(self, compiler, profile):
        directory = self.evidence / (compiler + '-' + profile)
        reference = directory / 'native-reference'
        reference.mkdir(parents=True)
        tests = {}
        for name in TESTS:
            record = reference / (name + '.json')
            record.write_bytes(b'{"records":{}}\n')
            tests[name] = {'record_file': record.name, 'record_sha256': sha(record)}
        cases = ''.join('<testcase name="t%d"/>' % index for index in range(90))
        (directory / 'ctest.xml').write_text('<testsuite>%s</testsuite>' % cases)
        (directory / 'actual-VST3.log').write_text(
            'PASS: actual plugin two-instance audio/editor/state/reopen lifecycle\nBOOLEAN_NORMALIZATION_PASS\n')
        commands = [{'name': name, 'exit_code': 0} for name in (
            'harness-tests', 'configure', 'actual-VST3', 'ctest', 'build-regressions')]
        data = {
            'compiler': compiler, 'profile': profile, 'platform': 'Windows',
            'source_sha': os.environ['GITHUB_SHA'], 'run_id': os.environ['GITHUB_RUN_ID'],
            'decision': 'PASS', 'commands': commands, 'test_count': 90,
            'actual_native_vst3': 'PASS', 'native_reference_negative_control': 'PASS',
            'sanitizer_negative_controls': 'PASS',
            'native_audio_reference': {
                'candidate_sha': os.environ['GITHUB_SHA'], 'baseline_sha': BASELINE_SHA,
                'run_id': os.environ['GITHUB_RUN_ID'], 'profile': compiler + '-' + profile,
                'architecture': 'x86_64', 'tests': tests,
            },
        }
        if profile == 'Release':
            commands.extend({'name': name, 'exit_code': 0} for name in (
                'configure-pluginval', 'build-pluginval', 'pluginval-tool-selftest', 'pluginval',
                'configure-validator', 'build-validator', 'validator-selftest', 'validator-extensive'))
            data['vst3sdk_tool'] = {'source_sha': SDK_SHA, 'binary_sha256': 'b' * 64}
            data['external_validators'] = {'vst3sdk_binary_sha256': 'b' * 64, 'sha256': 'a' * 64}
            data['pluginval_source'] = {
                'source_sha': PLUGINVAL_SHA, 'vendor_juce_sha': PLUGINVAL_JUCE_SHA,
                'upstream_fix_sha': PLUGINVAL_FIX_SHA, 'before_sha256': PLUGINVAL_BEFORE,
                'after_sha256': PLUGINVAL_AFTER, 'compiler_portability': PLUGINVAL_PORTABILITY,
                'binary_sha256': 'a' * 64,
            }
            (directory / 'pluginval.log').write_text('Num plugins found: 1\nStrictness level: 10\nv1.2.3\nSUCCESS\n')
            (directory / 'validator-extensive.log').write_text('Result: 100 tests passed, 0 tests failed\n')
        (directory / 'report.json').write_text(json.dumps(data))

    def files(self):
        return sorted(path.relative_to(self.runner).as_posix() for path in self.runner.rglob('*') if path.is_file())

    def test_success_report_is_stdout_only(self):
        before = self.files()
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            gate('Windows', self.evidence)
        text = buffer.getvalue()
        report = json.loads(text)
        self.assertTrue(text.endswith('\n'))
        self.assertEqual(text, json.dumps(report, indent=2) + '\n')
        self.assertEqual(report['decision'], 'NATIVE_CI_MATRIX_PASS')
        self.assertEqual(report['source_sha'], os.environ['GITHUB_SHA'])
        self.assertEqual(set(report['profiles']), {'msvc-Release', 'msvc-Debug', 'msvc-asan'})
        self.assertEqual(self.files(), before)
        self.assertNotIn('write_text', inspect.getsource(gate))
        self.assertNotIn('open(', inspect.getsource(gate))

    def test_failed_gate_emits_no_success_report(self):
        record = next((self.evidence / 'msvc-Release' / 'native-reference').glob('*.json'))
        record.write_bytes(b'tampered')
        before = self.files()
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            with self.assertRaisesRegex(RuntimeError, 'Changed native reference record'):
                gate('Windows', self.evidence)
        self.assertNotIn('NATIVE_CI_MATRIX_PASS', buffer.getvalue())
        self.assertEqual(buffer.getvalue(), '')
        self.assertEqual(self.files(), before)

    def test_cli_has_no_output_path_and_failure_writes_nothing(self):
        script = Path(__file__).resolve().parent / 'desktop.py'
        help_text = subprocess.run([sys.executable, str(script), 'gate', '--help'],
                                   check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(help_text.returncode, 0, help_text.stdout)
        self.assertNotIn('--output', help_text.stdout)
        target = self.runner / 'attacker-gate.json'
        env = dict(os.environ)
        failed = subprocess.run([sys.executable, str(script), 'gate', '--platform', 'Windows',
                                 '--evidence', str(self.runner / 'missing'), '--output', str(target)],
                                check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.assertNotEqual(failed.returncode, 0)
        self.assertNotIn('NATIVE_CI_MATRIX_PASS', failed.stdout)
        self.assertFalse(target.exists())


if __name__ == '__main__':
    unittest.main()
