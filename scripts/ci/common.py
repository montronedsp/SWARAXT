# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared Linux/Windows qualification primitives."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time
import urllib.request
import zipfile

SOURCE = Path(__file__).resolve().parents[2]
JUCE_SHA = 'e18f7f506c0b96f2c738a0bcd7fe6467a5005ad8'
SDK_SHA = '9fad9770f2ae8542ab1a548a68c1ad1ac690abe0'
PLUGINVAL = {
    'Linux': ('pluginval_Linux.zip', 'c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352'),
    'Windows': ('pluginval_Windows.zip', 'c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab'),
}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download(url, destination):
    with urllib.request.urlopen(url, timeout=120) as source, Path(destination).open('xb') as output:
        while chunk := source.read(1024 * 1024):
            output.write(chunk)


def architecture():
    machine = platform.machine().lower()
    return {'amd64': 'x86_64', 'x64': 'x86_64'}.get(machine, machine)


def command_argv(argv, env, system=None):
    argv = list(map(str, argv))
    if (system or platform.system()) == 'Windows' and not Path(argv[0]).is_absolute():
        # CreateProcess does not use the supplied child's PATH to locate its
        # executable. Resolve explicitly against the imported compiler PATH.
        executable = shutil.which(argv[0], path=env.get('PATH'))
        if executable is None:
            raise RuntimeError('Executable missing from native toolchain PATH: ' + argv[0])
        argv[0] = executable
    return argv


def extract_zip(archive, destination):
    destination = Path(destination).resolve()
    with zipfile.ZipFile(archive) as stream:
        for entry in stream.infolist():
            target = destination / entry.filename
            if not target.resolve().is_relative_to(destination) or '\\' in entry.filename:
                raise RuntimeError('Unsafe archive member')
            if (entry.external_attr >> 16) & 0o170000 == 0o120000:
                raise RuntimeError('Archive symlinks are not accepted')
        stream.extractall(destination)


def msvc_environment(env, owned_root):
    """Import only compiler environment fields; never print the environment."""
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([vswhere, '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    script = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not script.is_file():
        raise RuntimeError('Native x64 MSVC environment not found')
    wrapper = Path(owned_root) / 'activate-msvc.cmd'
    wrapper.write_text('@echo off\ncall "' + str(script) + '" >nul\nif errorlevel 1 exit /b 1\nset\n')
    # A fixed wrapper basename avoids cmd.exe quoting of user-supplied paths.
    output = subprocess.check_output(['cmd.exe', '/d', '/c', wrapper.name],
                                     cwd=owned_root, env=env, text=True)
    allowed = {'path', 'include', 'lib', 'libpath', 'vctoolsinstalldir', 'vsinstalldir',
               'windowssdkdir', 'windowssdkversion', 'ucrtversion', 'universalcrtsdkdir'}
    result = dict(env)
    for line in output.splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            if key.lower() in allowed:
                result[key.upper() if key.lower() in ('path', 'include', 'lib', 'libpath') else key] = value
    return result


class Evidence:
    def __init__(self, root, profile, compiler):
        self.root = Path(root).resolve()
        if self.root.is_relative_to(SOURCE) or SOURCE.is_relative_to(self.root):
            raise RuntimeError('Evidence must be separate from the source checkout')
        self.root.mkdir(parents=True, exist_ok=False)
        self.env = dict(os.environ)
        # Keep pip-pinned frontends even if vcvars prepends bundled VS tools.
        self.frontends = {tool: shutil.which(tool, path=self.env.get('PATH')) for tool in ('cmake', 'ninja')}
        if not all(self.frontends.values()):
            raise RuntimeError('Pinned CMake and Ninja must be installed first')
        if platform.system() == 'Windows':
            self.env = msvc_environment(self.env, self.root)
        self.report = {'decision': 'INCOMPLETE', 'source_sha': os.environ['GITHUB_SHA'],
                       'run_id': os.environ['GITHUB_RUN_ID'], 'run_attempt': os.environ.get('GITHUB_RUN_ATTEMPT'),
                       'platform': platform.system(), 'architecture': architecture(),
                       'build_frontends': self.frontends,
                       'profile': profile, 'compiler': compiler, 'juce_sha': JUCE_SHA,
                       'standalone': False, 'formats': ['VST3'], 'commands': [],
                       'visible_gui_acceptance': 'NOT_PROVEN_BY_EDITOR_LIFECYCLE_TESTS',
                       'installation': 'NOT_RUN', 'distribution': 'UNSIGNED_CANDIDATE_ONLY'}
        self.save()

    def save(self):
        (self.root / 'report.json').write_text(json.dumps(self.report, indent=2) + '\n')

    def run(self, name, argv, timeout=1800, expected=0, cwd=None):
        argv = self.argv(argv)
        print('START', name, flush=True)
        start = time.monotonic()
        with (self.root / (name + '.log')).open('x', encoding='utf-8') as log:
            try:
                result = subprocess.run(argv, cwd=cwd or self.root, env=self.env,
                    stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
        self.report['commands'].append({'name': name, 'argv': argv, 'exit_code': code,
                                       'seconds': round(time.monotonic() - start, 2)})
        self.save()
        print('END', name, code, flush=True)
        if expected is not None and code != expected:
            print((self.root / (name + '.log')).read_text(errors='replace')[-12000:], flush=True)
            raise RuntimeError(name + ' failed; original log retained')
        return code

    def output(self, argv, cwd=None):
        return subprocess.check_output(self.argv(argv), cwd=cwd or self.root,
                                       env=self.env, text=True).rstrip('\r\n')

    def argv(self, argv):
        argv = list(map(str, argv))
        if argv[0] in self.frontends:
            argv[0] = self.frontends[argv[0]]
        return command_argv(argv, self.env)

    def binary(self, build, name):
        suffix = '.exe' if platform.system() == 'Windows' else ''
        paths = [p for p in Path(build).rglob(name + suffix) if p.is_file()]
        if len(paths) != 1:
            raise RuntimeError('Expected one executable for ' + name + ', found ' + str(len(paths)))
        return paths[0]
