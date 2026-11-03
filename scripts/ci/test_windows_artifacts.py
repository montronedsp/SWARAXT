# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""SYNTHETIC contract controls, not native qualification or authentic attestations."""
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import windows_artifacts as artifact
import windows_current_reference as current
import pluginval_source as pluginval
from common import SDK_SHA

SOURCE = Path(__file__).resolve().parents[2]


def synthetic_profile(root, identity):
    """Deliberately fabricated unit data; never used as real qualification."""
    package = current.load_package(SOURCE)
    proof = dict(compile_commands_sha256='1' * 64, compiler_sha256=current.ADMITTED_COMPILER_SHA256,
                 juce_sha=package['reference_juce_sha'], cmake_cache_sha256='2' * 64,
                 configuration='Debug' if identity['profile'] == 'msvc-Debug' else 'Release',
                 translation_units=100, cpp_exception_unwinding=True,
                 asan_all_units=identity['profile'] == 'msvc-asan', juce_pristine=True, executables={})
    manifest = dict(schema=1, policy=current.POLICY, package_sha256=current.PACKAGE_SHA256,
                    **identity, toolchain={k: proof[k] for k in ('compile_commands_sha256', 'compiler_sha256', 'juce_sha')}, tests={})
    payload = {'profile.json': current.encode(identity)}
    suite = ET.Element('testsuite')
    for name, entry in package['tests'].items():
        data = current.encode(dict(schema=1, test=name, records=entry['cases']))
        payload['native-reference/' + name + '.json'] = data
        binary_hash = current.digest(('SYNTHETIC-BINARY-' + name).encode())
        manifest['tests'][name] = dict(record_file=name + '.json', record_sha256=current.digest(data),
            executable='Tests/' + name + '.exe', executable_sha256=binary_hash)
        proof['executables'][name] = dict(sha256=binary_hash, machine='x86_64', file=name + '.exe')
        lines = []
        for case, (count, value) in entry['cases'].items():
            if entry['grammar'] != 'headless-dual-v1':
                lines.append(f"GOLDEN {entry['grammar']} {case} samples={count} hash={value}")
            else:
                lines.append('DUAL_SVF_HEADLESS ' + json.dumps(dict(case=case, samples=count, hash=value,
                    mode='Independent' if case.startswith('DualIndependent') else 'Relative',
                    calibration=case.split('_')[1], finite=True, deterministic=True,
                    parameters_unchanged=True, repeats=2)))
        if entry['grammar'] == 'headless-dual-v1':
            lines.append('DUAL_SVF_HEADLESS_PASS cases=12 samples=25600 repeats=2 gui=excluded')
        c = ET.SubElement(suite, 'testcase', name=name, status='run')
        ET.SubElement(c, 'system-out').text = '\n'.join(lines)
    for i in range(86):
        ET.SubElement(suite, 'testcase', name='SYNTHETIC-' + str(i), status='run')
    payload['ctest.xml'] = ET.tostring(suite)
    payload['ctest-inventory.json'] = current.encode(dict(tests=[dict(name=c.get('name'), properties=[]) for c in suite]))
    payload['actual-VST3.log'] = b'PASS: actual plugin two-instance audio/editor/state/reopen lifecycle\nBOOLEAN_NORMALIZATION_PASS\n'
    payload['reference-negative-control.log'] = b'RuntimeError: Unauthorized Windows reference mutation\n'
    payload['native-reference/manifest.json'] = current.encode(manifest)
    manifest_hash = current.digest(current.encode(manifest))
    payload['native-reference/manifest.sha256'] = (manifest_hash + '\n').encode()
    payload['build-provenance.json'] = current.encode(proof)
    commands = {n: 0 for n in ('harness-tests', 'configure', 'actual-VST3', 'ctest', 'build-regressions')}
    commands['reference-negative-control'] = 1
    report = dict(source_sha=identity['candidate_sha'], run_id=identity['run_id'], platform='Windows', compiler='msvc',
        profile=identity['profile'][5:], decision='PASS', test_count=90, actual_native_vst3='PASS',
        native_reference_negative_control='PASS', native_audio_reference=manifest,
        native_audio_reference_manifest_sha256=manifest_hash)
    if identity['profile'] == 'msvc-Release':
        commands.update({n: 0 for n in ('windows-native-reference-controls', 'configure-pluginval', 'build-pluginval',
            'pluginval-tool-selftest', 'pluginval', 'configure-validator', 'build-validator', 'validator-selftest', 'validator-extensive')})
        report.update(pluginval_source=dict(source_sha=pluginval.SOURCE_SHA, vendor_juce_sha=pluginval.VENDOR_SHA,
            upstream_fix_sha=pluginval.FIX_SHA, before_sha256=pluginval.BEFORE_SHA256,
            after_sha256=pluginval.AFTER_SHA256, compiler_portability=pluginval.PORTABILITY_FILES, binary_sha256='3' * 64),
            vst3sdk_tool=dict(source_sha=SDK_SHA, binary_sha256='4' * 64),
            external_validators=dict(sha256='3' * 64, vst3sdk_binary_sha256='4' * 64))
        payload['windows-native-reference-controls.log'] = b'Ran 24 tests in 1.0s\n\nOK\n'
        payload['pluginval.log'] = b'Num plugins found: 1\nStrictness level: 10\nv1.3.0\nSUCCESS\n'
        payload['validator-extensive.log'] = b'Result: 100 tests passed, 0 tests failed\n'
    if identity['profile'] == 'msvc-asan':
        commands.update({'asan-control-build': 0, 'asan-negative-control': 3})
        report['sanitizer_negative_controls'] = 'PASS'
        payload['asan-negative-control.log'] = b'ERROR: AddressSanitizer: heap-buffer-overflow\n'
    report['commands'] = [dict(name=n, exit_code=c) for n, c in commands.items()]
    payload['report.json'] = current.encode(report)
    for n, b in payload.items():
        p = root / n
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b)
    index = {n: dict(role=artifact.files_for(identity['profile'])[n], size=len(b), sha256=current.digest(b)) for n, b in payload.items()}
    (root / 'index.json').write_bytes(current.encode(index))
    (root / 'attestation.json').write_bytes(current.encode(dict(schema=1, policy=artifact.POLICY, platform='Windows',
        **identity, reference_package_sha256=current.PACKAGE_SHA256, reference_manifest_sha256=manifest_hash,
        native_validation='PASS', native_proof=proof, index_sha256=current.digest(current.encode(index)))))


class ArtifactControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sha = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
        cls.tree = current.source_identity(SOURCE)
        cls.store = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.store.cleanup)
        cls.original = Path(cls.store.name)
        for p in current.PROFILES:
            synthetic_profile(cls.original / ('native-windows-' + p + '-' + cls.sha), cls.identity(p))

    @classmethod
    def identity(cls, profile='msvc-Release'):
        return dict(candidate_sha=cls.sha, candidate_tree_sha256=cls.tree, run_id='SYNTHETIC-RUN',
                    profile=profile, scope='full-native-72', architecture='x86_64')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'download'
        shutil.copytree(self.original, self.root)
        self.profile = self.root / ('native-windows-msvc-Release-' + self.sha)
        # The actual checkout digest is computed once in setUpClass. Negative
        # fixtures change artifact bytes, never the source tree; avoid hashing
        # the same checkout per case (especially across a WSL mounted drive).
        digest = patch.object(current, 'source_identity', return_value=self.tree)
        digest.start()
        self.addCleanup(digest.stop)

    def load(self, name):
        return current.decode((self.profile / name).read_bytes())

    def save(self, name, value):
        (self.profile / name).write_bytes(current.encode(value))

    def reseal(self, name):
        index = self.load('index.json')
        b = (self.profile / name).read_bytes()
        index[name].update(size=len(b), sha256=current.digest(b))
        self.save('index.json', index)
        a = self.load('attestation.json')
        a['index_sha256'] = current.digest((self.profile / 'index.json').read_bytes())
        self.save('attestation.json', a)

    def check(self, results=None):
        return artifact.admit_matrix(self.root, SOURCE, self.sha, 'SYNTHETIC-RUN',
            {'preflight': 'success', 'native': 'success'} if results is None else results)

    def reject(self, reason):
        with self.assertRaisesRegex((RuntimeError, FileNotFoundError), reason):
            self.check()

    def test_positive_without_native_build_or_toolchain(self):
        with patch.object(current, 'toolchain', side_effect=AssertionError('native call forbidden')), \
                patch.object(current, 'admit_reference', side_effect=AssertionError('native call forbidden')):
            self.assertEqual(self.check()['decision'], 'WINDOWS_PORTABLE_GATE_PASS')

    def test_missing_profile(self):
        shutil.rmtree(self.profile)
        self.reject('profile directory')

    def test_duplicate_profile_directory(self):
        shutil.copytree(self.profile, self.root / 'duplicate')
        self.reject('profile directory')

    def test_wrong_candidate(self):
        a = self.load('attestation.json');a['candidate_sha'] = '0' * 40;self.save('attestation.json', a)
        self.reject('execution identity')

    def test_wrong_run(self):
        a = self.load('attestation.json');a['run_id'] = 'other';self.save('attestation.json', a)
        self.reject('execution identity')

    def test_wrong_source_tree(self):
        a = self.load('attestation.json');a['candidate_tree_sha256'] = '0' * 64;self.save('attestation.json', a)
        self.reject('execution identity')

    def test_wrong_package(self):
        a = self.load('attestation.json');a['reference_package_sha256'] = '0' * 64;self.save('attestation.json', a)
        self.reject('package identity')

    def test_changed_manifest(self):
        (self.profile / 'native-reference/manifest.json').write_bytes(b'{}')
        self.reject('Changed evidence file')

    def test_resealed_reference_mutation(self):
        n = current.ALL_TESTS[0];f = 'native-reference/' + n + '.json';r = self.load(f)
        next(iter(r['records'].values()))[1] = '0' * 16;self.save(f, r);self.reseal(f)
        # Also reseal manifest and report: approved authority must still reject.
        m = self.load('native-reference/manifest.json');m['tests'][n]['record_sha256'] = current.digest((self.profile / f).read_bytes())
        self.update_manifest(m)
        self.reject('Unauthorized reference mutation')

    def update_manifest(self, manifest):
        f = 'native-reference/manifest.json';self.save(f, manifest);self.reseal(f)
        h = current.digest((self.profile / f).read_bytes());(self.profile / 'native-reference/manifest.sha256').write_bytes((h + '\n').encode());self.reseal('native-reference/manifest.sha256')
        a = self.load('attestation.json');a['reference_manifest_sha256'] = h;self.save('attestation.json', a)
        r = self.load('report.json');r['native_audio_reference'] = manifest;r['native_audio_reference_manifest_sha256'] = h;self.save('report.json', r);self.reseal('report.json')

    def test_changed_ctest(self):
        (self.profile / 'ctest.xml').write_bytes(b'<broken>')
        self.reject('Changed evidence file')

    def test_changed_test_result_resealed(self):
        tree = ET.fromstring((self.profile / 'ctest.xml').read_bytes());ET.SubElement(tree[0], 'failure')
        (self.profile / 'ctest.xml').write_bytes(ET.tostring(tree));self.reseal('ctest.xml');self.reject('Failing/skipped')

    def test_wrong_executable_hash(self):
        a = self.load('attestation.json');a['native_proof']['executables'][current.ALL_TESTS[0]]['sha256'] = '0' * 64
        self.save('build-provenance.json', a['native_proof']);self.save('attestation.json', a);self.reseal('build-provenance.json');self.reject('executable SHA')

    def test_executable_metadata_paths(self):
        n = current.ALL_TESTS[0]
        for path in ('Tests/../' + n + '.exe', 'Tests/./' + n + '.exe', 'C:/' + n + '.exe',
                     'Tests\\' + n + '.exe', '//server/share/' + n + '.exe'):
            with self.subTest(path=path):
                m = self.load('native-reference/manifest.json');m['tests'][n]['executable'] = path
                self.update_manifest(m);self.reject('Nonportable executable metadata')

    def test_wrong_compiler(self):
        self.change_proof('compiler_sha256', '0' * 64);self.reject('toolchain proof')

    def test_historical_compiler_is_not_current_hosted_admission(self):
        self.change_proof('compiler_sha256', current.load_package(SOURCE)['compiler_sha256'])
        self.reject('toolchain proof')

    def change_proof(self, key, value):
        a = self.load('attestation.json');a['native_proof'][key] = value;self.save('build-provenance.json', a['native_proof']);self.save('attestation.json', a);self.reseal('build-provenance.json')

    def test_wrong_juce(self):
        self.change_proof('juce_sha', '0' * 40);self.reject('toolchain proof')

    def test_missing_asan_instrumentation(self):
        self.profile = self.root / ('native-windows-msvc-asan-' + self.sha)
        self.change_proof('asan_all_units', False);self.reject('instrumentation')

    def test_missing_negative_proof(self):
        (self.profile / 'windows-native-reference-controls.log').write_bytes(b'PASS');self.reseal('windows-native-reference-controls.log');self.reject('negative-control proof')

    def test_forged_summary(self):
        (self.profile / 'actual-VST3.log').write_bytes(b'PASS');self.reseal('actual-VST3.log');self.reject('native host')

    def test_missing_attestation(self):
        (self.profile / 'attestation.json').unlink();self.reject('attestation.json')

    def test_malformed_attestation(self):
        (self.profile / 'attestation.json').write_bytes(b'{');self.reject('Malformed')

    def test_duplicate_json_keys(self):
        (self.profile / 'attestation.json').write_bytes(b'{"schema":1,"schema":1}');self.reject('Duplicate')

    def test_unexpected_directory(self):
        (self.profile / 'nested').mkdir();self.reject('Unexpected artifact directory')

    def test_paths_are_not_accepted_as_artifact_members(self):
        for path in ('../escape', '/etc/passwd', 'C:/escape', 'D:\\escape', '//server/share', 'a/../b'):
            with self.subTest(path=path):
                index = self.load('index.json');index[path] = dict(role='native-build-proof', size=0, sha256='0' * 64)
                self.save('index.json', index);a = self.load('attestation.json');a['index_sha256'] = current.digest((self.profile / 'index.json').read_bytes());self.save('attestation.json', a)
                self.reject('Missing/extra evidence member');index.pop(path);self.save('index.json', index)

    def test_symlink_or_junction_escape(self):
        target = Path(self.temp.name) / 'target';self.profile.rename(target)
        if os.name == 'nt':
            subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(self.profile), str(target)], check=True, capture_output=True)
            self.addCleanup(self.profile.rmdir)
        else:
            self.profile.symlink_to(target, target_is_directory=True)
        self.reject('link|junction|Untrusted')

    def test_wrong_case_inventory(self):
        n = current.ALL_TESTS[0];f = 'native-reference/' + n + '.json';r = self.load(f);r['records'].pop(next(iter(r['records'])))
        self.save(f, r);self.reseal(f);m = self.load('native-reference/manifest.json');m['tests'][n]['record_sha256'] = current.digest((self.profile / f).read_bytes());self.update_manifest(m);self.reject('Unauthorized reference mutation')

    def test_missing_executable(self):
        a = self.load('attestation.json');a['native_proof']['executables'].pop(current.ALL_TESTS[0]);self.save('build-provenance.json', a['native_proof']);self.save('attestation.json', a);self.reseal('build-provenance.json');self.reject('Missing native executable')

    def test_mixed_runs(self):
        self.profile = self.root / ('native-windows-msvc-Debug-' + self.sha)
        self.test_wrong_run()

    def test_skipped_ctest(self):
        tree = ET.fromstring((self.profile / 'ctest.xml').read_bytes());ET.SubElement(tree[0], 'skipped')
        (self.profile / 'ctest.xml').write_bytes(ET.tostring(tree));self.reseal('ctest.xml');self.reject('Failing/skipped')

    def test_missing_provenance(self):
        (self.profile / 'build-provenance.json').unlink();self.reject('Missing/extra artifact file')

    def test_missing_mandatory_upload(self):
        (self.profile / 'native-reference/manifest.sha256').unlink();self.reject('Missing/extra artifact file')

    def test_trusted_job_failure(self):
        with self.assertRaisesRegex(RuntimeError, 'Trusted native jobs'):
            self.check({'native': 'failure', 'preflight': 'success'})

    def test_native_failure_cannot_export_attestation(self):
        with patch.object(current, 'admit_reference', side_effect=RuntimeError('native rejection')):
            with self.assertRaisesRegex(RuntimeError, 'native rejection'):
                artifact.export(self.profile, SOURCE, self.identity(), Path('missing-build'))
        self.assertFalse((self.profile / 'windows-artifact').exists())

    def test_export_and_consume_synthetic(self):
        manifest = self.load('native-reference/manifest.json');proof = self.load('build-provenance.json')
        # CMake's original inventory need not be canonical. The exporter must
        # parse it without changing its contents and transport canonical bytes.
        inventory = self.load('ctest-inventory.json')
        inventory['backtraceGraph'] = {'files': ['D:/native/private/CMakeLists.txt']}
        inventory['tests'][0]['command'] = ['D:/native/build/test.exe']
        (self.profile / 'ctest-inventory.json').write_text(json.dumps(inventory, indent=2))
        with patch.object(current, 'admit_reference', return_value=manifest), patch.object(artifact, 'native_proof', return_value=proof):
            artifact.export(self.profile, SOURCE, self.identity(), Path('SYNTHETIC-NOT-A-BUILD'))
        artifact.admit_profile(self.profile / 'windows-artifact', SOURCE, self.identity())
        self.assertNotIn(b'D:/native', (self.profile / 'windows-artifact/ctest-inventory.json').read_bytes())

    def test_incomplete_native_campaign_cannot_export(self):
        report = self.load('report.json');report['decision'] = 'INCOMPLETE';self.save('report.json', report)
        with patch.object(current, 'admit_reference', return_value=self.load('native-reference/manifest.json')), \
                patch.object(artifact, 'native_proof', return_value=self.load('build-provenance.json')):
            with self.assertRaisesRegex(RuntimeError, 'incomplete; no attestation'):
                artifact.export(self.profile, SOURCE, self.identity(), Path('SYNTHETIC'))
        self.assertFalse((self.profile / 'windows-artifact').exists())

    def test_full_ctest_threshold_is_not_lowered(self):
        report = self.load('report.json');report['test_count'] = 37;self.save('report.json', report);self.reseal('report.json')
        self.reject('Incomplete full native CTest coverage')

    def test_native_command_failure(self):
        report = self.load('report.json')
        next(x for x in report['commands'] if x['name'] == 'ctest')['exit_code'] = 1
        self.save('report.json', report);self.reseal('report.json');self.reject('Missing successful native command')

    def test_unrelated_crash_is_not_reference_negative_proof(self):
        report = self.load('report.json')
        next(x for x in report['commands'] if x['name'] == 'reference-negative-control')['exit_code'] = -11
        self.save('report.json', report);self.reseal('report.json');self.reject('reference negative proof')

    def test_actual_reference_output_mutation(self):
        tree = ET.fromstring((self.profile / 'ctest.xml').read_bytes())
        output = tree[0].find('system-out')
        output.text = output.text.replace('hash=', 'hash=0', 1)
        (self.profile / 'ctest.xml').write_bytes(ET.tostring(tree));self.reseal('ctest.xml')
        self.reject('Malformed Windows audio record')

    def test_missing_sanitizer_sensitivity(self):
        self.profile = self.root / ('native-windows-msvc-asan-' + self.sha)
        (self.profile / 'asan-negative-control.log').write_bytes(b'PASS')
        self.reseal('asan-negative-control.log');self.reject('Missing sanitizer sensitivity proof')

    def test_wrong_debug_configuration(self):
        self.profile = self.root / ('native-windows-msvc-Debug-' + self.sha)
        self.change_proof('configuration', 'Release');self.reject('instrumentation/configuration')

    def test_extra_attestation_field(self):
        a = self.load('attestation.json');a['override'] = True;self.save('attestation.json', a)
        self.reject('Malformed attestation fields')

    def test_oversized_evidence_declaration(self):
        index = self.load('index.json');index['ctest.xml']['size'] = artifact.MAX_FILE + 1
        self.save('index.json', index);a = self.load('attestation.json')
        a['index_sha256'] = current.digest((self.profile / 'index.json').read_bytes());self.save('attestation.json', a)
        self.reject('Malformed evidence index entry')

    def test_portable_desktop_dispatch(self):
        import desktop
        with patch.dict(os.environ, {'PREFLIGHT_RESULT': 'success', 'NATIVE_RESULT': 'success'}), \
                patch('sys.stdout') as output:
            desktop.gate('Windows', self.root, dict(source_sha=self.sha, run_id='SYNTHETIC-RUN', trusted_root=self.temp.name))
        self.assertIn('WINDOWS_PORTABLE_GATE_PASS', ''.join(call.args[0] for call in output.write.call_args_list))

    def test_desktop_root_alias_is_rejected_before_resolution(self):
        import desktop
        alias = Path(self.temp.name) / 'alias'
        if os.name == 'nt':
            subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(alias), str(self.root)], check=True, capture_output=True)
            self.addCleanup(alias.rmdir)
        else:
            alias.symlink_to(self.root, target_is_directory=True)
        with patch.dict(os.environ, {'PREFLIGHT_RESULT': 'success', 'NATIVE_RESULT': 'success'}):
            with self.assertRaisesRegex(RuntimeError, 'artifact root link/junction'):
                desktop.gate('Windows', alias, dict(source_sha=self.sha, run_id='SYNTHETIC-RUN', trusted_root=self.temp.name))

    def test_workflow_contract(self):
        text = (SOURCE / '.github/workflows/windows-qualification.yml').read_text()
        self.assertIn('windows-artifact/', text)
        self.assertIn('merge-multiple: false', text)
        self.assertIn('diagnostic-windows-', text)
        self.assertIn('pattern: native-windows-msvc-*-${{ github.sha }}', text)
        self.assertEqual(text.count('if-no-files-found: error'), 3)


if __name__ == '__main__':
    unittest.main(verbosity=2)
