# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Negative controls for portable CI admission, archives and instrumentation."""
import os
from pathlib import Path
import json
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from common import architecture, command_argv, extract_zip
from desktop import LINUX_PROFILES, WINDOWS_PROFILES, profile_flags, validate_ctest, verify_binary, validate_msvc_defaults, validate_host_log, validate_peer_teardown_control, validate_compile_commands


class Profiles(unittest.TestCase):
    def test_windows_has_only_supported_native_profiles(self):
        self.assertEqual(WINDOWS_PROFILES, [('msvc', 'Release'), ('msvc', 'Debug'), ('msvc', 'asan')])
        for profile in ('tsan', 'asan-ubsan', 'rtsan', 'fuzz'):
            with self.assertRaises(RuntimeError): profile_flags('Windows', 'msvc', profile)

    def test_linux_diagnostics_are_separate_profiles(self):
        self.assertEqual(len(LINUX_PROFILES), 8)
        self.assertIn('-fsanitize=thread', profile_flags('Linux', 'clang', 'tsan'))
        self.assertIn('-fsanitize=realtime', profile_flags('Linux', 'clang', 'rtsan'))
        self.assertIn('-fno-sanitize-recover=all', profile_flags('Linux', 'clang', 'asan-ubsan'))

    def test_no_unix_flags_in_msvc(self):
        self.assertEqual(profile_flags('Windows', 'msvc', 'asan'), ['/fsanitize=address', '/Zi'])

    def test_cross_platform_or_unknown_combination_rejected(self):
        for system, compiler in [('Darwin', 'clang'), ('Linux', 'msvc'), ('Windows', 'gcc')]:
            with self.assertRaises(RuntimeError): profile_flags(system, compiler, 'Release')

    def test_windows_architecture_normalizes(self):
        with patch('platform.machine', return_value='AMD64'): self.assertEqual(architecture(), 'x86_64')

    def test_windows_resolves_executable_from_imported_compiler_path(self):
        with patch('shutil.which', return_value='C:/Native Tools/cl.exe') as find:
            self.assertEqual(command_argv(['cl', '/Bv'], {'PATH': 'native compiler path'}, 'Windows'),
                             ['C:/Native Tools/cl.exe', '/Bv'])
            find.assert_called_once_with('cl', path='native compiler path')
        with patch('shutil.which', return_value=None):
            with self.assertRaises(RuntimeError): command_argv(['cl'], {'PATH': ''}, 'Windows')


class Archives(unittest.TestCase):
    def test_rejects_escape_and_windows_backslash(self):
        for member in ('../escaped', '/absolute', '..\\escaped'):
            with self.subTest(member=member), tempfile.TemporaryDirectory() as d:
                archive = Path(d) / 'bad.zip'
                with zipfile.ZipFile(archive, 'w') as stream: stream.writestr(member, b'x')
                with self.assertRaises(RuntimeError): extract_zip(archive, Path(d) / 'output')

    def test_rejects_symlinks(self):
        with tempfile.TemporaryDirectory() as d:
            archive = Path(d) / 'bad.zip'
            entry = zipfile.ZipInfo('link'); entry.create_system = 3; entry.external_attr = 0o120777 << 16
            with zipfile.ZipFile(archive, 'w') as stream: stream.writestr(entry, '../escaped')
            with self.assertRaises(RuntimeError): extract_zip(archive, Path(d) / 'output')

    def test_accepts_regular_nested_file(self):
        with tempfile.TemporaryDirectory() as d:
            archive = Path(d) / 'ok.zip'
            with zipfile.ZipFile(archive, 'w') as stream: stream.writestr('nested/file', b'expected')
            extract_zip(archive, Path(d) / 'output')
            self.assertEqual((Path(d) / 'output/nested/file').read_bytes(), b'expected')


