# Copyright 2026 MontroneDSP.
# SPDX-License-Identifier: GPL-3.0-or-later

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest


class FactoryGeneratorTests(unittest.TestCase):
    def test_generation_reproduces_shipping_header(self):
        repo = Path(__file__).resolve().parents[2]
        sys.dont_write_bytecode = True
        spec = importlib.util.spec_from_file_location(
            "factory_generator", repo / "Tools/generate_shruthi_factory_presets.py")
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        with tempfile.TemporaryDirectory(prefix="swaraxt_factory_") as temporary:
            output = Path(temporary) / "factory.h"
            generator.OUTPUT_HEADER = output
            with contextlib.redirect_stdout(io.StringIO()):
                generator.main()
                first = output.read_bytes()
                generator.main()
            self.assertEqual(first, output.read_bytes())
            self.assertEqual(first.decode("utf-8"),
                             (repo / "Source/Plugin/ShruthiFactoryPresetData.h")
                             .read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
