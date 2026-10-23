#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent Linux/Windows native qualification and unsigned VST3 evidence."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import sys
import xml.etree.ElementTree as ET

from common import Evidence, JUCE_SHA, SDK_SHA, SOURCE, architecture, sha
from native_reference import BASELINE_SHA, TESTS, validate_output
from reference_metadata import apply as apply_reference_metadata
from linux_dependency import apply as apply_linux_dependency
from boolean_dependency import apply as apply_boolean_dependency
from pluginval_source import (apply as apply_pluginval_source, SOURCE_SHA as PLUGINVAL_SHA,
                              VENDOR_SHA as PLUGINVAL_JUCE_SHA, FIX_SHA as PLUGINVAL_FIX_SHA,
                              BEFORE_SHA256 as PLUGINVAL_BEFORE, AFTER_SHA256 as PLUGINVAL_AFTER,
                              PORTABILITY_FILES as PLUGINVAL_PORTABILITY,
                              git_argv as pluginval_git)

CI = SOURCE / 'scripts/ci'
LINUX_PROFILES = [('gcc', 'Release'), ('gcc', 'Debug'), ('clang', 'Release'),
                  ('clang', 'Debug'), ('clang', 'asan-ubsan'), ('clang', 'tsan'),
                  ('clang', 'rtsan'), ('clang', 'fuzz')]
WINDOWS_PROFILES = [('msvc', 'Release'), ('msvc', 'Debug'), ('msvc', 'asan')]


