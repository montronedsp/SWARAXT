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
from unittest import mock
from common import sha
from desktop import (PLUGINVAL_AFTER, PLUGINVAL_BEFORE, PLUGINVAL_FIX_SHA, PLUGINVAL_JUCE_SHA,
                     PLUGINVAL_PORTABILITY, PLUGINVAL_SHA, SDK_SHA, WINDOWS_PROFILES,
                     admit_reference_record, contained_runner_path, gate)
from native_reference import WINDOWS_BASELINE_SHA, TESTS, validate_windows_output
import current_reference as current
import shutil
from unittest.mock import patch


class WindowsLegacyParserTests(unittest.TestCase):
    def setUp(self):
        self.source = Path(__file__).resolve().parents[2]
        self.legacy = tempfile.TemporaryDirectory()
        self.source = Path(self.legacy.name)
        header = (Path(__file__).resolve().parents[2] / 'Tests/Parity/ExactParityGoldens.h').read_text()
        body = re.search(r'Entry kClassic\[\] = \{(.*?)\};', header, re.S)[1]
        self.cases = re.findall(r'\{"([^"]+)", 0x([0-9a-fA-F]+)ull', body)
        driver = self.source / TESTS['SwaraXTAudioParityTests']
        driver.parent.mkdir(parents=True)
        driver.write_text('\n'.join('{ "fixture", "' + name + '", 0x' + value + 'ull }' for name, value in self.cases))
        self.addCleanup(self.legacy.cleanup)
        self.output = '\n'.join('GOLDEN classic ' + name + ' samples=44100 hash=' + value.zfill(16)
                                for name, value in self.cases) + '\n'

    def check_output(self, text, code=0):
        return validate_windows_output('SwaraXTAudioParityTests', text, code, self.source)

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
            evidence = Path(runner) / 'evidence'
            evidence.mkdir()
            self.assertEqual(contained_runner_path(evidence, True, runner), evidence.resolve())
            with self.assertRaisesRegex(RuntimeError, 'escaped RUNNER_TEMP'):
                contained_runner_path(Path(outside), True, runner)



