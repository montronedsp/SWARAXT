#!/usr/bin/env python3
"""Regression checks for the Linux qualification verifier's command boundary."""
import contextlib
import io
import json
import os
from pathlib import Path
import runpy
import shlex
import sys
import tempfile
import unittest
from unittest.mock import patch


VERIFIER = Path(__file__).resolve().parents[2] / "scripts/verify-qualification.py"


class QualificationArguments(unittest.TestCase):
    def invoke(self, build):
        arguments = [str(VERIFIER), "--expect-build-type", "Release",
                     "--require-sanitizer", "address,undefined", "--", str(build)]
        with patch.object(sys, "argv", arguments), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as result:
                runpy.run_path(str(VERIFIER), run_name="__main__")
        return result.exception.code

    def test_build_paths_never_enter_ninja_arguments(self):
        for name in ("normal", "-fother.ninja", "--help", "build with spaces",
                     "build;echo injected", "build$(echo injected)"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                build = Path(directory) / name
                build.mkdir()
                compiler = str(Path(directory) / "fixture-compiler")
                (build / "CMakeCache.txt").write_text(
                    "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_GENERATOR:INTERNAL=Ninja\n"
                    f"CMAKE_CXX_COMPILER:FILEPATH={compiler}\n")
                flags = [compiler, "-O3", "-DNDEBUG", "-fsanitize=address,undefined",
                         "-fno-omit-frame-pointer", "-c", "Tests/sample.cpp"]
                (build / "compile_commands.json").write_text(json.dumps([
                    {"file": "Tests/sample.cpp", "arguments": flags,
                     "command": shlex.join(flags)}]))
                calls = []

                def output(command, **options):
                    calls.append((command, options))
                    if command == [compiler, "--version"]:
                        return "fixture compiler\n"
                    return shlex.join([compiler, "-fsanitize=address,undefined",
                                       "-o", "Tests/sample", "Tests/sample.o"]) + "\n"

                # Invoke with a relative, possibly option-looking path as well.
                previous = Path.cwd()
                try:
                    os.chdir(directory)
                    with patch("subprocess.check_output", side_effect=output):
                        self.assertEqual(self.invoke(Path(name)), 0)
                finally:
                    os.chdir(previous)
                self.assertEqual(len(calls), 2)
                self.assertEqual(calls[1][0], ["ninja", "-t", "commands"])
                self.assertEqual(calls[1][1], {"cwd": build.resolve(), "text": True})

    def test_invalid_directory_is_rejected_before_process_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "not-a-directory"
            file.write_text("fixture")
            for build in (file, Path(directory) / "missing"):
                with self.subTest(build=build), patch("subprocess.check_output") as output:
                    with contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(self.invoke(build), 2)
                    output.assert_not_called()


if __name__ == "__main__":
    unittest.main()
