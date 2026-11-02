# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure admission negative controls. Fixtures are not native audio evidence."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import current_reference as current
import dual_svf_reference as dual

SOURCE = Path(__file__).resolve().parents[2]


class DualAdmissionControls(unittest.TestCase):
    def setUp(self):
        self.rows = []
        for name, (samples, fingerprint) in dual.authority(SOURCE).items():
            self.rows.append(dict(case=name, mode='Independent' if name.startswith('DualIndependent_') else 'Relative',
                                  calibration=name.split('_')[1], samples=samples, hash=fingerprint,
                                  finite=True, deterministic=True, parameters_unchanged=True, repeats=2))
        self.terminal = dual.TERMINAL

    def text(self):
        return '\n'.join(dual.PREFIX + json.dumps(row) for row in self.rows) + '\n' + self.terminal + '\n'

    def reject(self, reason, text=None, code=0):
        with self.assertRaisesRegex(RuntimeError, reason):
            dual.validate_output(self.text() if text is None else text, code, SOURCE)

    def test_01_exact_approved_twelve(self):
        self.assertEqual(len(dual.validate_output(self.text(), 0, SOURCE)['records']), 12)

    def test_02_changed_hash(self):
        self.rows[0]['hash'] = '0' * 16
        self.reject('Changed Dual audio hash')

    def test_03_missing_case(self):
        self.rows.pop()
        self.reject('Missing Dual case')

    def test_04_duplicate_case(self):
        self.rows.append(self.rows[0])
        self.reject('Duplicate Dual case')

    def test_05_additional_case(self):
        self.rows.append(dict(self.rows[0], case='unapproved'))
        self.reject('Unexpected Dual case')

    def test_06_wrong_case_count(self):
        self.terminal = self.terminal.replace('cases=12', 'cases=11')
        self.reject('incorrect terminal case/sample count')

    def test_07_wrong_calibration(self):
        self.rows[0]['calibration'] = 'wrong'
        self.reject('Wrong Dual calibration')

    def test_08_wrong_mode(self):
        self.rows[0]['mode'] = 'wrong'
        self.reject('Wrong Dual Section2 mode')

    def test_09_wrong_sample_count(self):
        self.rows[0]['samples'] -= 1
        self.reject('Invalid Dual sample count')

    def test_10_malformed(self):
        self.reject('Malformed reference JSON', 'DUAL_SVF_HEADLESS {broken}\n')

    def test_11_nonfinite_audio(self):
        self.rows[0]['finite'] = False
        self.reject('Nonfinite Dual audio')

    def test_12_process_failure(self):
        self.reject('unexpected process failure', code=1)

    def test_13_sanitizer_diagnostic(self):
        self.reject('sanitizer/assertion diagnostic', self.text() + 'AddressSanitizer: test diagnostic\n')

    def test_14_unapproved_reference_mutation(self):
        # Real package authentication path; no package is rewritten/resealed.
        original = current.safe_bytes
        def changed(root, relative):
            data = original(root, relative)
            return data + b' ' if str(relative) == current.PACKAGE_FILE else data
        with patch.object(current, 'safe_bytes', changed):
            self.reject('Reference checksum mismatch')

    def test_unapproved_golden_and_driver_mutation(self):
        original = current.safe_bytes
        for name in ('Tests/Filter/DualSvfCurrentLinuxGoldens.h', 'Tests/Filter/DualSvfSection2Goldens.h', 'Tests/Filter/DualSvfSection2TestsMain.cpp', dual.DRIVER):
            def changed(root, relative):
                data = original(root, relative)
                return data + b' ' if str(relative) == name else data
            with self.subTest(name=name), patch.object(current, 'safe_bytes', changed):
                self.reject('checksum mismatch|Unapproved Dual headless driver')

    def test_byte_exact_guard_reversal(self):
        path = SOURCE / 'Tests/Filter/DualSvfSection2TestsMain.cpp'
        self.assertEqual(current.digest(current.original_dual_driver(path.read_bytes())), current.DUAL_ORIGINAL_SHA256)

    def test_executable_and_source_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / dual.TEST_NAME
            binary.write_bytes(b'explicit synthetic executable identity fixture, never executed')
            identity = current.source_identity(SOURCE)
            fingerprint = current.digest(binary.read_bytes())
            self.assertEqual(len(dual.admit(SOURCE, binary, fingerprint, identity, self.text(), 0)['records']), 12)
            with self.assertRaisesRegex(RuntimeError, 'Wrong Dual source identity'):
                dual.admit(SOURCE, binary, fingerprint, '0' * 64, self.text(), 0)
            binary.write_bytes(b'tampered')
            with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                dual.admit(SOURCE, binary, fingerprint, identity, self.text(), 0)
            binary = binary.rename(binary.with_name('SwaraXTDualSvfSection2Tests'))
            with self.assertRaisesRegex(RuntimeError, 'Wrong Dual executable identity'):
                dual.admit(SOURCE, binary, fingerprint, identity, self.text(), 0)

    def test_versioned_full_scope_identity_is_explicit(self):
        self.assertIn(dual.TEST_NAME, current.SCOPES['full-native-72'])
        self.assertNotIn('SwaraXTDualSvfSection2Tests', current.SCOPES['full-native-72'])
        self.assertEqual(len(current.load_package(SOURCE)['tests']), 4)
        self.assertEqual(sum(x['case_count'] for x in current.load_package(SOURCE)['tests'].values()), 72)

    def test_historical_mixer_fingerprints_rejected(self):
        provenance = json.loads((SOURCE / 'Tests/References/DualSvfCurrentLinuxProvenance.json').read_text())
        for row in self.rows:
            row['hash'] = provenance['historical']['cases'][row['case']][1]
        self.reject('Changed Dual audio hash')

    def test_old_full_manifest_identity_is_not_aliased(self):
        package = current.load_package(SOURCE)
        self.assertEqual(package['migration']['old_dual_identity'], 'SwaraXTDualSvfSection2Tests')
        self.assertEqual(package['migration']['new_dual_identity'], dual.TEST_NAME)
        with self.assertRaisesRegex(RuntimeError, 'Unexpected native test identity'):
            current.validate_output('SwaraXTDualSvfSection2Tests', self.text(), 0, SOURCE)


if __name__ == '__main__':
    unittest.main()