class GateReportTests(unittest.TestCase):
    """SIMULATED_POLICY_TEST: report formatting, not Windows source/runtime admission."""
    def setUp(self):
        # This fixture checks report emission only. Real current admission and
        # native rejection controls live in test_windows_current_reference.
        def admit_fixture(evidence, source, candidate, run_id, jobs):
            # Formatting-only seam. Full current artifact/provenance controls
            # exercise the real verifier in test_windows_artifacts.
            for report in evidence.glob('*/report.json'):
                data = json.loads(report.read_text())
                for name, entry in data['native_audio_reference']['tests'].items():
                    admit_reference_record(report.parent, name, entry)
            return {'decision': 'WINDOWS_PORTABLE_GATE_PASS', 'source_sha': candidate,
                    'run_id': run_id, 'profiles': {c + '-' + p: 'PASS' for c, p in WINDOWS_PROFILES}}
        guard = mock.patch('desktop.windows_artifacts.admit_matrix', side_effect=admit_fixture)
        guard.start()
        self.addCleanup(guard.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.runner = Path(self.tmp.name)
        self.evidence = self.runner / 'evidence'
        self.evidence.mkdir()
        self.context = {'trusted_root': str(self.runner), 'source_sha': 'c' * 40,
                        'run_id': 'local-unit-admission'}
        for compiler, profile in WINDOWS_PROFILES:
            self.write_profile(compiler, profile)

    def tearDown(self):
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
            'source_sha': self.context['source_sha'], 'run_id': self.context['run_id'],
            'decision': 'PASS', 'commands': commands, 'test_count': 90,
            'actual_native_vst3': 'PASS', 'native_reference_negative_control': 'PASS',
            'sanitizer_negative_controls': 'PASS',
            'native_audio_reference': {
                'candidate_sha': self.context['source_sha'], 'baseline_sha': WINDOWS_BASELINE_SHA,
                'run_id': self.context['run_id'], 'profile': compiler + '-' + profile,
                'architecture': 'x86_64', 'tests': tests,
            },
        }
        if profile == 'Release':
            commands.extend({'name': name, 'exit_code': 0} for name in (
                'configure-pluginval', 'build-pluginval', 'pluginval-tool-selftest', 'pluginval',
                'configure-validator', 'build-validator', 'validator-selftest', 'validator-extensive',
                'windows-native-reference-controls'))
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
        (reference / 'manifest.json').write_text(json.dumps(data['native_audio_reference']))
        data['native_audio_reference_manifest_sha256'] = sha(reference / 'manifest.json')
        (directory / 'report.json').write_text(json.dumps(data))

    def files(self):
        return sorted(path.relative_to(self.runner).as_posix() for path in self.runner.rglob('*') if path.is_file())

    def test_success_report_is_stdout_only(self):
        before = self.files()
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            gate('Windows', self.evidence, context=self.context)
        text = buffer.getvalue()
        report = json.loads(text)
        self.assertTrue(text.endswith('\n'))
        self.assertEqual(text, json.dumps(report, indent=2) + '\n')
        self.assertEqual(report['decision'], 'WINDOWS_PORTABLE_GATE_PASS')
        self.assertEqual(report['source_sha'], self.context['source_sha'])
        self.assertEqual(set(report['profiles']), {'msvc-Release', 'msvc-Debug', 'msvc-asan'})
        self.assertEqual(self.files(), before)
        self.assertNotIn('write_text', inspect.getsource(gate))
        self.assertNotIn('open(', inspect.getsource(gate))

    def test_failed_gate_emits_no_success_report(self):
        record = self.evidence / 'msvc-Release' / 'native-reference' / (next(iter(TESTS)) + '.json')
        record.write_bytes(b'tampered')
        before = self.files()
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            with self.assertRaisesRegex(RuntimeError, 'Changed native reference record'):
                gate('Windows', self.evidence, context=self.context)
        self.assertNotIn('WINDOWS_PORTABLE_GATE_PASS', buffer.getvalue())
        self.assertEqual(buffer.getvalue(), '')
        self.assertEqual(self.files(), before)

    def test_cli_has_no_output_path_and_failure_writes_nothing(self):
        # Exercise pure argument parsing, never the desktop CLI/campaign.
        from desktop import argument_parser
        parser=argument_parser();output=io.StringIO();errors=io.StringIO()
        from contextlib import redirect_stderr
        with redirect_stdout(output),redirect_stderr(errors),self.assertRaises(SystemExit)as help_exit:
            parser.parse_args(['gate','--help'])
        self.assertEqual(help_exit.exception.code,0)
        self.assertNotIn('--output',output.getvalue())
        target=self.runner/'attacker-gate.json';output=io.StringIO()
        with redirect_stdout(output),redirect_stderr(errors),self.assertRaises(SystemExit)as failure:
            parser.parse_args(['gate','--platform','Windows','--evidence',str(self.runner/'missing'),
                               '--output',str(target)])
        self.assertEqual(failure.exception.code,2)
        self.assertNotIn('WINDOWS_PORTABLE_GATE_PASS',output.getvalue())
        self.assertFalse(target.exists())





