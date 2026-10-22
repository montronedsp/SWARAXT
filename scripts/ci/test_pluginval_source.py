# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

import pluginval_source as tool


class ToolSourceRepair(unittest.TestCase):
    def test_real_git_text_attributes_use_explicit_lf(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def run(args):
                subprocess.run(args, cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            run(['git', 'init', '--quiet'])
            (root / '.gitattributes').write_bytes(b'* text=auto\n')
            path = root / 'example.cpp'; original = b'first line\nsecond line\n'
            path.write_bytes(original)
            run(['git', '-c', 'core.autocrlf=false', 'add', '.gitattributes', 'example.cpp'])
            path.unlink()
            run(['git', '-c', 'core.autocrlf=false', '-c', 'core.eol=crlf', 'checkout-index', '--all'])
            self.assertEqual(path.read_bytes(), original.replace(b'\n', b'\r\n'))
            path.unlink()
            run(tool.git_argv('checkout-index', '--all'))
            self.assertEqual(path.read_bytes(), original)

    def test_exact_owned_repair_and_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'pluginval-source'
            path = source / 'Source/tests/BasicTests.cpp'; path.parent.mkdir(parents=True)
            original = b'#include <thread>\noriginal state test\n' + tool.ANCHOR + b'\n'
            validator = source / 'Source/Validator.cpp'; validator_before = b'#include <thread>\nvalidator test\n'
            validator_after = b'#include <chrono>\n' + validator_before
            validator.write_bytes(validator_before)
            path.write_bytes(original)
            with self.assertRaises(RuntimeError): tool.apply(source, root / 'unrelated')
            with self.assertRaises(RuntimeError): tool.apply(source, root)
            self.assertEqual(path.read_bytes(), original)
            after = original.replace(tool.ANCHOR, tool.INSERT + tool.ANCHOR).replace(b'#include <thread>', b'#include <chrono>\n#include <thread>')
            with patch.object(tool, 'BEFORE_SHA256', hashlib.sha256(original).hexdigest()), \
                 patch.object(tool, 'AFTER_SHA256', hashlib.sha256(after).hexdigest()), \
                 patch.object(tool, 'PORTABILITY_FILES', {'Source/Validator.cpp': {
                    'before_sha256': hashlib.sha256(validator_before).hexdigest(),
                    'after_sha256': hashlib.sha256(validator_after).hexdigest()}}):
                validator.write_bytes(b'upstream drift\n')
                with self.assertRaises(RuntimeError): tool.apply(source, root)
                self.assertEqual(path.read_bytes(), original)
                validator.write_bytes(validator_before)
                record = tool.apply(source, root)
                self.assertEqual(path.read_bytes(), after)
                self.assertEqual(validator.read_bytes(), validator_after)
                self.assertEqual(record['upstream_fix_sha'], tool.FIX_SHA)
                with self.assertRaises(RuntimeError): tool.apply(source, root)


if __name__ == '__main__': unittest.main()