class NativeEvidence(unittest.TestCase):
    def test_resource_compilation_is_separate_from_instrumented_cpp(self):
        rc = {'file': 'JuceLibraryCode/resources.rc', 'command': 'rc.exe /fo resources.res resources.rc'}
        cpp = {'file': 'Source/Plugin/PluginProcessor.cpp', 'command': 'cl.exe /EHsc /O2 /fsanitize=address /c PluginProcessor.cpp'}
        self.assertEqual(validate_compile_commands([rc, cpp], ['/fsanitize=address'], 'Release'), 1)
        with self.assertRaisesRegex(RuntimeError, 'Uninstrumented'):
            validate_compile_commands([rc, dict(cpp, command='cl.exe /EHsc /O2 /c PluginProcessor.cpp')], ['/fsanitize=address'], 'Release')
        with self.assertRaisesRegex(RuntimeError, 'optimization'):
            validate_compile_commands([rc, dict(cpp, command='cl.exe /EHsc /Od /c PluginProcessor.cpp')], [], 'Release')
        with self.assertRaisesRegex(RuntimeError, 'assertions'):
            validate_compile_commands([dict(cpp, command='cl.exe /Zi /Od /DNDEBUG')], [], 'Debug')

    def test_peer_control_requires_the_original_x11_failure(self):
        original = 'DELIBERATE_PREMATURE_PEER_TEARDOWN\nX Error of failed request: BadWindow\n'
        validate_peer_teardown_control(original, 1)
        for text, code in ((original, 0), ('unrelated process error', 1),
                           (original.replace('DELIBERATE_PREMATURE_PEER_TEARDOWN', ''), 1),
                           (original.replace('BadWindow', 'BadAtom'), 1)):
            with self.assertRaises(RuntimeError): validate_peer_teardown_control(text, code)

    def test_success_marker_does_not_hide_runtime_diagnostics(self):
        success = 'PASS: actual plugin two-instance audio/editor/state/reopen lifecycle\n'
        validate_host_log(success)
        for marker in ('FAIL:', 'JUCE Assertion failure', 'Leaked objects detected:',
                       'X Error of failed request:', 'AddressSanitizer:', 'ThreadSanitizer:'):
            with self.subTest(marker=marker), self.assertRaises(RuntimeError):
                validate_host_log(marker + '\n' + success)
        with self.assertRaises(RuntimeError): validate_host_log('PASS: actual plugin creates editor\n')

    def test_msvc_commands_retain_native_exception_unwinding(self):
        command = 'cl.exe /nologo /TP /O2 /DNDEBUG -std:c++17 -MD /W4 /WX /c TuningTable.cpp'
        entry = {'file': 'Source/Tuning/TuningTable.cpp', 'command': command}
        # Reproduce the real failed runner command before any expensive build.
        with self.assertRaisesRegex(RuntimeError, 'exception-unwinding'):
            validate_msvc_defaults([entry])
        validate_msvc_defaults([dict(entry, command=command + ' /EHsc')])
        with self.assertRaises(RuntimeError):
            validate_msvc_defaults([dict(entry, command=command + ' /EHsc-')])
        with self.assertRaises(RuntimeError): validate_msvc_defaults([])

    def test_ctest_requires_all_cases_without_skip_or_failure(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'ctest.xml'
            cases = '<testcase name="pass"/>' * 95
            path.write_text('<testsuite>' + cases + '</testsuite>'); validate_ctest(path, 95)
            for tag in ('failure', 'error', 'skipped'):
                path.write_text('<testsuite>' + cases.replace('<testcase name="pass"/>',
                    '<testcase><' + tag + '/></testcase>', 1) + '</testsuite>')
                with self.assertRaises(RuntimeError): validate_ctest(path, 95)
            path.write_text('<testsuite>' + cases + '</testsuite>')
            with self.assertRaises(RuntimeError): validate_ctest(path, 96)
            with self.assertRaises(RuntimeError): validate_ctest(path, 0)

    def test_native_architecture_is_required(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'plugin'
            data = bytearray(128); data[:6] = b'\x7fELF\x02\x01'; data[18:20] = (62).to_bytes(2, 'little')
            path.write_bytes(data); verify_binary(path, 'Linux')
            data[18:20] = (183).to_bytes(2, 'little'); path.write_bytes(data)
            with self.assertRaises(RuntimeError): verify_binary(path, 'Linux')
            data = bytearray(128); data[:2] = b'MZ'; data[60:64] = (80).to_bytes(4, 'little')
            data[80:84] = b'PE\0\0'; data[84:86] = b'\x64\x86'; path.write_bytes(data); verify_binary(path, 'Windows')
            data[84:86] = b'\x4c\x01'; path.write_bytes(data)
            with self.assertRaises(RuntimeError): verify_binary(path, 'Windows')

    def test_portable_ci_has_no_apple_helper_dependency(self):
        root = Path(__file__).resolve().parent
        for name in ('desktop.py', 'common.py', 'native_reference.py', 'project.cmake'):
            self.assertNotIn('scripts/macos', (root / name).read_text())


class WorkflowArguments(unittest.TestCase):
    def execute(self, command):
        # Execute the checked-in Bash command with harmless command stand-ins.
        # No native build, display server, network or files are touched.
        interpreter = shlex.quote(Path(sys.executable).as_posix())
        script = '''set -eu
python() { INTERPRETER -c 'import json,sys; print(json.dumps(sys.argv[1:]))' "$@"; }
xvfb-run() { test "$1" = -a; test "$2" = -s; shift 3; "$@"; }
bash() { test "$1" = scripts/ci/run-linux.sh; shift; "$@"; }
COMMAND
'''.replace('INTERPRETER', interpreter).replace('COMMAND', command)
        env = dict(os.environ, RUNNER_TEMP='/ci temp with spaces', COMPILER='clang', PROFILE='Release', MSYS_NO_PATHCONV='1')
        # CreateProcess searches System32 before PATH; bare "bash" may select
        # the WSL launcher instead of the Git Bash used by Actions.
        bash = str(Path(os.environ['ProgramFiles']) / 'Git/bin/bash.exe') if os.name == 'nt' else 'bash'
        return subprocess.run([bash, '-c', script], env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_checked_in_native_and_gate_arguments(self):
        root = Path(__file__).resolve().parents[2]
        for system in ('linux', 'windows'):
            lines = (root / '.github/workflows' / (system + '-qualification.yml')).read_text().splitlines()
            native, = [line.split('run: ', 1)[1] for line in lines if 'run: ' in line and 'desktop.py native' in line]
            index, = [i for i, line in enumerate(lines) if 'python scripts/ci/desktop.py gate' in line]
            gate = lines[index].strip() + ' ' + lines[index + 1].strip()
            self.assertNotIn('--output', gate)
            self.assertIn('> "$RUNNER_TEMP/' + system + '-gate.json"', gate)
            for command in (native, gate.split('>', 1)[0].strip()):
                with self.subTest(system=system, command=command):
                    result = self.execute(command)
                    self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                    args = json.loads(result.stdout)
                    if 'native' in args:
                        self.assertEqual(args[args.index('--root') + 1], '/ci temp with spaces/swara-native')
                        self.assertEqual(args[args.index('--profile') + 1], 'Release')
                    else:
                        self.assertNotIn('--output', args)
                        self.assertEqual(args[args.index('--evidence') + 1], '/ci temp with spaces/native-evidence')

    def test_original_placeholder_corruption_is_rejected(self):
        result = self.execute('python scripts/ci/desktop.py native --root "$ubuntu-24.04_TEMP/swara-native"')
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__': unittest.main()