@unittest.skipUnless(sys.platform.startswith('linux'), 'Current pinned reference policy is Linux-only')
class CurrentParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = Path(__file__).resolve().parents[2]
        cls.package = current.load_package(cls.source)

    def output(self, name):
        entry = self.package['tests'][name]
        if entry['grammar'] == 'headless-dual-v1':
            rows = [dict(case=case, mode='Independent' if case.startswith('DualIndependent_') else 'Relative',
                         calibration=case.split('_')[1], samples=value[0], hash=value[1],
                         finite=True, deterministic=True, parameters_unchanged=True, repeats=2)
                    for case, value in entry['cases'].items()]
            return '\n'.join('DUAL_SVF_HEADLESS ' + json.dumps(row) for row in rows) + '\n' + \
                'DUAL_SVF_HEADLESS_PASS cases=12 samples=25600 repeats=2 gui=excluded\n'
        return '\n'.join((case + ' hash=' + value[1]) if entry['grammar'] == 'dual' else
            'GOLDEN ' + entry['grammar'] + ' ' + case + ' samples=' + str(value[0]) + ' hash=' + value[1]
            for case, value in entry['cases'].items()) + '\n'

    def check(self, text=None, code=0, name=current.AUDIO_TESTS[0]):
        return current.validate_output(name, self.output(name) if text is None else text, code, self.source)

    def test_exact_current_30_case_set(self):
        records = [self.check(name=name) for name in current.AUDIO_TESTS]
        self.assertEqual(sum(len(value['records']) for value in records), 30)
        self.assertTrue(all('historical_hash_mismatches' not in value for value in records))

    def test_all_four_parser_grammars_and_inventory(self):
        for name, count in zip(current.ALL_TESTS, (8, 22, 30, 12)):
            with self.subTest(name=name):
                self.assertEqual(len(self.check(name=name)['records']), count)

    def test_changed_audio_hash_rejected(self):
        text = self.output(current.AUDIO_TESTS[0])
        value = next(iter(self.package['tests'][current.AUDIO_TESTS[0]]['cases'].values()))[1]
        with self.assertRaisesRegex(RuntimeError, 'audio hash mismatch'):
            self.check(text.replace(value, '0' * 16, 1))

    def test_changed_sample_count_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'sample count mismatch'):
            self.check(self.output(current.AUDIO_TESTS[0]).replace('samples=44100', 'samples=44101', 1))

    def test_missing_case_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'Missing or additional'):
            self.check('\n'.join(self.output(current.AUDIO_TESTS[0]).splitlines()[1:]))

    def test_additional_case_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'Missing or additional'):
            self.check(self.output(current.AUDIO_TESTS[0]) + 'GOLDEN classic unexpected samples=44100 hash=0000000000000000\n')

    def test_duplicate_case_rejected(self):
        text = self.output(current.AUDIO_TESTS[0])
        with self.assertRaisesRegex(RuntimeError, 'Duplicate current audio case'):
            self.check(text + text.splitlines()[0] + '\n')

    def test_malformed_partial_ambiguous_grammar_rejected(self):
        text = self.output(current.AUDIO_TESTS[0])
        for altered in (text.replace('samples=', 'count=', 1), text.replace('GOLDEN ', ' GOLDEN ', 1),
                        text.replace('GOLDEN classic', 'GOLDEN src', 1), text.replace('hash=', 'hash=0x', 1),
                        text.replace('samples=44100', 'samples=044100', 1), text[:-8]):
            with self.subTest(altered=altered[:90]), self.assertRaisesRegex(RuntimeError, 'Malformed|unexpected'):
                self.check(altered)

    def test_any_fail_rejected_even_with_approved_hashes(self):
        for code in (0, 1):
            with self.subTest(code=code), self.assertRaisesRegex(RuntimeError, 'Failed assertion'):
                self.check(self.output(current.AUDIO_TESTS[0]) + 'FAIL: formerly expected historical mismatch\n', code)

    def test_nonzero_exit_rejected(self):
        for code in (1, 2, -6, 124):
            with self.subTest(code=code), self.assertRaisesRegex(RuntimeError, 'Nonzero'):
                self.check(code=code)

    def test_sanitizer_and_assertion_diagnostics_rejected(self):
        for marker in ('runtime error: enum', 'ERROR: AddressSanitizer: ODR', 'WARNING: ThreadSanitizer: race',
                       'ERROR: LeakSanitizer: leak', 'ERROR: RealtimeSanitizer: malloc', 'JUCE Assertion failure'):
            with self.subTest(marker=marker), self.assertRaisesRegex(RuntimeError, 'diagnostic'):
                self.check(self.output(current.AUDIO_TESTS[0]) + marker + '\n')

    def test_obsolete_mixer_reference_rejected(self):
        source = self.source / 'Tests/Parity/ExactParityGoldens.h'
        body = re.search(r'Entry kIntegrated\[\] = \{(.*?)\};', source.read_text(), re.S)[1]
        records = re.findall(r'\{"([^"]+)", 0x([0-9a-f]+)ull, (\d+)\}', body)
        text = '\n'.join('GOLDEN integrated '+case+' samples='+count+' hash='+value for case, value, count in records)
        with self.assertRaisesRegex(RuntimeError, 'audio hash mismatch'):
            self.check(text, name=current.AUDIO_TESTS[1])

    def test_dual_hash_and_duplicate_rejected(self):
        name = current.ALL_TESTS[3];text=self.output(name)
        value = next(iter(self.package['tests'][name]['cases'].values()))[1]
        with self.assertRaisesRegex(RuntimeError, 'Changed Dual audio hash'):
            self.check(text.replace(value, '0'*16, 1), name=name)
        with self.assertRaisesRegex(RuntimeError, 'Duplicate'):
            self.check(text+text.splitlines()[0]+'\n', name=name)

    def test_unknown_test_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'test identity'):
            self.check('', name='arbitrary')


