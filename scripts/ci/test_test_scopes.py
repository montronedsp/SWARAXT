# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real CMake directory-scope regression and baseline metadata controls."""
import json
import platform
import shutil
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from reference_metadata import apply, PARENT_BEFORE, PARENT_AFTER, CHILD_BEFORE, CHILD_AFTER


class TestScopes(unittest.TestCase):
    @unittest.skipUnless(platform.system() == 'Linux', 'Linux loader contract')
    def test_fault_shim_is_built_and_missing_preload_is_rejected(self):
        source = Path(__file__).resolve().parents[2]
        text = (source / 'Tests/CMakeLists.txt').read_text()
        # Exercise the actual target/CTest declarations, with a zero-exit dummy
        # caller. This is build/loader regression evidence, not product testing.
        begin = text.index('    add_library(SwaraXTPresetFilesystemFaults SHARED')
        end = text.index('    endforeach()', begin) + len('    endforeach()')
        declarations = text[begin:end]
        # ld.so separates LD_PRELOAD entries at spaces; use a private space-free
        # Linux tmpfs path rather than a caller's possibly spaced TMPDIR.
        with tempfile.TemporaryDirectory(prefix='swara-loader-contract-', dir='/tmp') as directory:
            root = Path(directory); (root / 'Regression').mkdir()
            shutil.copyfile(source / 'Tests/Regression/PresetFilesystemFaults.cpp',
                            root / 'Regression/PresetFilesystemFaults.cpp')
            (root / 'caller.cpp').write_text('int main(){return 0;}\n')
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.22)\nproject(LoaderContract CXX)\nenable_testing()\n'
                'add_executable(SwaraXTControlIntegrityTests caller.cpp)\n' + declarations + '\n'
                'add_custom_target(SwaraXTQualificationTests DEPENDS SwaraXTControlIntegrityTests)\n')
            build = root / 'build'
            for argv in (['cmake', '-S', root, '-B', build, '-G', 'Ninja'],
                         ['cmake', '--build', build, '--target', 'SwaraXTQualificationTests', '--parallel', '1']):
                result = subprocess.run(argv, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            libraries = list(build.rglob('libSwaraXTPresetFilesystemFaults.so'))
            self.assertEqual(len(libraries), 1, 'Aggregate must build the preload dependency')
            inventory = json.loads(subprocess.check_output(
                ['ctest', '--test-dir', build, '--show-only=json-v1'], text=True))
            self.assertEqual(len(inventory['tests']), 3)
            for test in inventory['tests']:
                props = {p['name']: p['value'] for p in test['properties']}
                self.assertIn('LD_PRELOAD=' + str(libraries[0]), props['ENVIRONMENT'])
                self.assertEqual(props['FAIL_REGULAR_EXPRESSION'], ['cannot be preloaded'])
            valid = subprocess.run(['ctest', '--test-dir', build, '--output-on-failure'], capture_output=True, text=True)
            self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
            # Recreate the original missing-library error. The caller still
            # exits zero, so only the real loader diagnostic rejects this run.
            libraries[0].rename(libraries[0].with_suffix('.withheld'))
            missing = subprocess.run(['ctest', '--test-dir', build, '--output-on-failure'], capture_output=True, text=True)
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn('cannot be preloaded', missing.stdout + missing.stderr)
            self.assertIn('3 tests failed out of 3', missing.stdout + missing.stderr)

    def test_tuning_fuzz_regression_is_in_the_qualification_aggregate(self):
        source = Path(__file__).resolve().parents[2]
        text = (source / 'Tests/Qualification.cmake').read_text()
        pattern = re.search(r'NOT target MATCHES "([^"]+)"', text)[1]
        with tempfile.TemporaryDirectory(prefix='swara qualification inventory ') as directory:
            script = Path(directory) / 'inventory.cmake'
            checks = []
            for name, excluded in (('SwaraXTTuningFuzz', True),
                                   ('SwaraXTStateMidiFuzz', True),
                                   ('SwaraXTFilterCpuBench', True),
                                   ('SwaraXTTuningFuzzRegressionTests', False),
                                   ('SwaraXTTuningTableTests', False)):
                condition = 'NOT ' if excluded else ''
                checks.append(f'if({condition}"{name}" MATCHES "{pattern}")\n'
                              f'  message(FATAL_ERROR "Wrong aggregate membership: {name}")\nendif()\n')
            script.write_text(''.join(checks))
            result = subprocess.run(['cmake', '-P', script], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Recreate the exact prefix collision, rather than only comparing text.
            script.write_text(script.read_text().replace('SwaraXTTuningFuzz$', 'SwaraXTTuningFuzz'))
            result = subprocess.run(['cmake', '-P', script], text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Wrong aggregate membership: SwaraXTTuningFuzzRegressionTests', result.stderr)

    def test_actual_parent_and_child_metadata_configure_and_label_all_cases(self):
        source = Path(__file__).resolve().parents[2]
        parent = (source / 'Tests/Qualification.cmake').read_text()
        child = (source / 'Tests/DspBoard/CMakeLists.txt').read_text()
        parent_block = re.search(r'if\(MSVC\)\n(\s*set_tests_properties\(SwaraXtShruthiSvf2pSanitizerTests.*?\)\n)endif\(\)', parent, re.S)[1]
        child_block = re.search(r'(    set_tests_properties\(SwaraBoardSanitizerTests SwaraBoardIntegrationSanitizerTests PROPERTIES.*?\)\n)endif\(\)', child, re.S)[1]
        with tempfile.TemporaryDirectory(prefix='swara ctest scopes ') as directory:
            root = Path(directory); (root / 'board').mkdir()
            (root / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.22)\nproject(TestScopes NONE)\nenable_testing()\nadd_subdirectory(board)\nadd_test(NAME SwaraXtShruthiSvf2pSanitizerTests COMMAND "${CMAKE_COMMAND}" -E true)\n' + parent_block)
            (root / 'board/CMakeLists.txt').write_text('add_test(NAME SwaraBoardSanitizerTests COMMAND "${CMAKE_COMMAND}" -E true)\nadd_test(NAME SwaraBoardIntegrationSanitizerTests COMMAND "${CMAKE_COMMAND}" -E true)\n' + child_block)
            result = subprocess.run(['cmake', '-S', root, '-B', root / 'build', '-G', 'Ninja'],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            listing = json.loads(subprocess.check_output(['ctest', '--test-dir', root / 'build', '--show-only=json-v1'], text=True))
            self.assertEqual(len(listing['tests']), 3)
            for test in listing['tests']:
                props = {item['name']: item['value'] for item in test['properties']}
                self.assertEqual(props['LABELS'], ['sanitizer'])
                self.assertEqual(props['TIMEOUT'], 600)
            # Recreate the original scope error and require CMake to reject it.
            original = (root / 'CMakeLists.txt').read_text().replace(PARENT_AFTER, PARENT_BEFORE)
            (root / 'CMakeLists.txt').write_text(original)
            result = subprocess.run(['cmake', '-S', root, '-B', root / 'broken', '-G', 'Ninja'],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Can not find test', result.stdout)

    def fixture(self, root):
        source = root / 'frozen-upstream'
        (source / 'Tests/DspBoard').mkdir(parents=True)
        (source / 'Tests/Qualification.cmake').write_text(PARENT_BEFORE)
        (source / 'Tests/DspBoard/CMakeLists.txt').write_text(CHILD_BEFORE + '        ENVIRONMENT "unchanged")\n')
        return source

    def test_owned_baseline_metadata_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = self.fixture(root)
            records = apply(source, root)
            self.assertEqual(len(records), 2)
            self.assertEqual((source / 'Tests/Qualification.cmake').read_text(), PARENT_AFTER)
            self.assertIn(CHILD_AFTER, (source / 'Tests/DspBoard/CMakeLists.txt').read_text())
            self.assertTrue(all(r['before_sha256'] != r['after_sha256'] for r in records))
            with self.assertRaises(RuntimeError): apply(source, root / 'another-owner')

    def test_changed_anchor_is_rejected_without_partial_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = self.fixture(root)
            (source / 'Tests/DspBoard/CMakeLists.txt').write_text('unexpected upstream metadata')
            with self.assertRaises(RuntimeError): apply(source, root)
            self.assertEqual((source / 'Tests/Qualification.cmake').read_text(), PARENT_BEFORE)

    def test_crlf_metadata_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = self.fixture(root)
            for name in ('Tests/Qualification.cmake', 'Tests/DspBoard/CMakeLists.txt'):
                path = source / name
                path.write_bytes(path.read_text().replace('\n', '\r\n').encode())
            apply(source, root)
            for name in ('Tests/Qualification.cmake', 'Tests/DspBoard/CMakeLists.txt'):
                data = (source / name).read_bytes()
                self.assertIn(b'\r\n', data)
                self.assertNotIn(b'\n', data.replace(b'\r\n', b''))


if __name__ == '__main__': unittest.main()