def profile_flags(system, compiler, profile):
    allowed = LINUX_PROFILES if system == 'Linux' else WINDOWS_PROFILES if system == 'Windows' else []
    if (compiler, profile) not in allowed:
        raise RuntimeError('Unsupported native compiler/profile combination')
    if profile == 'asan':
        return ['/fsanitize=address', '/Zi']
    if profile in ('asan-ubsan', 'fuzz'):
        return ['-fsanitize=address,undefined,vptr', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-gline-tables-only']
    if profile == 'tsan':
        return ['-fsanitize=thread', '-fno-omit-frame-pointer', '-gline-tables-only']
    if profile == 'rtsan':
        return ['-fsanitize=realtime', '-fno-omit-frame-pointer', '-gline-tables-only']
    return []


def validate_ctest(path, expected):
    cases = list(ET.parse(path).getroot().iter('testcase'))
    if len(cases) != expected or expected < 90:
        raise RuntimeError('Missing native regression cases')
    if any(list(case.iter(tag)) for case in cases for tag in ('failure', 'error', 'skipped')):
        raise RuntimeError('Failed, errored or skipped native regression')


def verify_binary(path, system):
    data = path.read_bytes()
    if system == 'Linux':
        if data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != 62:
            raise RuntimeError('Expected native x86_64 ELF plugin')
    else:
        offset = int.from_bytes(data[60:64], 'little')
        if data[:2] != b'MZ' or data[offset:offset + 4] != b'PE\0\0' or data[offset + 4:offset + 6] != b'\x64\x86':
            raise RuntimeError('Expected native x86_64 PE plugin')


def validate_msvc_defaults(entries):
    cpp = [entry for entry in entries if Path(entry['file']).suffix.lower() in ('.cpp', '.cc', '.cxx')]
    if not cpp or any(not re.search(r'(?:^|\s)/EHsc(?:\s|$)', entry['command']) for entry in cpp):
        raise RuntimeError('MSVC C++ exception-unwinding defaults are missing')


def validate_host_log(text):
    if 'PASS: actual plugin two-instance audio/editor/state/reopen lifecycle' not in text:
        raise RuntimeError('Actual plugin lifecycle did not complete')
    if any(marker in text for marker in ('FAIL:', 'JUCE Assertion failure', 'Leaked objects detected:',
                                         'X Error of failed request:', 'Sanitizer:')):
        raise RuntimeError('Actual plugin host reported a runtime diagnostic')


def validate_peer_teardown_control(text, code):
    if (code == 0 or 'DELIBERATE_PREMATURE_PEER_TEARDOWN' not in text
            or 'X Error of failed request:' not in text
            or not any(marker in text for marker in ('BadWindow', 'BadDrawable'))):
        raise RuntimeError('Original X11 peer teardown failure was not reproduced')


def validate_compile_commands(entries, flags, config):
    resources = [entry for entry in entries if Path(entry['file']).suffix.lower() == '.rc']
    for entry in entries:
        if entry in resources:
            continue  # RC resources are not C/C++ translation units.
        command = entry['command']
        if flags and flags[0] not in command:
            raise RuntimeError('Uninstrumented translation unit: ' + entry['file'])
        if config == 'Debug' and re.search(r'(?:-|/)D(?:NDEBUG)(?:\b|=)', command):
            raise RuntimeError('Debug compilation unexpectedly disables assertions')
        if config == 'Release' and not re.search(r'(?:-|/)O[123s]', command):
            raise RuntimeError('Release compilation lacks optimization')
    return len(resources)


class Campaign(Evidence):
    def __init__(self, args):
        self.args = args
        self.system = platform.system()
        self.flags = profile_flags(self.system, args.compiler, args.profile)
        self.config = 'Debug' if args.profile == 'Debug' else 'Release'
        super().__init__(args.root, args.profile, args.compiler)
        self.env['SWARAXT_NATIVE_PROFILE'] = args.compiler + '-' + args.profile
        if args.profile in ('asan-ubsan', 'fuzz', 'asan'):
            self.env.update(ASAN_OPTIONS='halt_on_error=1:abort_on_error=1:detect_leaks=' +
                            ('1' if self.system == 'Linux' else '0'),
                            UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        elif args.profile == 'tsan':
            self.env['TSAN_OPTIONS'] = 'halt_on_error=1:abort_on_error=1'
        elif args.profile == 'rtsan':
            self.env['RTSAN_OPTIONS'] = 'halt_on_error=1:abort_on_error=1'
        if self.system == 'Linux' and self.flags:
            for key in ('ASAN_OPTIONS', 'UBSAN_OPTIONS', 'TSAN_OPTIONS', 'RTSAN_OPTIONS'):
                if key in self.env:
                    self.env[key] += ':external_symbolizer_path=/usr/bin/llvm-symbolizer-21'
            self.env['ASAN_SYMBOLIZER_PATH'] = '/usr/bin/llvm-symbolizer-21'
        if self.system == 'Windows':
            self.report['unsupported_sanitizers'] = ['UBSan', 'TSan', 'RTSan', 'LSan']
            self.report['debug_runtime_checks'] = 'RTC1 omitted because this suite includes native MSVC ASan executables'

    def compilers(self):
        return ('cl', 'cl') if self.system == 'Windows' else (
            ('gcc', 'g++') if self.args.compiler == 'gcc' else ('clang-21', 'clang++-21'))

    def prepare(self):
        if self.system not in ('Linux', 'Windows') or architecture() != 'x86_64':
            raise RuntimeError('Native Linux/Windows x86_64 runner required')
        if self.output(['git', 'rev-parse', 'HEAD'], SOURCE) != self.report['source_sha']:
            raise RuntimeError('Checkout differs from reviewed revision')
        self.run('harness-tests', [sys.executable, '-m', 'unittest', 'test_common', 'test_native_reference',
                                  'test_sharing', 'test_test_scopes', 'test_sanitizer_policy', 'test_linux_dependency',
                                  'test_pluginval_source', 'test_capacity_diagnostic', 'test_boolean_dependency'], cwd=CI)
        self.run('submodules', ['git', '-C', SOURCE, '-c', 'url.https://github.com/.insteadOf=git://github.com/',
                               'submodule', 'update', '--init', '--recursive'])
        submodules = self.output(['git', 'submodule', 'status', '--recursive'], SOURCE)
        if any(not line.startswith(' ') for line in submodules.splitlines()):
            raise RuntimeError('Submodule revision mismatch')
        self.report['submodules'] = submodules.splitlines()
        if self.output(['git', 'status', '--porcelain'], SOURCE):
            raise RuntimeError('Source checkout must be clean')
        self.run('platform', [sys.executable, '-c', 'import platform; print(platform.platform()); print(platform.machine())'])
        self.run('cmake-version', ['cmake', '--version'])
        if self.system == 'Linux':
            self.run('compiler-version', [self.compilers()[1], '--version'])
            self.run('kernel', ['uname', '-a'])
            self.run('address-space-layout', ['sudo', '-n', 'sysctl', 'vm.mmap_rnd_bits'])
            self.run('window-manager', ['xprop', '-root', '_NET_SUPPORTING_WM_CHECK'])
            wm_log = Path(os.environ['RUNNER_TEMP']) / 'swara-wm.log'
            if wm_log.is_file(): shutil.copyfile(wm_log, self.root / 'display-wm.log')
        else:
            probe = self.root / 'compiler-probe.cpp'
            probe.write_text('#include "NativeStdio.h"\n#include <chrono>\nusing namespace std::literals;\n'
                             'static_assert(100ms == std::chrono::milliseconds(100));\n'
                             'static_assert(sizeof(void*)==8,"native x64 required");\n'
                             'int main(){if(!prepareNativeOutput())return 1;std::puts("STDIO_PREFLIGHT_PASS");}\n')
            executable = self.root / 'compiler-probe.exe'
            self.run('compiler-version', ['cl', '/nologo', '/Bv', '/EHsc', '/std:c++17', '/I' + str(CI), probe,
                                          '/Fe:' + str(executable),
                                          '/Fo:' + str(self.root / 'compiler-probe.obj')])
            self.run('stdio-preflight', [executable], timeout=30)
        juce = self.root / 'JUCE'
        self.run('clone-juce', ['git', '-c', 'core.autocrlf=false', 'clone', '--filter=blob:none', '--no-checkout',
                              'https://github.com/juce-framework/JUCE.git', juce])
        self.run('checkout-juce', ['git', '-C', juce, '-c', 'core.autocrlf=false', 'checkout', '--detach', JUCE_SHA])
        if self.output(['git', '-C', juce, 'rev-parse', 'HEAD']) != JUCE_SHA:
            raise RuntimeError('JUCE revision mismatch')
        self.report['dependency_overlay'] = [apply_boolean_dependency(juce, self.root)]
        if self.system == 'Linux':
            self.report['dependency_overlay'] += apply_linux_dependency(juce, self.root)
            probe = self.root / 'dependency-headers.cpp'
            probe.write_text('#include <juce_audio_utils/juce_audio_utils.h>\n')
            self.run('dependency-header-preflight', [self.compilers()[1], '-std=c++17', '-fsyntax-only',
                '-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1', '-DJUCE_STANDALONE_APPLICATION=1',
                '-DJUCE_WEB_BROWSER=0', '-DJUCE_USE_CURL=0', '-DJUCE_PLUGINHOST_VST3=1',
                '-I' + str(juce / 'modules'), '-I/usr/include/freetype2', probe])
        self.sanitizer_controls()
        if self.system == 'Linux' and self.flags:
            self.run('symbolizer-version', ['/usr/bin/llvm-symbolizer-21', '--version'])
            self.module_runtime_probe()
        if self.args.profile == 'Release':
            self.prepare_validator_source()
            self.build_pluginval()
            self.build_vst3_validator()
        self.save()
        return juce

    def cmake_options(self, juce):
        cc, cxx = self.compilers()
        options = ['-DCMAKE_BUILD_TYPE=' + self.config,
                   '-DCMAKE_MAKE_PROGRAM:FILEPATH=' + self.frontends['ninja'], '-DCMAKE_C_COMPILER=' + cc,
                   '-DCMAKE_CXX_COMPILER=' + cxx, '-DSWARAXT_JUCE_DIR=' + str(juce),
                   '-DSWARA_BUILD_TESTS=ON', '-DSWARA_BUILD_STANDALONE=OFF', '-DSWARA_BUILD_SKIN_EDITOR=OFF',
                   '-DSWARAXT_SHARE_TEST_JUCE=ON', '-DSWARAXT_SHARE_TEST_PRODUCTION=ON',
                   '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
        # Initialize instead of replacing the native MSVC defaults: Windows
        # appends /EHsc, /GR and its platform definitions during language setup.
        suffix = '_INIT' if self.system == 'Windows' else ''
        options += ['-DCMAKE_C_FLAGS' + suffix + '=' + ' '.join(self.flags),
                    '-DCMAKE_CXX_FLAGS' + suffix + '=' + ' '.join(self.flags)]
        if self.system == 'Windows':
            options += ['-DCMAKE_C_FLAGS_DEBUG=/Zi /Ob0 /Od', '-DCMAKE_CXX_FLAGS_DEBUG=/Zi /Ob0 /Od',
                        '-DCMAKE_EXE_LINKER_FLAGS=/INCREMENTAL:NO /DEBUG',
                        '-DCMAKE_SHARED_LINKER_FLAGS=/INCREMENTAL:NO /DEBUG',
                        '-DCMAKE_MODULE_LINKER_FLAGS=/INCREMENTAL:NO /DEBUG']
        elif self.args.compiler == 'clang':
            options += ['-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld-21' + (' -Wl,--export-dynamic' if self.flags else ''),
                        '-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld-21', '-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld-21']
        return options

    def configure(self, juce):
        build = self.root / 'build'
        extra = ['-DCMAKE_PROJECT_INCLUDE=' + str(CI / 'project.cmake'),
                 '-DSWARAXT_CI_SANITIZER=' + ('ON' if self.flags else 'OFF')]
        if self.system == 'Linux' and self.flags:
            sanitizer = 'asan-ubsan' if self.args.profile == 'fuzz' else self.args.profile
            extra += ['-DSWARAXT_DIAGNOSTICS_MODULE=' + str(CI / 'diagnostics.cmake'),
                      '-DDIAG_SANITIZER=' + sanitizer,
                      '-DDIAG_FUZZER=' + ('libfuzzer' if self.args.profile == 'fuzz' else 'OFF')]
        self.run('configure', ['cmake', '-S', SOURCE, '-B', build, '-G', 'Ninja', *self.cmake_options(juce), *extra])
        cache = (build / 'CMakeCache.txt').read_text()
        for option in ('SWARA_BUILD_STANDALONE:BOOL=OFF', 'SWARA_BUILD_SKIN_EDITOR:BOOL=OFF'):
            if option not in cache:
                raise RuntimeError('Unexpected product build mode')
        if self.system == 'Windows':
            validate_msvc_defaults(json.loads((build / 'compile_commands.json').read_text()))
            self.report['native_exception_unwinding'] = 'MSVC /EHsc verified before compilation'
        return build

    def sanitizer_controls(self):
        if not self.flags:
            return
        samples = {
            'asan': [('asan', '#include <cstdlib>\nint main(int n,char**){auto*p=(volatile int*)malloc(sizeof(int));p[n+3]=9;free((void*)p);}', 'AddressSanitizer')],
            'asan-ubsan': [('asan', '#include <cstdlib>\nint main(int n,char**){auto*p=(volatile int*)malloc(sizeof(int));p[n+3]=9;free((void*)p);}', 'AddressSanitizer'),
                           ('ubsan', '#include <climits>\nint main(int n,char**){volatile int x=INT_MAX;return x+n;}', 'runtime error:')],
            'tsan': [('tsan', '#include <thread>\nint x;void f(){for(int i=0;i<100000;++i)++x;}int main(){std::thread a(f),b(f);a.join();b.join();}', 'ThreadSanitizer: data race')],
            'rtsan': [('rtsan', '#include <cstdlib>\nvoid*volatile p;void f() [[clang::nonblocking]]{p=malloc(4);free(p);}int main(){f();}', 'RealtimeSanitizer')],
        }
        selected = 'asan-ubsan' if self.args.profile == 'fuzz' else self.args.profile
        if self.system == 'Linux' and selected == 'asan-ubsan':
            samples[selected].append(('lsan', '#include <cstdlib>\n__attribute__((noinline)) void f(){auto*p=(volatile char*)malloc(123);p[0]=7;}int main(){f();}', 'LeakSanitizer'))
        for name, source, diagnostic in samples[selected]:
            path = self.root / (name + '-negative.cpp'); path.write_text(source + '\n')
            binary = self.root / (name + '-negative' + ('.exe' if self.system == 'Windows' else ''))
            if self.system == 'Windows':
                argv = ['cl', '/nologo', '/std:c++17', '/EHsc', '/Od', *self.flags, path,
                        '/Fe:' + str(binary), '/link', '/INCREMENTAL:NO', '/DEBUG']
            else:
                argv = [self.compilers()[1], '-std=c++17', '-O1', '-g', *self.flags, path, '-o', binary, '-pthread']
            self.run(name + '-control-build', argv)
            code = self.run(name + '-negative-control', [binary], expected=None)
            if code == 0 or diagnostic not in (self.root / (name + '-negative-control.log')).read_text(errors='replace'):
                raise RuntimeError('Sanitizer sensitivity negative control failed')
        self.report['sanitizer_negative_controls'] = 'PASS'

    def module_runtime_probe(self):
        module_source = self.root / 'module-runtime.cpp'
        module_source.write_text('extern "C" int probe(int n){int a[2]={n,n+1};return a[n%2];}\n')
        module = self.root / 'module-runtime.so'
        compiler = self.compilers()[1]
        self.run('module-runtime-build', [compiler, '-std=c++17', '-O1', *self.flags,
                 '-shared', '-fPIC', module_source, '-o', module])
        host_source = self.root / 'module-runtime-host.cpp'
        host_source.write_text('#include <dlfcn.h>\n#include <cstdio>\nint main(int n,char**v){if(n!=2)return 2;'
            'void*h=dlopen(v[1],RTLD_NOW);if(!h){std::puts(dlerror());return 3;}'
            'auto f=reinterpret_cast<int(*)(int)>(dlsym(h,"probe"));if(!f||f(1)!=2)return 4;'
            'std::puts("MATCHING_SANITIZER_MODULE_RUNTIME_PASS");return 0;}\n')
        host = self.root / 'module-runtime-host'
        self.run('module-runtime-host-build', [compiler, '-std=c++17', '-O1', *self.flags,
                 '-Wl,--export-dynamic', host_source, '-ldl', '-o', host])
        self.run('module-runtime-load', [host, module])
        self.report['matching_sanitizer_module_runtime'] = 'PASS'

    def build(self, build, name, targets, timeout=10800):
        self.run(name, ['cmake', '--build', build, '--target', *targets, '--parallel', '2'], timeout=timeout)

    def reference(self, juce, build):
        baseline = self.root / 'frozen-upstream'
        self.run('clone-reference', ['git', '-c', 'core.autocrlf=false', 'clone', '--filter=blob:none',
                 '--no-checkout', 'https://github.com/montronedsp/SWARAXT.git', baseline])
        self.run('checkout-reference', ['git', '-C', baseline, '-c', 'core.autocrlf=false', 'checkout', '--detach', BASELINE_SHA])
        self.run('reference-submodules', ['git', '-C', baseline, '-c', 'url.https://github.com/.insteadOf=git://github.com/',
                                         'submodule', 'update', '--init', '--recursive'])
        for relative in [*TESTS.values(), 'Tests/Parity/ExactParityGoldens.h', 'Tests/Filter/DualSvfSection2Goldens.h']:
            if sha(baseline / relative) != sha(SOURCE / relative):
                raise RuntimeError('Frozen reference driver or legacy golden changed')
        metadata_overlay = []
        if self.system == 'Windows':
            if self.output(['git', 'status', '--porcelain'], baseline):
                raise RuntimeError('Reference must be pristine before build-metadata staging')
            metadata_overlay = apply_reference_metadata(baseline, self.root)
        reference_build = self.root / 'reference-build'
        options = self.cmake_options(juce)
        if self.system == 'Windows':
            options += ['-DCMAKE_PROJECT_INCLUDE=' + str(CI / 'project.cmake'),
                        '-DSWARAXT_CI_SANITIZER=' + ('ON' if self.flags else 'OFF')]
        self.run('configure-reference', ['cmake', '-S', baseline, '-B', reference_build, '-G', 'Ninja', *options])
        self.build(reference_build, 'build-reference', list(TESTS))
        root = self.root / 'native-reference'; root.mkdir()
        manifest = {'baseline_sha': BASELINE_SHA, 'candidate_sha': self.report['source_sha'],
                    'architecture': architecture(), 'profile': self.env['SWARAXT_NATIVE_PROFILE'],
                    'run_id': self.report['run_id'], 'baseline_build_metadata_overlay': metadata_overlay, 'tests': {}}
        for name in TESTS:
            code = self.run('reference-' + name, [self.binary(reference_build, name)], expected=None)
            record = validate_output(name, (self.root / ('reference-' + name + '.log')).read_text(errors='replace'), code, baseline)
            file = root / (name + '.json'); file.write_text(json.dumps(record, indent=2) + '\n')
            manifest['tests'][name] = {'record_file': file.name, 'record_sha256': sha(file),
                                     'case_count': len(record['records']), 'legacy_exit_code': code,
                                     'historical_hash_mismatches': record['historical_hash_mismatches']}
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        negative = self.root / 'negative-reference'; shutil.copytree(root, negative)
        name = 'SwaraXTHostSrcExactParityTests'; file = negative / manifest['tests'][name]['record_file']
        record = json.loads(file.read_text()); case = next(key for key in record['records'] if 'impulse' in key)
        value = record['records'][case][1]; record['records'][case][1] = ('1' if value[0] == '0' else '0') + value[1:]
        file.write_text(json.dumps(record, indent=2) + '\n')
        bad = json.loads((negative / 'manifest.json').read_text()); bad['tests'][name]['record_sha256'] = sha(file)
        (negative / 'manifest.json').write_text(json.dumps(bad, indent=2) + '\n')
        code = self.run('reference-negative-control', [sys.executable, CI / 'native_reference.py',
            '--reference-root', negative, '--test-name', name, self.binary(build, name)], expected=None)
        if code == 0 or 'Actual native audio differs' not in (self.root / 'reference-negative-control.log').read_text(errors='replace'):
            raise RuntimeError('Native reference mutation control failed')
        changes = self.output(['git', 'diff', '--name-only'], baseline).splitlines()
        if set(changes) != {record['file'] for record in metadata_overlay}:
            raise RuntimeError('Unexpected change in frozen reference checkout')
        for record in metadata_overlay:
            if sha(baseline / record['file']) != record['after_sha256']:
                raise RuntimeError('Frozen reference build metadata changed unexpectedly')
        if self.output(['git', 'diff', '--name-only', '--', 'Source', *TESTS.values(),
                        'Tests/Parity/ExactParityGoldens.h', 'Tests/Filter/DualSvfSection2Goldens.h'], baseline):
            raise RuntimeError('Frozen production source, driver or golden changed')
        self.report['native_reference_negative_control'] = 'PASS'
        self.report['native_audio_reference'] = manifest
        self.run('configure-reference-launchers', ['cmake', '-S', SOURCE, '-B', build,
                  '-DSWARAXT_CI_REFERENCE_ROOT=' + str(root)])

    def audit(self, build):
        entries = json.loads((build / 'compile_commands.json').read_text())
        production = [e for e in entries if Path(e['file']).resolve().is_relative_to(SOURCE / 'Source')]
        if len(production) < 10:
            raise RuntimeError('Production compile-command inventory missing')
        resources = validate_compile_commands(entries, self.flags, self.config)
        shutil.copyfile(build / 'compile_commands.json', self.root / 'compile-commands.json')
        self.report.update(compile_commands_count=len(entries), production_compile_commands_count=len(production),
                           resource_compile_commands_count=resources,
                           c_cpp_compile_commands_count=len(entries) - resources)

    def prepare_validator_source(self):
        tool = self.root / 'pluginval-source'
        self.run('clone-pluginval', pluginval_git('clone', '--filter=blob:none',
                 '--no-checkout', 'https://github.com/Tracktion/pluginval.git', tool))
        self.run('checkout-pluginval', pluginval_git('-C', tool, 'checkout', '--detach', PLUGINVAL_SHA))
        self.run('pluginval-juce', pluginval_git('-C', tool, 'submodule', 'update', '--init', '--depth', '1', 'modules/juce'))
        if (self.output(['git', '-C', tool, 'cat-file', '-t', PLUGINVAL_SHA]) != 'commit' or
                self.output(['git', '-C', tool, 'rev-parse', 'HEAD']) != PLUGINVAL_SHA or
                self.output(['git', '-C', tool / 'modules/juce', 'rev-parse', 'HEAD']) != PLUGINVAL_JUCE_SHA):
            raise RuntimeError('pluginval source/vendor revision mismatch')
        provenance = apply_pluginval_source(tool, self.root)
        self.report['pluginval_source'] = provenance; self.save()

    def build_pluginval(self):
        tool = self.root / 'pluginval-source'
        provenance = self.report['pluginval_source']
        tool_build = self.root / 'pluginval-build'; cc, cxx = self.compilers()
        self.run('configure-pluginval', ['cmake', '-S', tool, '-B', tool_build, '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                 '-DCMAKE_MAKE_PROGRAM:FILEPATH=' + self.frontends['ninja'],
                 '-DCMAKE_C_COMPILER=' + cc, '-DCMAKE_CXX_COMPILER=' + cxx])
        self.build(tool_build, 'build-pluginval', ['pluginval'])
        executable = self.binary(tool_build, 'pluginval')
        provenance['binary_sha256'] = sha(executable)
        verify_binary(executable, self.system)
        self.save()
        self.run('pluginval-tool-selftest', [executable, '--help'], timeout=60)

    def build_vst3_validator(self):
        sdk = self.root / 'vst3sdk'
        self.run('clone-sdk', ['git', 'clone', '--filter=blob:none', '--no-checkout', 'https://github.com/steinbergmedia/vst3sdk.git', sdk])
        self.run('checkout-sdk', ['git', '-C', sdk, 'checkout', '--detach', SDK_SHA])
        self.run('sdk-submodules', ['git', '-C', sdk, 'submodule', 'update', '--init', '--recursive', 'base', 'cmake', 'pluginterfaces', 'public.sdk'])
        if self.output(['git', '-C', sdk, 'rev-parse', 'HEAD']) != SDK_SHA:
            raise RuntimeError('VST3 SDK revision mismatch')
        project = self.root / 'validator-project'; project.mkdir()
        shutil.copyfile(CI / 'Validator.cmake', project / 'CMakeLists.txt')
        build = self.root / 'validator-build'; cc, cxx = self.compilers()
        self.run('configure-validator', ['cmake', '-S', project, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_MAKE_PROGRAM:FILEPATH=' + self.frontends['ninja'],
                 '-DCMAKE_C_COMPILER=' + cc, '-DCMAKE_CXX_COMPILER=' + cxx, '-DSWARAXT_SDK_ROOT=' + str(sdk)])
        self.build(build, 'build-validator', ['validator'])
        validator = self.binary(build, 'validator')
        verify_binary(validator, self.system)
        self.report['vst3sdk_tool'] = {'source_sha': SDK_SHA, 'binary_sha256': sha(validator)}
        self.save()
        self.run('validator-selftest', [validator, '-selftest'])

    def validators(self, bundle):
        executable = self.binary(self.root / 'pluginval-build', 'pluginval')
        if sha(executable) != self.report['pluginval_source']['binary_sha256']:
            raise RuntimeError('pluginval binary changed after preflight')
        self.run('pluginval', [executable, '--strictness-level', '10', '--repeat', '2', '--timeout-ms', '120000',
                 '--randomise', '--verbose', '--random-seed', '0x53776172', '--output-dir', self.root / 'pluginval-results',
                 '--validate', bundle], timeout=2400)
        validator = self.binary(self.root / 'validator-build', 'validator')
        if sha(validator) != self.report['vst3sdk_tool']['binary_sha256']:
            raise RuntimeError('VST3 validator changed after preflight')
        self.run('validator-extensive', [validator, '-e', bundle], timeout=1800)
        self.report['external_validators'] = {'pluginval': '1.0.4+upstream-state-cache-fix', 'sha256': sha(executable), 'strictness': 10,
                                            'repeat': 2, 'vst3sdk_sha': SDK_SHA,
                                            'vst3sdk_binary_sha256': sha(validator)}

    def package(self, bundle):
        stage = self.root / 'candidate'; stage.mkdir()
        shutil.copytree(bundle, stage / bundle.name)
        files = {p.relative_to(stage).as_posix(): sha(p) for p in sorted(stage.rglob('*')) if p.is_file()}
        manifest = {k: self.report[k] for k in ('source_sha', 'run_id', 'platform', 'architecture', 'compiler', 'profile', 'juce_sha')}
        manifest.update(version='1.2.3', formats=['VST3'], standalone=False, files=files,
                        dependency_overlay=self.report['dependency_overlay'],
                        distribution='UNSIGNED_CANDIDATE; full workflow gate required')
        (stage / 'provenance.json').write_text(json.dumps(manifest, indent=2) + '\n')
        name = 'swaraxt-' + self.system.lower() + '-' + self.args.compiler + '-vst3-1.2.3'
        archive = Path(shutil.make_archive(str(self.root / name), 'zip', stage))
        (self.root / (name + '.sha256')).write_text(sha(archive) + '  ' + archive.name + '\n')
        self.report['candidate'] = {'archive': archive.name, 'sha256': sha(archive), 'manifest': manifest}

    def fuzz(self, build):
        self.build(build, 'build-fuzzers', ['SwaraXTTuningFuzz', 'SwaraXTStateMidiFuzz'])
        self.audit(build)
        results = {}
        for target in ('SwaraXTTuningFuzz', 'SwaraXTStateMidiFuzz'):
            corpus = self.root / (target + '-corpus'); corpus.mkdir()
            for index, data in enumerate((b'\0', b'\1\0\0\1', b'\2\x7f\x7f\x7f', b'fuzz scale\n2\n700.0\n1200.0\n')):
                (corpus / str(index)).write_bytes(data)
            artifacts = self.root / (target + '-crashes'); artifacts.mkdir()
            self.run(target, [self.binary(build, target), '-seed=1398235506', '-runs=50000', '-max_total_time=300',
                '-max_len=4096', '-timeout=20', '-rss_limit_mb=4096', '-print_final_stats=1',
                '-artifact_prefix=' + str(artifacts) + os.sep, corpus], timeout=600)
            text = (self.root / (target + '.log')).read_text(errors='replace')
            count = re.search(r'stat::number_of_executed_units:\s*(\d+)', text)
            if not count or int(count[1]) == 0:
                raise RuntimeError('No executed fuzz inputs')
            results[target] = int(count[1])
        self.report['fuzz_executions'] = results

    def tests(self, juce, build):
        self.build(build, 'build-platform-preflight', ['AvrlibHdUpperRegisterOscillatorTests'])
        self.run('platform-preflight', ['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error',
                                       '-R', '^AvrlibHdUpperRegisterOscillatorTests$'])
        self.build(build, 'build-host-preflight', ['SwaraXTNativePluginHost', 'SwaraXT_VST3'])
        if self.system == 'Linux':
            self.run('framework-probes', [self.binary(build, 'SwaraXTNativePluginHost'), '--self-test-framework'], timeout=60)
            if 'FRAMEWORK_LOCK_QUEUE_PASS' not in (self.root / 'framework-probes.log').read_text(errors='replace'):
                raise RuntimeError('Native framework controls did not complete')
        bundle = build / 'SwaraXT_artefacts' / self.config / 'VST3/Swara XT.vst3'
        binaries = [p for p in bundle.rglob('*') if p.is_file() and p.suffix in ('.so', '.vst3', '.dll')]
        if len(binaries) != 1:
            raise RuntimeError('Expected exactly one VST3 executable')
        verify_binary(binaries[0], self.system)
        self.run('actual-VST3', [self.binary(build, 'SwaraXTNativePluginHost'), 'VST3', bundle, self.root / 'VST3-editor.png'], timeout=600)
        validate_host_log((self.root / 'actual-VST3.log').read_text(errors='replace'))
        self.report['actual_native_vst3'] = 'PASS'
        if self.system == 'Linux' and not self.flags:
            code = self.run('peer-teardown-negative-control',
                [self.binary(build, 'SwaraXTNativePluginHost'), 'VST3', bundle,
                 self.root / 'negative-VST3-editor.png', '--premature-peer-teardown'],
                timeout=600, expected=None)
            validate_peer_teardown_control((self.root / 'peer-teardown-negative-control.log').read_text(errors='replace'), code)
            self.report['peer_teardown_negative_control'] = 'PASS'
        if self.args.profile == 'Release':
            self.validators(bundle)
        self.build(build, 'build-regressions', ['SwaraXTQualificationTests'])
        self.reference(juce, build)
        self.audit(build)
        inventory = json.loads(self.output(['ctest', '--test-dir', build, '--show-only=json-v1']))
        if len(inventory['tests']) < 90 or any(p['name'] == 'DISABLED' and p['value'] for t in inventory['tests'] for p in t['properties']):
            raise RuntimeError('Missing or disabled regression coverage')
        (self.root / 'ctest-inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
        self.report['test_count'] = len(inventory['tests']); self.save()
        self.run('ctest', ['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error', '--parallel', '1',
                 '--test-output-size-passed', '16777216', '--test-output-size-failed', '16777216',
                 '--output-junit', self.root / 'ctest.xml', '--verbose'], timeout=16200)
        validate_ctest(self.root / 'ctest.xml', len(inventory['tests']))
        if self.args.profile == 'Release':
            self.package(bundle)

    def execute(self):
        try:
            juce = self.prepare(); build = self.configure(juce)
            if self.args.profile == 'fuzz': self.fuzz(build)
            else: self.tests(juce, build)
            if self.output(['git', 'status', '--porcelain'], SOURCE):
                raise RuntimeError('Source checkout changed during qualification')
            self.report['decision'] = 'PASS'
        except BaseException as error:
            self.report.update(decision='FAIL', failure=str(error))
            raise
        finally:
            self.save()


def toolchain(args):
    campaign = Campaign(args)
    try:
        campaign.prepare()
        campaign.report.update(decision='VALIDATOR_TOOLCHAIN_PASS', product_qualification='NOT_RUN')
    except BaseException as error:
        campaign.report.update(decision='FAIL', failure=str(error), product_qualification='NOT_RUN')
        raise
    finally:
        campaign.save()


def contained_runner_path(path, must_exist):
    """Resolve a qualification path and keep it inside RUNNER_TEMP."""
    root_text = os.environ.get('RUNNER_TEMP')
    if not root_text:
        raise RuntimeError('Qualification paths require RUNNER_TEMP')
    root = Path(root_text).resolve(strict=True)
    candidate = Path(path)
    if candidate.is_symlink():
        raise RuntimeError('Qualification path must not be a symlink')
    resolved = candidate.resolve(strict=must_exist)
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise RuntimeError('Qualification path escaped RUNNER_TEMP') from error
    return resolved


def admit_reference_record(profile_root, name, entry):
    """Read one native-reference record without leaving its evidence directory."""
    expected = name + '.json'
    record_file = entry.get('record_file') if isinstance(entry, dict) else None
    digest = entry.get('record_sha256') if isinstance(entry, dict) else None
    if name not in TESTS or record_file != expected:
        raise RuntimeError('Unexpected native reference filename')
    if (not isinstance(digest, str) or len(digest) != 64
            or any(character not in '0123456789abcdef' for character in digest)):
        raise RuntimeError('Malformed native reference checksum')
    evidence = Path(profile_root).resolve(strict=True)
    reference = evidence / 'native-reference'
    if reference.is_symlink() or not reference.is_dir():
        raise RuntimeError('Native reference directory is not a trusted directory')
    candidate = reference / expected
    if candidate.is_symlink() or not candidate.is_file():
        raise RuntimeError('Missing or untrusted native reference record')
    resolved = candidate.resolve(strict=True)
    try:
        relative = resolved.relative_to(evidence)
    except ValueError as error:
        raise RuntimeError('Native reference record escaped the evidence directory') from error
    if relative != Path('native-reference') / expected or resolved.parent != reference.resolve():
        raise RuntimeError('Native reference record escaped the evidence directory')
    if sha(resolved) != digest:
        raise RuntimeError('Changed native reference record')


def gate(system, evidence, output):
    evidence = contained_runner_path(evidence, True)
    output = contained_runner_path(output, False)
    expected = set(LINUX_PROFILES if system == 'Linux' else WINDOWS_PROFILES)
    results = {}
    for path in evidence.rglob('report.json'):
        data = json.loads(path.read_text())
        key = (data['compiler'], data['profile'])
        if key in results or data['platform'] != system or data['source_sha'] != os.environ['GITHUB_SHA'] or data['run_id'] != os.environ['GITHUB_RUN_ID']:
            raise RuntimeError('Duplicate or mismatched profile provenance')
        if data['decision'] != 'PASS':
            raise RuntimeError('Profile did not pass')
        commands = {entry['name']: entry['exit_code'] for entry in data['commands']}
        if commands.get('harness-tests') != 0 or commands.get('configure') != 0:
            raise RuntimeError('Missing native setup/harness validation')
        if data['profile'] == 'fuzz':
            if len(data['fuzz_executions']) != 2 or not all(n > 0 for n in data['fuzz_executions'].values()):
                raise RuntimeError('Missing fuzz execution')
        else:
            validate_ctest(path.parent / 'ctest.xml', data['test_count'])
            if data['actual_native_vst3'] != 'PASS' or data['native_reference_negative_control'] != 'PASS':
                raise RuntimeError('Missing actual plugin/native audio validation')
            for name in ('actual-VST3', 'ctest', 'build-regressions'):
                if commands.get(name) != 0:
                    raise RuntimeError('Missing successful native command: ' + name)
            reference = data['native_audio_reference']
            if reference['candidate_sha'] != data['source_sha'] or reference['baseline_sha'] != BASELINE_SHA or reference['run_id'] != data['run_id'] or reference['profile'] != data['compiler'] + '-' + data['profile'] or reference['architecture'] != 'x86_64':
                raise RuntimeError('Mismatched native reference provenance')
            for name in TESTS:
                admit_reference_record(path.parent, name, reference['tests'][name])
            host = (path.parent / 'actual-VST3.log').read_text(errors='replace')
            validate_host_log(host)
            if 'BOOLEAN_NORMALIZATION_PASS' not in host:
                raise RuntimeError('Missing native boolean normalization control')
            if system == 'Linux' and (commands.get('framework-probes') != 0 or
                    'FRAMEWORK_LOCK_QUEUE_PASS' not in (path.parent / 'framework-probes.log').read_text(errors='replace')):
                raise RuntimeError('Missing native framework lock/queue validation')
            if system == 'Linux' and data['profile'] in ('Release', 'Debug'):
                if data['peer_teardown_negative_control'] != 'PASS':
                    raise RuntimeError('Missing native peer teardown regression control')
                validate_peer_teardown_control((path.parent / 'peer-teardown-negative-control.log').read_text(errors='replace'),
                                               commands.get('peer-teardown-negative-control', 0))
            if data['profile'] == 'Release':
                sdk_tool = data['vst3sdk_tool']
                if sdk_tool['source_sha'] != SDK_SHA or sdk_tool['binary_sha256'] != data['external_validators']['vst3sdk_binary_sha256']:
                    raise RuntimeError('Missing VST3 SDK validator provenance')
                tool = data['pluginval_source']
                if (tool['source_sha'] != PLUGINVAL_SHA or tool['vendor_juce_sha'] != PLUGINVAL_JUCE_SHA
                        or tool['upstream_fix_sha'] != PLUGINVAL_FIX_SHA
                        or tool['before_sha256'] != PLUGINVAL_BEFORE or tool['after_sha256'] != PLUGINVAL_AFTER
                        or tool['compiler_portability'] != PLUGINVAL_PORTABILITY
                        or tool['binary_sha256'] != data['external_validators']['sha256']):
                    raise RuntimeError('Missing corrected pluginval provenance')
                for name in ('configure-pluginval', 'build-pluginval', 'pluginval-tool-selftest', 'pluginval',
                             'configure-validator', 'build-validator', 'validator-selftest', 'validator-extensive'):
                    if commands.get(name) != 0:
                        raise RuntimeError('Missing successful external validator: ' + name)
                text = (path.parent / 'pluginval.log').read_text(errors='replace')
                if not all(marker in text for marker in ('Num plugins found: 1', 'Strictness level: 10', 'v1.2.3', 'SUCCESS')):
                    raise RuntimeError('Incomplete pluginval product/stress validation')
                result = re.search(r'Result: (\d+) tests passed, (\d+) tests failed',
                    (path.parent / 'validator-extensive.log').read_text(errors='replace'))
                if not result or int(result[1]) < 100 or int(result[2]) != 0:
                    raise RuntimeError('Incomplete extensive VST3 validation')
        if data['profile'] not in ('Release', 'Debug') and data['sanitizer_negative_controls'] != 'PASS':
            raise RuntimeError('Missing sanitizer sensitivity control')
        results[key] = 'PASS'
    if set(results) != expected:
        raise RuntimeError('Missing or extra native profiles: ' + str(expected.symmetric_difference(results)))
    report = {'source_sha': os.environ['GITHUB_SHA'], 'run_id': os.environ['GITHUB_RUN_ID'], 'platform': system,
              'decision': 'NATIVE_CI_MATRIX_PASS', 'profiles': {c + '-' + p: s for (c, p), s in results.items()},
              'distribution_approval': False, 'visible_gui_acceptance': 'NOT_RUN', 'installation': 'NOT_RUN'}
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='task', required=True)
    native = commands.add_parser('native')
    native.add_argument('--root', type=Path, required=True)
    native.add_argument('--compiler', choices=['gcc', 'clang', 'msvc'], required=True)
    native.add_argument('--profile', choices=['Release', 'Debug', 'asan-ubsan', 'asan', 'tsan', 'rtsan', 'fuzz'], required=True)
    preflight = commands.add_parser('toolchain', help='Build and execute the external validator only; no product qualification')
    preflight.add_argument('--root', type=Path, required=True)
    preflight.add_argument('--compiler', choices=['gcc', 'clang', 'msvc'], required=True)
    preflight.set_defaults(profile='Release')
    review = commands.add_parser('gate')
    review.add_argument('--platform', choices=['Linux', 'Windows'], required=True)
    review.add_argument('--evidence', type=Path, required=True)
    review.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.task == 'native': Campaign(args).execute()
    elif args.task == 'toolchain': toolchain(args)
    else: gate(args.platform, args.evidence, args.output)


if __name__ == '__main__':
    main()
