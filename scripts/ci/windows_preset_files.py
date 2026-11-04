# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Temporary focused execution of existing Windows preset filesystem tests."""
import argparse
import base64
import hashlib
import subprocess
import json
import os
from pathlib import Path
import platform
import sys
import xml.etree.ElementTree as ET

from common import SOURCE, JUCE_SHA
from desktop import Campaign, validate_compile_commands
import windows_current_reference


PROPOSED_PATCH = 'LS0tIGEvU291cmNlL1BsdWdpbi9QbHVnaW5Qcm9jZXNzb3IuY3BwCisrKyBiL1NvdXJjZS9QbHVnaW4vUGx1Z2luUHJvY2Vzc29yLmNwcApAQCAtMTQsNiArMTQsMTMgQEAKICNpbmNsdWRlIDxhbGdvcml0aG0+CiAjaW5jbHVkZSA8Y3N0cmluZz4KICNpbmNsdWRlIDxmaWxlc3lzdGVtPgorCisjaWYgSlVDRV9XSU5ET1dTCisjaWZuZGVmIE5PTUlOTUFYCisjZGVmaW5lIE5PTUlOTUFYCisjZW5kaWYKKyNpbmNsdWRlIDx3aW5kb3dzLmg+CisjZW5kaWYKIAogYm9vbCBTd2FyYVh0QXVkaW9Qcm9jZXNzb3I6OmltcG9ydFR1bmluZ0ZpbGUoY29uc3QganVjZTo6RmlsZSYgZmlsZSwgY29uc3QganVjZTo6RmlsZSYga2V5Ym9hcmQsCiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGp1Y2U6OlN0cmluZyYgZXJyb3IpCkBAIC03NzAsNyArNzc3LDIyIEBACiAjZW5kaWYKICAgICBpZiAob3ZlcndyaXRlKQogICAgIHsKKyNpZiBKVUNFX1dJTkRPV1MKKyAgICAgICAgLy8gUmVwbGFjZUZpbGVXIHJlamVjdHMgc3ltbGlua3MuIFJlbmFtZSB0aGUgY29tcGxldGUgc2FtZS1kaXJlY3RvcnkKKyAgICAgICAgLy8gdGVtcG9yYXJ5IG92ZXIgdGhlIGVudHJ5IGl0c2VsZiwgd2l0aG91dCBhIGNvcHkvZGVsZXRlIGZhbGxiYWNrLgorICAgICAgICBib29sIHB1Ymxpc2hlZCA9IGZhbHNlOworICAgICAgICBmb3IgKGludCBhdHRlbXB0ID0gMDsgYXR0ZW1wdCA8IDUgJiYgISBwdWJsaXNoZWQ7ICsrYXR0ZW1wdCkKKyAgICAgICAgeworICAgICAgICAgICAgcHVibGlzaGVkID0gTW92ZUZpbGVFeFcodGVtcG9yYXJ5LmdldEZpbGUoKS5nZXRGdWxsUGF0aE5hbWUoKS50b1dpZGVDaGFyUG9pbnRlcigpLAorICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgdGFyZ2V0LmdldEZ1bGxQYXRoTmFtZSgpLnRvV2lkZUNoYXJQb2ludGVyKCksCisgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBNT1ZFRklMRV9SRVBMQUNFX0VYSVNUSU5HIHwgTU9WRUZJTEVfV1JJVEVfVEhST1VHSCkgIT0gRkFMU0U7CisgICAgICAgICAgICBpZiAoISBwdWJsaXNoZWQpCisgICAgICAgICAgICAgICAganVjZTo6VGhyZWFkOjpzbGVlcCgxMDApOworICAgICAgICB9CisgICAgICAgIGlmICghIHB1Ymxpc2hlZCkKKyNlbHNlCiAgICAgICAgIGlmICghIHRlbXBvcmFyeS5vdmVyd3JpdGVUYXJnZXRGaWxlV2l0aFRlbXBvcmFyeSgpKQorI2VuZGlmCiAgICAgICAgIHsKICAgICAgICAgICAgIGVycm9yID0gIkNvdWxkIG5vdCB3cml0ZSB0aGUgcHJlc2V0IHNhZmVseS4iOwogICAgICAgICAgICAgcmV0dXJuIGZhbHNlOwotLS0gYS9UZXN0cy9SZWdyZXNzaW9uL0NvbnRyb2xJbnRlZ3JpdHlUZXN0c01haW4uY3BwCisrKyBiL1Rlc3RzL1JlZ3Jlc3Npb24vQ29udHJvbEludGVncml0eVRlc3RzTWFpbi5jcHAKQEAgLTE3LDcgKzE3LDYgQEAKICNpbmNsdWRlIDxjc3RkaW8+CiAjaW5jbHVkZSA8dGhyZWFkPgogI2luY2x1ZGUgPG1hcD4KLSNpbmNsdWRlICIuLi9TdXBwb3J0L1ByZXNldEZpbGVEaWFnbm9zdGljLmgiCiAKIG5hbWVzcGFjZSBzd2FyYXh0IHsKIHN0cnVjdCBTZXF1ZW5jZXJNaWRpVGVzdEFjY2VzcyB7CkBAIC0yOTAsMzMgKzI4OSwxOSBAQAogICAgIGNoZWNrKHAuc2F2ZVVzZXJQcmVzZXQoIlZhbGlkLm5hbWUiLGZhbHNlLGVycm9yKSwib3JkaW5hcnkgZW1iZWRkZWQgZG90cyByZW1haW4gdmFsaWQiKTsKICAgICBjb25zdCBhdXRvIG91dHNpZGU9ZGlyLmdldFNpYmxpbmdGaWxlKGRpci5nZXRGaWxlTmFtZSgpKyItb3V0c2lkZSIpOwogICAgIGNoZWNrKG91dHNpZGUucmVwbGFjZVdpdGhUZXh0KCJvdXRzaWRlIHNlbnRpbmVsIiksImlzb2xhdGVkIHN5bWxpbmsgdGFyZ2V0IGZpeHR1cmUiKTsKLSAgICBpZihvdXRzaWRlLmNyZWF0ZVN5bWJvbGljTGluayhkaXIuZ2V0Q2hpbGRGaWxlKCJMaW5rLnN3YXJheHRwcmVzZXQiKSxmYWxzZSkpeworICAgIGNvbnN0IGF1dG8gbGluaz1kaXIuZ2V0Q2hpbGRGaWxlKCJMaW5rLnN3YXJheHRwcmVzZXQiKTsKKyAgICBjb25zdCBib29sIGxpbmtDcmVhdGVkPW91dHNpZGUuY3JlYXRlU3ltYm9saWNMaW5rKGxpbmssZmFsc2UpOworI2lmIEpVQ0VfV0lORE9XUworICAgIGNoZWNrKGxpbmtDcmVhdGVkLCJXaW5kb3dzIHN5bWJvbGljLWxpbmsgZml4dHVyZSBtdXN0IGV4ZWN1dGUiKTsKKyNlbmRpZgorICAgIGlmKGxpbmtDcmVhdGVkKXsKICAgICAgICAgY2hlY2soIXAuc2F2ZVVzZXJQcmVzZXQoIkxpbmsiLGZhbHNlLGVycm9yKSYmb3V0c2lkZS5sb2FkRmlsZUFzU3RyaW5nKCk9PSJvdXRzaWRlIHNlbnRpbmVsIiwiY3JlYXRlLW9ubHkgY2Fubm90IGZvbGxvdyBvciBvdmVyd3JpdGUgYSBzeW1saW5rIik7Ci0jaWYgSlVDRV9XSU5ET1dTICYmICFkZWZpbmVkKFNXQVJBX0NPTlRST0xfQkFTRUxJTkUpCi0gICAgICAgIHByZXNldEZpbGVEaWFnbm9zdGljOjpDYXB0dXJlIGNhcHR1cmV7ZGlyLCB7fX07Ci0gICAgICAgIGlmKHByZXNldEZpbGVEaWFnbm9zdGljOjplbmFibGVkKCkpIHsKLSAgICAgICAgICAgIHByZXNldEZpbGVEaWFnbm9zdGljOjplbWl0KCJsaW5rX2JlZm9yZSIscHJlc2V0RmlsZURpYWdub3N0aWM6OmZpbGUoZGlyLmdldENoaWxkRmlsZSgiTGluay5zd2FyYXh0cHJlc2V0IikpKTsKLSAgICAgICAgICAgIHByZXNldEZpbGVEaWFnbm9zdGljOjplbWl0KCJvdXRzaWRlX2JlZm9yZSIscHJlc2V0RmlsZURpYWdub3N0aWM6OmZpbGUob3V0c2lkZSkpOwotICAgICAgICAgICAgcC5zZXRTdGF0ZVB1YmxpY2F0aW9uSG9vayhwcmVzZXRGaWxlRGlhZ25vc3RpYzo6Q2FwdHVyZTo6aG9vaywmY2FwdHVyZSk7Ci0gICAgICAgIH0KLSNlbmRpZgogICAgICAgICBjb25zdCBib29sIGxpbmtTYXZlZD1wLnNhdmVVc2VyUHJlc2V0KCJMaW5rIix0cnVlLGVycm9yKTsKLSNpZiBKVUNFX1dJTkRPV1MgJiYgIWRlZmluZWQoU1dBUkFfQ09OVFJPTF9CQVNFTElORSkKLSAgICAgICAgaWYocHJlc2V0RmlsZURpYWdub3N0aWM6OmVuYWJsZWQoKSkgewotICAgICAgICAgICAgY29uc3QgYXV0byBsYXN0RXJyb3I9R2V0TGFzdEVycm9yKCk7Ci0gICAgICAgICAgICBwLnNldFN0YXRlUHVibGljYXRpb25Ib29rKG51bGxwdHIsbnVsbHB0cik7Ci0gICAgICAgICAgICBhdXRvKiByZXN1bHQ9bmV3IGp1Y2U6OkR5bmFtaWNPYmplY3Q7Ci0gICAgICAgICAgICByZXN1bHQtPnNldFByb3BlcnR5KCJzYXZlZCIsbGlua1NhdmVkKTtyZXN1bHQtPnNldFByb3BlcnR5KCJlcnJvciIsZXJyb3IpO3Jlc3VsdC0+c2V0UHJvcGVydHkoInBvc3Rfc2F2ZV9sYXN0X2Vycm9yIixzdGF0aWNfY2FzdDxpbnQ+KGxhc3RFcnJvcikpOwotICAgICAgICAgICAgcHJlc2V0RmlsZURpYWdub3N0aWM6OmVtaXQoInNhdmVfcmVzdWx0IixqdWNlOjp2YXIocmVzdWx0KSk7Ci0gICAgICAgICAgICBwcmVzZXRGaWxlRGlhZ25vc3RpYzo6ZW1pdCgibGlua19hZnRlciIscHJlc2V0RmlsZURpYWdub3N0aWM6OmZpbGUoZGlyLmdldENoaWxkRmlsZSgiTGluay5zd2FyYXh0cHJlc2V0IikpKTsKLSAgICAgICAgICAgIHByZXNldEZpbGVEaWFnbm9zdGljOjplbWl0KCJvdXRzaWRlX2FmdGVyIixwcmVzZXRGaWxlRGlhZ25vc3RpYzo6ZmlsZShvdXRzaWRlKSk7Ci0gICAgICAgICAgICBmb3IoY29uc3QgYXV0byYgdGVtcG9yYXJ5OmNhcHR1cmUudGVtcG9yYXJpZXMpIHByZXNldEZpbGVEaWFnbm9zdGljOjplbWl0KCJ0ZW1wb3JhcnlfYWZ0ZXJfc2F2ZSIscHJlc2V0RmlsZURpYWdub3N0aWM6OmZpbGUodGVtcG9yYXJ5KSk7Ci0gICAgICAgICAgICBjb25zdCBhdXRvIHByb2JlRGlyZWN0b3J5PWRpci5nZXRDaGlsZEZpbGUoIndpbjMyLXJlcGxhY2VtZW50LXByb2JlIik7Ci0gICAgICAgICAgICBjaGVjayhwcm9iZURpcmVjdG9yeS5jcmVhdGVEaXJlY3RvcnkoKS53YXNPaygpLCJvd25lZCBuYXRpdmUgZGlhZ25vc3RpYyBkaXJlY3RvcnkiKTsKLSAgICAgICAgICAgIGZvcihjb25zdCBhdXRvKiBvcGVyYXRpb246eyJSZXBsYWNlRmlsZVciLCJNb3ZlRmlsZUV4VyIsInN0ZF9maWxlc3lzdGVtX3JlbmFtZSJ9KSBwcmVzZXRGaWxlRGlhZ25vc3RpYzo6cHJvYmUocHJvYmVEaXJlY3Rvcnksb3BlcmF0aW9uKTsKLSAgICAgICAgfQotI2VuZGlmCi0gICAgICAgIGNoZWNrKGxpbmtTYXZlZCYmb3V0c2lkZS5sb2FkRmlsZUFzU3RyaW5nKCk9PSJvdXRzaWRlIHNlbnRpbmVsIiwib3ZlcndyaXRlIHJlcGxhY2VzIGxpbmsgZW50cnkgYW5kIHByZXNlcnZlcyBvdXRzaWRlIHRhcmdldCIpOworICAgICAgICBjb25zdCBib29sIG91dHNpZGVQcmVzZXJ2ZWQ9b3V0c2lkZS5sb2FkRmlsZUFzU3RyaW5nKCk9PSJvdXRzaWRlIHNlbnRpbmVsIjsKKyAgICAgICAgaWYoIWxpbmtTYXZlZHx8IW91dHNpZGVQcmVzZXJ2ZWQpc3RkOjpwcmludGYoIlN5bWxpbmsgb3ZlcndyaXRlOiBzYXZlZD0lZCBvdXRzaWRlUHJlc2VydmVkPSVkIGVycm9yPSVzXG4iLGludChsaW5rU2F2ZWQpLGludChvdXRzaWRlUHJlc2VydmVkKSxlcnJvci50b1Jhd1VURjgoKSk7CisgICAgICAgIGNoZWNrKGxpbmtTYXZlZCYmb3V0c2lkZVByZXNlcnZlZCwib3ZlcndyaXRlIHJlcGxhY2VzIGxpbmsgZW50cnkgYW5kIHByZXNlcnZlcyBvdXRzaWRlIHRhcmdldCIpOworICAgICAgICBjaGVjayghbGluay5pc1N5bWJvbGljTGluaygpLCJwdWJsaXNoZWQgcHJlc2V0IHJlcGxhY2VzIHRoZSBzeW1ib2xpYy1saW5rIGVudHJ5Iik7CisgICAgICAgIFAgbGlua1JlYWRlcjtjaGVjayhsaW5rUmVhZGVyLmxvYWRQcmVzZXRFbnRyeSh7IkxpbmsiLGxpbmssLTEsZmFsc2V9LGVycm9yKSwicmVwbGFjZWQgbGluayBjb250YWlucyBhIGNvbXBsZXRlIGxvYWRhYmxlIHByZXNldCIpOwogICAgIH1lbHNlIHN0ZDo6cHV0cygiTk9UX1JVTjogc3ltbGluayBmaXh0dXJlIHVuYXZhaWxhYmxlIG9uIHRoaXMgZmlsZXN5c3RlbS9hY2NvdW50Iik7CiAgICAgY2hlY2sob3V0c2lkZS5kZWxldGVGaWxlKCksInJlbW92ZSBvd24gb3V0c2lkZSB0YXJnZXQgZml4dHVyZSIpOwogICAgIHNldChwLHN3YXJheHQ6OklEczo6b3NjMVBhcmFtLDE3KTtjaGVjayhwLnNhdmVVc2VyUHJlc2V0KCJLZWVwIixmYWxzZSxlcnJvciksImZyZXNoIHNhdmUiKTtjb25zdCBhdXRvIHRhcmdldD1kaXIuZ2V0Q2hpbGRGaWxlKCJLZWVwLnN3YXJheHRwcmVzZXQiKTtjb25zdCBhdXRvIGZpcnN0PXRhcmdldC5sb2FkRmlsZUFzU3RyaW5nKCk7Cg=='
PROPOSED_FILES = {'Source/Plugin/PluginProcessor.cpp': {'before_sha256': '28e7afdbf2b74fdd0f5d5d41e7da4de3dfbe03eaae3b5f5382beea19278075c6', 'after_sha256': '23e7485268c10062cbedf613c4aefeebb3a82ab94850015ad58c90409a39c318'}, 'Tests/Regression/ControlIntegrityTestsMain.cpp': {'before_sha256': '20289c1fef54e6207e2e8f3052c73740fd5e2e319225f4cce4247a9158ac9ff4', 'after_sha256': '1ef2d3b851f8847106a5f28c8e82dfd85f72978908968090ee741614cdb5d049'}}

def apply_proposed_fix(campaign):
    for name, expected in PROPOSED_FILES.items():
        if hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() != expected['before_sha256']:
            raise RuntimeError('Proposed patch source mismatch: ' + name)
    patch = campaign.root / 'proposed-fix.patch'
    patch.write_bytes(base64.b64decode(PROPOSED_PATCH, validate=True))
    campaign.run('check-proposed-fix', ['git', '-C', SOURCE, 'apply', '--check', patch])
    campaign.run('apply-proposed-fix', ['git', '-C', SOURCE, 'apply', patch])
    for name, expected in PROPOSED_FILES.items():
        if hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() != expected['after_sha256']:
            raise RuntimeError('Proposed patch result mismatch: ' + name)
    campaign.report['explicit_uncommitted_proposal'] = PROPOSED_FILES
    (campaign.root / 'actual-source.patch').write_bytes(subprocess.check_output(['git', '-C', str(SOURCE), 'diff', '--', *PROPOSED_FILES]))


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
        if args.mode == 'verify':
            apply_proposed_fix(campaign)
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
                    or 'NOT_RUN: symlink fixture' in (campaign.root / 'focused-ctest.log').read_text()
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
