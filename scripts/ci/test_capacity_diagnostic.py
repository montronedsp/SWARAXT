# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise CMake's actual exit-status decoding for the RTSan fault control."""
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest

from common import SOURCE


@unittest.skipUnless(platform.system() == 'Linux', 'RTSan control is Linux-only')
class CapacityDiagnostic(unittest.TestCase):
    def execute(self, ending, markers=True):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'fake-capacity-control'
            text = 'RealtimeSanitizer: unsafe-library-call realloc MidiBuffer::addEvent flushSequencerMidi' if markers else 'unrelated error'
            executable.write_text('#!' + sys.executable + '\nimport os, resource, signal\n'
                'resource.setrlimit(resource.RLIMIT_CORE, (0, 0))\n'
                'print(' + repr(text) + ', flush=True)\n' + ending + '\n')
            executable.chmod(0o700)
            return subprocess.run(['cmake', '-DTEST_EXECUTABLE=' + str(executable), '-P',
                str(SOURCE / 'Tests/Support/ExpectMidiCapacityDiagnostic.cmake')],
                capture_output=True, text=True, timeout=10)

    def test_actual_abort_and_nonzero_are_expected_faults(self):
        aborted = self.execute('os.abort()')
        self.assertIn('Subprocess aborted', aborted.stderr)
        self.assertEqual(aborted.returncode, 0, aborted.stderr)
        self.assertEqual(self.execute('raise SystemExit(1)').returncode, 0)

    def test_success_unrelated_crash_and_missing_markers_fail(self):
        for ending, markers in [('raise SystemExit(0)', True),
                                ('os.kill(os.getpid(), signal.SIGKILL)', True),
                                ('os.abort()', False), ('raise SystemExit(1)', False)]:
            with self.subTest(ending=ending, markers=markers):
                self.assertNotEqual(self.execute(ending, markers).returncode, 0)


if __name__ == '__main__': unittest.main()