@unittest.skipUnless(sys.platform.startswith('linux'), 'Current pinned reference policy is Linux-only')
class CurrentAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.source = Path(__file__).resolve().parents[2]
        self.temp = tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.profile = Path(self.temp.name);self.build = self.profile/'build';self.build.mkdir()
        self.identity = current.context(self.source, subprocess.check_output(
            ['git','-C',str(self.source),'rev-parse','HEAD'],text=True).strip(),
            'clang-Release','local-reference-unit','focused-current-audio-30')
        self.executables={}
        for name in current.AUDIO_TESTS:
            binary=self.build/name;shutil.copy2('/usr/bin/true',binary);self.executables[name]=binary
        self.manifest=current.stage_reference(self.profile,self.source,self.identity,self.executables,self.build)
        self.root=self.profile/'native-reference'

    def admit(self, **kwargs):
        return current.admit_reference(kwargs.get('root',self.root),self.source,
            kwargs.get('identity',self.identity),self.build,kwargs.get('expected_manifest'))

    def reseal(self, value):
        data=current.encode(value);(self.root/'manifest.json').write_bytes(data)
        (self.root/'manifest.sha256').write_text(current.digest(data)+'\n')

    def test_valid_approved_current_reference(self):
        self.assertEqual(self.admit(expected_manifest=self.manifest),self.manifest)

    def test_wrong_source_profile_architecture_run_scope(self):
        for field, bad in (('candidate_sha','f'*40),('reference_source_sha','f'*40),
                           ('candidate_tree_sha256','f'*64),('profile','gcc-Debug'),
                           ('architecture','arm64'),('run_id','different'),('scope','full-native-72')):
            altered=json.loads(json.dumps(self.manifest));altered[field]=bad;self.reseal(altered)
            with self.subTest(field=field),self.assertRaisesRegex(RuntimeError,'identity|provenance|schema'):
                self.admit()
        self.reseal(self.manifest)

    def test_corrupt_manifest_rejected(self):
        (self.root/'manifest.json').write_bytes(b'{broken')
        with self.assertRaisesRegex(RuntimeError,'checksum'):self.admit()

    def test_duplicate_json_key_rejected_after_reseal(self):
        data=current.encode(self.manifest).replace(b'"schema": 2',b'"schema": 2, "schema": 2')
        (self.root/'manifest.json').write_bytes(data);(self.root/'manifest.sha256').write_text(current.digest(data)+'\n')
        with self.assertRaisesRegex(RuntimeError,'Duplicate JSON'):self.admit()

    def test_invalid_json_nan_rejected(self):
        with self.assertRaisesRegex(RuntimeError,'Malformed'):current.decode(b'{"v":NaN}')

    def test_manifest_vs_producer_snapshot(self):
        bad=dict(self.manifest,run_id='other');self.reseal(bad)
        with self.assertRaisesRegex(RuntimeError,'recorded producer'):
            self.admit(identity=dict(self.identity,run_id='other'),expected_manifest=self.manifest)

    def test_tampered_reference_file_rejected(self):
        name=current.AUDIO_TESTS[0];(self.root/(name+'.json')).write_text('tampered')
        with self.assertRaisesRegex(RuntimeError,'checksum'):self.admit()

    def test_resealed_audio_mutation_not_trusted(self):
        name=current.AUDIO_TESTS[0];p=self.root/(name+'.json');value=current.decode(p.read_bytes())
        value['records'][next(iter(value['records']))][1]='0'*16;p.write_bytes(current.encode(value))
        altered=json.loads(json.dumps(self.manifest));altered['tests'][name]['record_sha256']=current.digest(p.read_bytes());self.reseal(altered)
        with self.assertRaisesRegex(RuntimeError,'immutable approved cases'):self.admit()

    def test_resealed_sample_mutation_not_trusted(self):
        name=current.AUDIO_TESTS[0];p=self.root/(name+'.json');value=current.decode(p.read_bytes())
        value['records'][next(iter(value['records']))][0]+=1;p.write_bytes(current.encode(value))
        altered=json.loads(json.dumps(self.manifest));altered['tests'][name]['record_sha256']=current.digest(p.read_bytes());self.reseal(altered)
        with self.assertRaisesRegex(RuntimeError,'immutable approved cases'):self.admit()

    def test_missing_record_and_extra_filename(self):
        name=current.AUDIO_TESTS[0];p=self.root/(name+'.json');data=p.read_bytes();p.unlink()
        with self.assertRaisesRegex(RuntimeError,'filename inventory'):self.admit()
        p.write_bytes(data);(self.root/'unexpected.json').write_text('{}')
        with self.assertRaisesRegex(RuntimeError,'filename inventory'):self.admit()

    def test_record_filename_traversal_and_other_test(self):
        name=current.AUDIO_TESTS[0]
        for relative in ('../escape.json','/tmp/escape.json','..\\escape.json',current.AUDIO_TESTS[1]+'.json'):
            altered=json.loads(json.dumps(self.manifest));altered['tests'][name]['record_file']=relative;self.reseal(altered)
            with self.subTest(relative=relative),self.assertRaisesRegex(RuntimeError,'filename'):self.admit()

    def test_record_symlink_rejected(self):
        name=current.AUDIO_TESTS[0];p=self.root/(name+'.json');target=self.profile/'outside';p.rename(target);p.symlink_to(target)
        with self.assertRaisesRegex(RuntimeError,'untrusted'):self.admit()

    def test_directory_symlink_rejected(self):
        target=self.profile/'moved';self.root.rename(target);self.root.symlink_to(target,target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError,'untrusted'):self.admit()

    def test_intermediate_directory_symlink_rejected(self):
        link=self.profile/'link';link.symlink_to(self.profile,target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError,'untrusted'):self.admit(root=link/'native-reference')

    def test_executable_path_escape_and_wrong_name(self):
        name=current.AUDIO_TESTS[0]
        for value in ('../'+name,'/tmp/'+name,'..\\'+name,'not-the-test'):
            altered=json.loads(json.dumps(self.manifest));altered['tests'][name]['executable']=value;self.reseal(altered)
            with self.subTest(value=value),self.assertRaisesRegex(RuntimeError,'traversal|absolute|identity'):
                self.admit()

    def test_wrong_executable_hash_rejected(self):
        self.executables[current.AUDIO_TESTS[0]].write_bytes(b'tampered')
        with self.assertRaisesRegex(RuntimeError,'checksum'):self.admit()

    def test_executable_symlink_rejected(self):
        p=self.executables[current.AUDIO_TESTS[0]];p.unlink();p.symlink_to('/usr/bin/true')
        with self.assertRaisesRegex(RuntimeError,'untrusted'):self.admit()

    def test_wrong_candidate_command_rejected_before_execution(self):
        with self.assertRaisesRegex(RuntimeError,'executable path'):
            current.execute_candidate(self.root,self.source,self.identity,self.build,current.AUDIO_TESTS[0],['/usr/bin/true'])

    def test_candidate_argument_injection_rejected(self):
        with self.assertRaisesRegex(RuntimeError,'test command'):
            current.execute_candidate(self.root,self.source,self.identity,self.build,current.AUDIO_TESTS[0],
                [str(self.executables[current.AUDIO_TESTS[0]]),'--skip-tests'])

    def test_focused_data_cannot_be_admitted_as_full_ci(self):
        with self.assertRaisesRegex(RuntimeError,'identity'):
            self.admit(identity=dict(self.identity,scope='full-native-72'))

    def test_data_cannot_be_overwritten_by_acquisition(self):
        with self.assertRaisesRegex(RuntimeError,'new trusted directory'):
            current.stage_reference(self.profile,self.source,self.identity,self.executables,self.build)

    def test_acquisition_never_runs_candidate(self):
        root=self.profile/'new';root.mkdir()
        with patch.object(current.subprocess,'run',side_effect=AssertionError('candidate used as producer')):
            current.stage_reference(root,self.source,self.identity,self.executables,self.build)

    def test_full_reference_inventory_retained(self):
        root=self.profile/'full';root.mkdir();executables={}
        for name in current.ALL_TESTS:
            binary=self.build/name
            if not binary.exists():shutil.copy2('/usr/bin/true',binary)
            executables[name]=binary
        identity=dict(self.identity,scope='full-native-72')
        manifest=current.stage_reference(root,self.source,identity,executables,self.build)
        current.admit_reference(root/'native-reference',self.source,identity,self.build,manifest)
        self.assertEqual(sum(value['case_count']for value in manifest['tests'].values()),72)


    def test_exact_record_numeric_types_are_not_coerced(self):
        name=current.AUDIO_TESTS[0];p=self.root/(name+'.json');record=current.decode(p.read_bytes())
        case=next(iter(record['records']));record['records'][case][0]=float(record['records'][case][0]);p.write_bytes(current.encode(record))
        altered=json.loads(json.dumps(self.manifest));altered['tests'][name]['record_sha256']=sha(p);self.reseal(altered)
        with self.assertRaisesRegex(RuntimeError,'immutable approved cases'):self.admit()

    def test_manifest_schema_numeric_type_is_strict(self):
        altered=dict(self.manifest,schema=2.0);self.reseal(altered)
        with self.assertRaisesRegex(RuntimeError,'schema'):self.admit()

    def test_final_linux_admission_checks_same_schema(self):
        from desktop import admit_linux_reference
        root=self.profile/'full-admission';root.mkdir();build=root/'build';build.mkdir();executables={}
        for name in current.ALL_TESTS:
            binary=build/name;shutil.copy2('/usr/bin/true',binary);executables[name]=binary
        identity=dict(self.identity,scope='full-native-72')
        manifest=current.stage_reference(root,self.source,identity,executables,build)
        manifest_sha=sha(root/'native-reference/manifest.json')
        admit_linux_reference(root,manifest,manifest_sha,identity['candidate_sha'],identity['profile'],identity['run_id'])
        with self.assertRaisesRegex(RuntimeError,'producer manifest'):
            admit_linux_reference(root,manifest,'0'*64,identity['candidate_sha'],identity['profile'],identity['run_id'])
        wrong=json.loads(json.dumps(manifest));wrong['tests'][current.AUDIO_TESTS[0]]['case_count']+=1
        with self.assertRaisesRegex(RuntimeError,'recorded producer'):
            admit_linux_reference(root,wrong,manifest_sha,identity['candidate_sha'],identity['profile'],identity['run_id'])

    def test_final_linux_admission_rejects_focused_evidence(self):
        from desktop import admit_linux_reference
        with self.assertRaisesRegex(RuntimeError,'identity'):
            admit_linux_reference(self.profile,self.manifest,sha(self.root/'manifest.json'),
                self.identity['candidate_sha'],self.identity['profile'],self.identity['run_id'])


