# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real CMake directory-scope regression and baseline metadata controls."""
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from reference_metadata import apply, PARENT_BEFORE, PARENT_AFTER, CHILD_BEFORE, CHILD_AFTER


class TestScopes(unittest.TestCase):
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
