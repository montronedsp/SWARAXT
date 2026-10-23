# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Negative controls for native audio evidence admission."""
import os
from pathlib import Path
import re
import tempfile
import unittest
from common import sha
from desktop import admit_reference_record, contained_runner_path, gate
from native_reference import TESTS, validate_output


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
                    gate('Windows', evidence, Path(outside) / 'gate.json')
                with self.assertRaisesRegex(RuntimeError, 'escaped RUNNER_TEMP'):
                    gate('Windows', Path(outside), Path(runner) / 'gate.json')
            finally:
                if previous is None:
                    os.environ.pop('RUNNER_TEMP', None)
                else:
                    os.environ['RUNNER_TEMP'] = previous


if __name__ == '__main__':
    unittest.main()
