# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import tempfile
import unittest

from boolean_dependency import apply, FILE, OLD, NEW


class BooleanDependency(unittest.TestCase):
    def test_owned_patch_and_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'JUCE'; path = source / FILE
            path.parent.mkdir(parents=True); original = b'prefix\r\n' + OLD + b'\r\nsuffix\r\n'
            path.write_bytes(original)
            with self.assertRaises(RuntimeError): apply(source, root / 'unrelated')
            self.assertEqual(path.read_bytes(), original)
            record = apply(source, root)
            self.assertEqual(path.read_bytes(), original.replace(OLD, NEW))
            self.assertNotEqual(record['before_sha256'], record['after_sha256'])
            with self.assertRaises(RuntimeError): apply(source, root)


if __name__ == '__main__': unittest.main()