@unittest.skipUnless(sys.platform.startswith('linux'), 'Current pinned reference policy is Linux-only')
class PackageAuthenticationTests(unittest.TestCase):
    def test_approval_label_cannot_authenticate_tampered_package(self):
        source=Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory()as temp:
            root=Path(temp);relative=current.PACKAGE_FILE;path=root/relative;path.parent.mkdir(parents=True)
            value=json.loads((source/relative).read_text());value['approved']=True;path.write_bytes(current.encode(value))
            with self.assertRaisesRegex(RuntimeError,'checksum'):current.load_package(root)

    def test_current_dispatch_has_no_frozen_clone(self):
        from desktop import Campaign
        fake=type('LocalDispatch',(),{'system':'Linux','current_reference':lambda self,build:('current',build)})()
        self.assertEqual(Campaign.reference(fake,None,'local-build'),('current','local-build'))
        self.assertNotIn('clone',inspect.getsource(Campaign.current_reference))
        self.assertIn('stage_reference',inspect.getsource(Campaign.current_reference))

    def test_linux_artifact_transport_keeps_reference_bindings(self):
        source=Path(__file__).resolve().parents[2]
        workflow=(source/'.github/workflows/linux-qualification.yml').read_text()
        prefix='${{ runner.temp }}/swara-native/'
        self.assertIn(prefix+'native-reference/manifest.sha256',workflow)
        for name in current.ALL_TESTS:
            self.assertIn(prefix+'build/Tests/**/'+name,workflow)

    def test_platform_reference_authorities_remain_explicit(self):
        from desktop import Campaign
        import native_reference
        body = inspect.getsource(Campaign.reference)
        self.assertIn("if self.system == 'Linux'", body)
        self.assertIn('return self.current_reference(build)', body)
        for method in ('context', 'stage_reference', 'admit_reference'):
            self.assertIn('windows_current_reference.' + method, body)
        self.assertNotIn('WINDOWS_BASELINE_SHA', body)
        self.assertIn('windows_reference_policy.require_frozen_source',
                      inspect.getsource(native_reference.historical_windows_main))


if __name__ == '__main__':
    unittest.main()
