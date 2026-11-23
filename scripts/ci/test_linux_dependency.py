# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Future verify-only Linux identity contracts; no shared dependency mutation."""
from pathlib import Path
import hashlib
import tempfile
import unittest
from unittest.mock import patch
from linux_dependency import apply, STOCK_904_SHA

class OwnedDependency(unittest.TestCase):
    def fixture(self, root):
        source = root / 'JUCE'; source.mkdir()
        (source / 'CMakeLists.txt').write_text('fixture source\n')
        return source

    def test_clean_exact_dependency_is_not_modified(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); source = self.fixture(root)
            before = (source / 'CMakeLists.txt').read_bytes()
            hashes = {'CMakeLists.txt': hashlib.sha256(before).hexdigest()}
            with patch('linux_dependency.STOCK_904_HASHES', hashes), patch('linux_dependency.subprocess.check_output', side_effect=[STOCK_904_SHA, '']):
                self.assertEqual(apply(source, root), [])
            self.assertEqual((source / 'CMakeLists.txt').read_bytes(), before)

    def test_revision_and_tracked_untracked_ignored_drift_rejected_without_writes(self):
        for revision, dirty in [('0'*40, ''), (STOCK_904_SHA, ' M changed'), (STOCK_904_SHA, '?? extra'), (STOCK_904_SHA, '!! ignored')]:
            with self.subTest(revision=revision, dirty=dirty), tempfile.TemporaryDirectory() as d:
                root = Path(d); source = self.fixture(root)
                before = (source / 'CMakeLists.txt').read_bytes()
                with patch('linux_dependency.subprocess.check_output', side_effect=[revision, dirty]):
                    with self.assertRaises(RuntimeError): apply(source, root)
                self.assertEqual((source / 'CMakeLists.txt').read_bytes(), before)

    def test_hash_drift_is_transactional(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); source = self.fixture(root)
            before = (source / 'CMakeLists.txt').read_bytes()
            with patch('linux_dependency.STOCK_904_HASHES', {'CMakeLists.txt': '0'*64}), patch('linux_dependency.subprocess.check_output', side_effect=[STOCK_904_SHA, '']):
                with self.assertRaises(RuntimeError): apply(source, root)
            self.assertEqual((source / 'CMakeLists.txt').read_bytes(), before)

    def test_unowned_input_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); source = self.fixture(root)
            with self.assertRaises(RuntimeError): apply(source, root/'unrelated')

if __name__ == '__main__': unittest.main()
