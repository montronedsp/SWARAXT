# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""SIMULATED_POLICY_TEST only: no Windows executable or CI is run."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import native_reference
import windows_reference_policy as policy

SOURCE = Path(__file__).resolve().parents[2]


class SimulatedWindowsPolicyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.identity = dict(candidate_sha='c' * 40, profile='msvc-Release',
                             run_id='SIMULATED_POLICY_TEST', architecture='x86_64')
        self.manifest = dict(self.identity, baseline_sha=policy.BASELINE_SHA, tests={})
        for name in policy.TEST_NAMES:
            path = self.root / (name + '.json')
            path.write_bytes(b'{"records":{"synthetic":[25600,"aaaaaaaaaaaaaaaa"]}}\n')
            self.manifest['tests'][name] = dict(record_file=path.name,
                                              record_sha256=policy.digest(path.read_bytes()))
        self.expected = {'synthetic-A': [25600, 'a' * 16], 'synthetic-B': [25600, 'b' * 16]}
        self.text = '\n'.join('GOLDEN integrated %s samples=%d hash=%s' % (name, *row)
                              for name, row in self.expected.items())

    def reject_manifest(self, pattern):
        with self.assertRaisesRegex(RuntimeError, pattern):
            policy.admit_manifest(self.root, self.manifest, self.identity)

    def test_valid_synthetic_manifest(self):
        policy.admit_manifest(self.root, self.manifest, self.identity)

    def test_stale_baseline(self):
        self.manifest['baseline_sha'] = '0' * 40
        self.reject_manifest('baseline source')

    def test_wrong_source_profile_run_architecture(self):
        for field in self.identity:
            with self.subTest(field=field):
                original = self.manifest[field]
                self.manifest[field] = 'wrong'
                self.reject_manifest('source/profile/run/architecture')
                self.manifest[field] = original

    def test_missing_manifest_case(self):
        self.manifest['tests'].pop(policy.TEST_NAMES[0])
        self.reject_manifest('Incomplete')

    def test_extra_manifest_case(self):
        self.manifest['tests']['unexpected'] = {}
        self.reject_manifest('Incomplete')

    def test_duplicate_manifest_key(self):
        with self.assertRaisesRegex(RuntimeError, 'Duplicate'):
            policy.strict_json('{"tests":{},"tests":{}}')

    def test_malformed_manifest(self):
        for data in ('{', '{"x":NaN}', b'\xff'):
            with self.subTest(data=data), self.assertRaisesRegex(RuntimeError, 'Malformed'):
                policy.strict_json(data)

    def test_mutated_record(self):
        (self.root / (policy.TEST_NAMES[0] + '.json')).write_text('{}')
        self.reject_manifest('checksum')

    def test_wrong_record_name(self):
        self.manifest['tests'][policy.TEST_NAMES[0]]['record_file'] = '../escape'
        self.reject_manifest('filename')

    def test_trusted_path_enforcement(self):
        for relative in ('../escape', '/etc/passwd', 'C:/escape', 'sub\\escape', 'sub//escape'):
            with self.subTest(relative=relative), self.assertRaisesRegex(RuntimeError, 'path escape'):
                policy.trusted_file(self.root, relative)
        path = self.root / 'linked'
        try:
            path.symlink_to(self.root / (policy.TEST_NAMES[0] + '.json'))
        except OSError as error:
            self.skipTest('symlink unavailable: ' + str(error))
        with self.assertRaisesRegex(RuntimeError, 'link/junction'):
            policy.trusted_file(self.root, path.name)
        linkroot = self.root / 'linked-root'
        linkroot.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'link/junction'):
            policy.trusted_file(linkroot, policy.TEST_NAMES[0] + '.json')

    def test_synthetic_driver_identity_and_wrong_source_rejection(self):
        path = self.root / 'driver.cpp'
        path.write_bytes(b'explicitly synthetic test driver\n')
        with patch.object(policy, 'FROZEN_FILES', {'driver.cpp': policy.digest(path.read_bytes())}):
            policy.require_frozen_source(self.root)
            path.write_text('wrong source')
            with self.assertRaisesRegex(RuntimeError, 'frozen driver/header identity is stale'):
                policy.require_frozen_source(self.root)
            path.unlink()
            with self.assertRaisesRegex(RuntimeError, 'frozen driver/header identity is stale'):
                policy.require_frozen_source(self.root)

    def test_actual_current_source_is_blocked(self):
        # Real source bytes, pure preflight only; never Windows runtime evidence.
        self.assertGreaterEqual(len(policy.source_divergence(SOURCE)), 3)
        with self.assertRaisesRegex(RuntimeError, 'WINDOWS_NATIVE_QUALIFICATION_REQUIRED'):
            policy.require_frozen_source(SOURCE)

    def test_no_native_current_authority(self):
        with self.assertRaisesRegex(RuntimeError, 'no independently qualified current MSVC'):
            policy.validate_current_output(self.text, 0)

    def test_synthetic_current_records(self):
        self.assertEqual(policy.validate_current_output(self.text, 0, self.expected), self.expected)

    def test_missing_current_case(self):
        with self.assertRaisesRegex(RuntimeError, 'Missing/additional'):
            policy.validate_current_output(self.text.splitlines()[0], 0, self.expected)

    def test_duplicate_current_case(self):
        with self.assertRaisesRegex(RuntimeError, 'Duplicate Windows current case'):
            policy.validate_current_output(self.text + '\n' + self.text.splitlines()[0], 0, self.expected)

    def test_changed_hash(self):
        with self.assertRaisesRegex(RuntimeError, 'sample/hash mismatch'):
            policy.validate_current_output(self.text.replace('a' * 16, 'c' * 16), 0, self.expected)

    def test_nonzero_rejected(self):
        for code in (1, 2, -11):
            with self.subTest(code=code), self.assertRaisesRegex(RuntimeError, 'nonzero'):
                policy.validate_current_output(self.text, code, self.expected)

    def test_diagnostic_rejected(self):
        for diagnostic in ('AddressSanitizer: error', 'runtime error: invalid enum',
                           'JUCE Assertion failure', 'Leaked objects detected:'):
            with self.subTest(diagnostic=diagnostic), self.assertRaisesRegex(RuntimeError, 'diagnostic'):
                policy.validate_current_output(self.text + '\n' + diagnostic, 0, self.expected)

    def test_platform_dispatch(self):
        # Pure dispatch mocks: do not call either orchestration entry point.
        for name, method in (('Linux', 'current_reference.main'), ('Windows', 'windows_main')):
            with self.subTest(platform=name), patch('native_reference.platform.system', return_value=name), \
                    patch('native_reference.' + method, return_value='SIMULATED_POLICY_TEST') as dispatch:
                self.assertEqual(native_reference.main(), 'SIMULATED_POLICY_TEST')
                dispatch.assert_called_once_with()


if __name__ == '__main__':
    unittest.main()
