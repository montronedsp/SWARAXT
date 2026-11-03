# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail before native qualification if the hosted compiler identity differs."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

from common import SOURCE, juce_revision, msvc_environment, sha
import windows_current_reference


def require_compiler(actual, expected):
    if actual != expected:
        raise RuntimeError('Hosted MSVC differs from the qualified compiler; owner review required')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    root = parser.parse_args().root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = {'decision': 'INCOMPLETE', 'source_sha': os.environ.get('GITHUB_SHA'),
              'image': {k: os.environ.get(k) for k in
                        ('ImageOS', 'ImageVersion', 'RUNNER_OS', 'RUNNER_ARCH')},
              'native_qualification': 'NOT_RUN'}
    try:
        if platform.system() != 'Windows':
            raise RuntimeError('Actual Windows runner required')
        actual_source = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
        if actual_source != report['source_sha']:
            raise RuntimeError('Source revision mismatch')
        package = windows_current_reference.load_package(SOURCE)
        env = msvc_environment(dict(os.environ), root)
        compiler = shutil.which('cl.exe', path=env['PATH'])
        if not compiler:
            raise RuntimeError('Selected cl.exe not found')
        report.update(compiler_path=compiler, compiler_sha256=sha(compiler),
                      expected_compiler_sha256=package['compiler_sha256'])
        result = subprocess.run([compiler, '/Bv'], env=env, capture_output=True, text=True)
        (root / 'cl-Bv.log').write_text(result.stdout + result.stderr)
        report['cl_Bv_exit_code'] = result.returncode  # cl without an input can exit nonzero.
        juce = root / 'JUCE'
        with (root / 'juce-source.log').open('w') as log:
            subprocess.run(['git', '-c', 'core.autocrlf=false', 'clone', '--filter=blob:none',
                            '--no-checkout', 'https://github.com/juce-framework/JUCE.git', str(juce)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run(['git', '-C', str(juce), '-c', 'core.autocrlf=false', 'checkout',
                            '--detach', juce_revision('Windows')], stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        report['juce_sha'] = subprocess.check_output(['git', '-C', str(juce), 'rev-parse', 'HEAD'], text=True).strip()
        dirty = subprocess.check_output(['git', '-C', str(juce), 'status', '--porcelain',
                                         '--untracked-files=all', '--ignored'], text=True)
        if report['juce_sha'] != package['reference_juce_sha'] or dirty:
            raise RuntimeError('JUCE source identity mismatch')
        require_compiler(report['compiler_sha256'], package['compiler_sha256'])
        report['decision'] = 'TOOLCHAIN_IDENTITY_PASS'
    except BaseException as error:
        report.update(decision='FAIL', failure=str(error))
        raise
    finally:
        (root / 'identity.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
