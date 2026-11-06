# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""24 native admission controls; no GUI or CI is launched.

Select an MSVC Release build using SWARAXT_WINDOWS_REFERENCE_TEST_BUILD.
Without it, these native integration tests are explicitly skipped.
"""
import copy
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import windows_current_reference as policy

SOURCE = Path(__file__).resolve().parents[2]
BUILD = os.environ.get('SWARAXT_WINDOWS_REFERENCE_TEST_BUILD')


@unittest.skipUnless(os.name == 'nt' and BUILD, 'Native Windows build must be explicitly selected')
class NativeWindowsReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(BUILD)
        cls.package = policy.load_package(SOURCE)
        cls.storage = tempfile.TemporaryDirectory(prefix='swara-windows-admission-')
        cls.addClassCleanup(cls.storage.cleanup)
        root = Path(cls.storage.name)
        sha = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
        cls.identity = policy.context(SOURCE, sha, 'msvc-Release', 'native-negative-controls')
        (root / 'profile.json').write_bytes(policy.encode(cls.identity))
        executables = {}
        cls.output = {}
        for name in policy.ALL_TESTS:
            matches = list((cls.build / 'Tests').rglob(name + '.exe'))
            if len(matches) != 1:
                raise RuntimeError('Native control requires exactly one executable: ' + name)
            executables[name] = matches[0]
            result = subprocess.run([str(matches[0])], capture_output=True, text=True, timeout=300, cwd=root)
            text = result.stdout + result.stderr
            policy.validate_output(name, text, result.returncode, SOURCE)
            cls.output[name] = text
        policy.stage_reference(root, SOURCE, cls.identity, executables, cls.build)
        cls.original = root / 'native-reference'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='swara-negative-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'reference'
        shutil.copytree(self.original, self.root)
        self.manifest = policy.decode((self.root / 'manifest.json').read_bytes())
        self.name = policy.ALL_TESTS[0]
        self.text = self.output[self.name]

    def save_manifest(self):
        data = policy.encode(self.manifest)
        (self.root / 'manifest.json').write_bytes(data)
        (self.root / 'manifest.sha256').write_bytes((policy.digest(data) + '\n').encode('ascii'))

    def reject_manifest(self, reason):
        self.save_manifest()
        with self.assertRaisesRegex(RuntimeError, reason):
            policy.admit_reference(self.root, SOURCE, self.identity, self.build)

    def reject_output(self, text, reason, code=0, name=None):
        with self.assertRaisesRegex(RuntimeError, reason):
            policy.validate_output(name or self.name, text, code, SOURCE)

    def test_01_valid_approved_current(self):
        policy.admit_reference(self.root, SOURCE, self.identity, self.build)
        self.assertEqual(sum(len(policy.validate_output(n, t, 0, SOURCE)['records'])
                             for n, t in self.output.items()), 72)

    def test_02_modified_hash(self):
        self.reject_output(re.sub(r'hash=[0-9a-f]{16}', 'hash=' + '0' * 16, self.text, count=1),
                           'sample count/hash mismatch')

    def test_03_modified_sample_count(self):
        self.reject_output(self.text.replace('samples=44100', 'samples=44101', 1), 'sample count/hash mismatch')

    def test_04_missing_case(self):
        lines = self.text.splitlines()
        lines.pop(next(i for i, line in enumerate(lines) if line.startswith('GOLDEN ')))
        self.reject_output('\n'.join(lines), 'Missing/additional')

    def test_05_additional_case(self):
        self.reject_output(self.text + '\nGOLDEN classic extra samples=44100 hash=' + '0' * 16, 'Missing/additional')

    def test_06_duplicate_case(self):
        line = next(line for line in self.text.splitlines() if line.startswith('GOLDEN '))
        self.reject_output(self.text + '\n' + line, 'Duplicate Windows case')

    def test_07_malformed_output(self):
        self.reject_output(self.text + '\nGOLDEN malformed', 'Malformed Windows audio record')

    def test_08_corrupt_json(self):
        (self.root / 'manifest.json').write_bytes(b'{')
        (self.root / 'manifest.sha256').write_bytes((policy.digest(b'{') + '\n').encode('ascii'))
        with self.assertRaisesRegex(RuntimeError, 'Malformed Windows manifest JSON'):
            policy.admit_reference(self.root, SOURCE, self.identity, self.build)

    def test_09_duplicate_json_keys(self):
        data = b'{"schema":1,"schema":1}'
        (self.root / 'manifest.json').write_bytes(data)
        (self.root / 'manifest.sha256').write_bytes((policy.digest(data) + '\n').encode('ascii'))
        with self.assertRaisesRegex(RuntimeError, 'Duplicate Windows manifest key'):
            policy.admit_reference(self.root, SOURCE, self.identity, self.build)

    def test_10_wrong_source_sha(self):
        self.manifest['candidate_sha'] = '0' * 40
        self.reject_manifest('source/profile/run identity')

    def test_11_wrong_executable_sha(self):
        self.manifest['tests'][self.name]['executable_sha256'] = '0' * 64
        self.reject_manifest('checksum mismatch')

    def test_12_wrong_build_configuration(self):
        self.manifest['profile'] = 'msvc-Debug'
        self.reject_manifest('source/profile/run identity')
        with self.assertRaisesRegex(RuntimeError, 'build configuration'):
            policy.toolchain(self.build, 'msvc-Debug', self.package)

    def test_13_wrong_reference_package(self):
        self.manifest['package_sha256'] = '0' * 64
        self.reject_manifest('reference package/schema')

    def test_14_wrong_compiler_profile(self):
        self.manifest['toolchain']['compiler_sha256'] = '0' * 64
        self.reject_manifest('toolchain binding')
        with self.assertRaisesRegex(RuntimeError, 'Uninstrumented Windows ASan'):
            policy.toolchain(self.build, 'msvc-asan', self.package)

    def test_15_wrong_run_identity(self):
        self.manifest['run_id'] = 'other-run'
        self.reject_manifest('source/profile/run identity')

    def test_16_junction_escape(self):
        link = Path(self.temp.name) / 'junction'
        subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(link), str(self.root)], check=True, capture_output=True)
        try:
            with self.assertRaisesRegex(RuntimeError, 'link/junction'):
                policy.admit_reference(link, SOURCE, self.identity, self.build)
        finally:
            link.rmdir()  # Only remove this test-created junction, never its target.

    def test_17_path_traversal(self):
        self.manifest['tests'][self.name]['executable'] = '../' + self.name + '.exe'
        self.reject_manifest('path escape')

    def test_18_unexpected_fail(self):
        self.reject_output(self.text + '\nFAIL: regression', 'unexpected FAIL')

    def test_19_nonzero_exit(self):
        self.reject_output(self.text, 'nonzero process exit', code=1)

    def test_20_asan_diagnostic(self):
        self.reject_output(self.text + '\nERROR: AddressSanitizer: heap-buffer-overflow', 'sanitizer/assertion diagnostic')

    def test_21_juce_assertion(self):
        self.reject_output(self.text + '\nJUCE Assertion failure in file.cpp:1', 'sanitizer/assertion diagnostic')

    def test_22_obsolete_historical_mixer(self):
        name = policy.ALL_TESTS[3]
        current = next(iter(self.package['tests'][name]['cases'].values()))[1]
        historical = re.search(r'0x([0-9a-f]{16})', (SOURCE / 'Tests/Filter/DualSvfSection2Goldens.h').read_text())[1]
        self.assertNotEqual(current, historical)
        self.reject_output(self.output[name].replace(current, historical, 1), 'sample count/hash mismatch', name=name)

    def test_23_unauthorized_reference_mutation(self):
        entry = self.manifest['tests'][self.name]
        path = self.root / entry['record_file']
        record = policy.decode(path.read_bytes())
        next(iter(record['records'].values()))[1] = '0' * 16
        data = policy.encode(record)
        path.write_bytes(data)
        entry['record_sha256'] = policy.digest(data)
        self.reject_manifest('Unauthorized Windows reference mutation')

    def test_24_focused_scope_impersonation(self):
        identity = copy.deepcopy(self.identity)
        identity['scope'] = 'focused-current-audio-30'
        with self.assertRaisesRegex(RuntimeError, 'full scope'):
            policy.admit_reference(self.root, SOURCE, identity, self.build)


@unittest.skipUnless(os.name == 'nt', 'Fixture admission tests require Windows')
class FixtureAdmissionTests(unittest.TestCase):
    """Positive and negative controls for AudioRegressionTestsMain.cpp fixture admission.

    Verifies that the Mission 06 source-fixture migration is authenticated
    strictly: only the exact reviewed PR #22 transformation is accepted.
    """

    RELATIVE = 'Tests/AudioRegression/AudioRegressionTestsMain.cpp'
    EXPECTED = '9c27e06bf0dd56185f49d92fa316da51cd10abe9e76a071e7085c2751dc04592'

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='swara-fixture-')
        self.addCleanup(self.tmp.cleanup)
        self.tmp_root = Path(self.tmp.name)

    def _write_fixture(self, content: bytes):
        target = self.tmp_root / self.RELATIVE
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)

    def test_positive_current_source_accepted(self):
        """Positive control: current post-Mission-06 source accepted via reverse transformation."""
        policy.checked_fixture(SOURCE, self.RELATIVE, self.EXPECTED)

    def test_positive_original_source_accepted(self):
        """Positive control: original pre-Mission-06 source accepted directly."""
        # Reconstruct the historical source via the same reverse transformation
        # that checked_fixture applies. Do not depend on git show f608479 being
        # available in a shallow GitHub Actions checkout.
        data = (SOURCE / self.RELATIVE).read_bytes()
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
        self._write_fixture(original)
        policy.checked_fixture(self.tmp_root, self.RELATIVE, self.EXPECTED)

    def test_negative_changed_filter_call(self):
        """Negative control: changed filter-capture migration statement is rejected."""
        current = (SOURCE / self.RELATIVE).read_bytes()
        modified = current.replace(
            b'filter.processInstrumentSample(s, 1.0f)',
            b'filter.processInstrumentSample(s, 0.5f)'
        )
        self._write_fixture(modified)
        with self.assertRaises(RuntimeError):
            policy.checked_fixture(self.tmp_root, self.RELATIVE, self.EXPECTED)

    def test_negative_single_char_change(self):
        """Negative control: unrelated parameter change is rejected."""
        current = (SOURCE / self.RELATIVE).read_bytes()
        modified = current.replace(b'8000.0f', b'8001.0f', 1)
        self._write_fixture(modified)
        with self.assertRaises(RuntimeError):
            policy.checked_fixture(self.tmp_root, self.RELATIVE, self.EXPECTED)

    def test_negative_missing_process_call(self):
        """Negative control: removed filter call is rejected."""
        current = (SOURCE / self.RELATIVE).read_bytes()
        normalized = current.replace(b'\r\n', b'\n')
        target = b'        capture.output.push_back(filter.processInstrumentSample(s, 1.0f));\n'
        self.assertEqual(normalized.count(target), 1, 'Expected filter call to occur exactly once')
        modified = normalized.replace(target, b'')
        self._write_fixture(modified)
        with self.assertRaises(RuntimeError):
            policy.checked_fixture(self.tmp_root, self.RELATIVE, self.EXPECTED)

    def test_negative_modified_expected_checksum(self):
        """Negative control: modified historical expected checksum is rejected."""
        with self.assertRaises(RuntimeError):
            policy.checked_fixture(SOURCE, self.RELATIVE, '0' * 64)


if __name__ == '__main__':
    unittest.main(verbosity=2)
