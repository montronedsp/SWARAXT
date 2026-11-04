# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Temporary focused execution of existing Windows preset filesystem tests."""
import argparse
import json
import os
from pathlib import Path
import platform
import sys
import xml.etree.ElementTree as ET

from common import SOURCE, JUCE_SHA
from desktop import Campaign, validate_compile_commands
import windows_current_reference


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--profile', choices=('Release', 'Debug', 'asan'), required=True)
    parser.add_argument('--mode', choices=('diagnose', 'verify'), required=True)
    args = parser.parse_args()
    args.compiler = 'msvc'
    if platform.system() != 'Windows':
        raise RuntimeError('Native Windows execution required')
    campaign = Campaign(args)
    campaign.report['purpose'] = 'FOCUSED_PRESET_FILES_ONLY_NOT_FULL_QUALIFICATION'
    try:
        if campaign.output(['git', 'rev-parse', 'HEAD'], SOURCE) != os.environ['GITHUB_SHA']:
            raise RuntimeError('Source identity mismatch')
        campaign.run('submodules', ['git', '-C', SOURCE, '-c', 'url.https://github.com/.insteadOf=git://github.com/', 'submodule', 'update', '--init', '--recursive'])
        if campaign.output(['git', 'status', '--porcelain'], SOURCE):
            raise RuntimeError('Dirty source checkout')
        juce = campaign.root / 'JUCE'
        campaign.run('clone-juce', ['git', '-c', 'core.autocrlf=false', 'clone', '--filter=blob:none', '--no-checkout', 'https://github.com/juce-framework/JUCE.git', juce])
        campaign.run('checkout-juce', ['git', '-C', juce, '-c', 'core.autocrlf=false', 'checkout', '--detach', JUCE_SHA])
        build = campaign.configure(juce)
        package = windows_current_reference.load_package(SOURCE)
        campaign.report['toolchain'] = windows_current_reference.toolchain(build, 'msvc-' + args.profile, package)
        entries = json.loads((build / 'compile_commands.json').read_text())
        validate_compile_commands(entries, campaign.flags, campaign.config)
        if args.profile == 'asan':
            campaign.sanitizer_controls()
        campaign.build(build, 'build-filesystem-target', ['SwaraXTControlIntegrityTests'])
        campaign.env['SWARA_PRESET_FILE_DIAGNOSTIC'] = '1'
        regex = '^SwaraXTControlIntegrity_files$' if args.mode == 'diagnose' else '^SwaraXTControlIntegrity_(files|files-write-failure|files-publish-failure)$'
        code = campaign.run('focused-ctest', ['ctest', '--test-dir', build, '-R', regex, '--no-tests=error', '--output-on-failure', '--verbose', '--output-junit', campaign.root / 'ctest.xml'], expected=None)
        cases = list(ET.parse(campaign.root / 'ctest.xml').getroot().iter('testcase'))
        campaign.report['focused_tests'] = [{'name': t.get('name'), 'failed': t.find('failure') is not None, 'skipped': t.find('skipped') is not None} for t in cases]
        if args.mode == 'diagnose':
            if len(cases) != 1 or 'PRESET_FILE_DIAGNOSTIC save_result' not in (campaign.root / 'focused-ctest.log').read_text():
                raise RuntimeError('Missing real symlink diagnostic')
            campaign.report['decision'] = 'DIAGNOSTIC_CAPTURED_NOT_QUALIFICATION'
        else:
            expected = {'SwaraXTControlIntegrity_files', 'SwaraXTControlIntegrity_files-write-failure', 'SwaraXTControlIntegrity_files-publish-failure'}
            if (code != 0 or {t.get('name') for t in cases} != expected
                    or 'PRESET_FILE_DIAGNOSTIC save_result' not in (campaign.root / 'focused-ctest.log').read_text()
                    or any(t.find('failure') is not None or t.find('error') is not None or t.find('skipped') is not None for t in cases)):
                raise RuntimeError('Focused filesystem tests failed')
            campaign.report['decision'] = 'FOCUSED_FILESYSTEM_PASS_NOT_FULL_QUALIFICATION'
    except BaseException as error:
        campaign.report.update(decision='FAIL', failure=str(error))
        raise
    finally:
        campaign.save()


if __name__ == '__main__':
    main()
