# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Negative controls for native audio evidence admission."""
from pathlib import Path
import re
import unittest
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


if __name__ == '__main__':
    unittest.main()
