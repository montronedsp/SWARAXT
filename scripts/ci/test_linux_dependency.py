# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail closed on dependency drift or an unowned source tree."""
from pathlib import Path
import tempfile
import unittest

from linux_dependency import EDITS, apply


class OwnedDependency(unittest.TestCase):
    def fixture(self, root, newline='\n'):
        source = root / 'JUCE'
        for name, old, _ in EDITS:
            path = source / name; path.parent.mkdir(parents=True, exist_ok=True)
            with path.open('ab') as stream: stream.write(old.replace('\n', newline).encode())
        return source

    def test_owned_patches_preserve_newlines_and_thread_guard(self):
        for newline in ('\n', '\r\n'):
            with self.subTest(newline=newline), tempfile.TemporaryDirectory() as d:
                root = Path(d); source = self.fixture(root, newline)
                records = apply(source, root)
                self.assertEqual(len(records), 3)
                for record in records:
                    self.assertNotEqual(record['before_sha256'], record['after_sha256'])
                    data = (source / record['file']).read_bytes()
                    if newline == '\r\n': self.assertNotIn(b'\n', data.replace(b'\r\n', b''))
                with self.assertRaises(RuntimeError): apply(source, root)

    def test_anchor_drift_is_transactional(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); source = self.fixture(root)
            enum = source / EDITS[0][0]; original = enum.read_bytes()
            (source / EDITS[1][0]).write_bytes(b'changed upstream source\n')
            with self.assertRaises(RuntimeError): apply(source, root)
            self.assertEqual(enum.read_bytes(), original)

    def test_external_checkout_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); source = self.fixture(root)
            with self.assertRaises(RuntimeError): apply(source, root / 'unrelated')


if __name__ == '__main__': unittest.main()
