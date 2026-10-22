# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build-owned repair of legacy Windows CTest directory metadata only."""
from pathlib import Path
from common import sha

PARENT_BEFORE = '''    set_tests_properties(SwaraXtShruthiSvf2pSanitizerTests SwaraBoardSanitizerTests
        SwaraBoardIntegrationSanitizerTests
        PROPERTIES LABELS sanitizer TIMEOUT 600)'''
PARENT_AFTER = '''    set_tests_properties(SwaraXtShruthiSvf2pSanitizerTests
        PROPERTIES LABELS sanitizer TIMEOUT 600)'''
CHILD_BEFORE = '    set_tests_properties(SwaraBoardSanitizerTests SwaraBoardIntegrationSanitizerTests PROPERTIES\n'
CHILD_AFTER = CHILD_BEFORE + '        LABELS sanitizer TIMEOUT 600\n'


def apply(source, owned_root):
    source, owned_root = Path(source).resolve(), Path(owned_root).resolve()
    if not source.is_relative_to(owned_root) or source.name != 'frozen-upstream':
        raise RuntimeError('Only the owned frozen-reference clone may receive metadata overlays')
    pending = []
    for name, before, after in [('Tests/Qualification.cmake', PARENT_BEFORE, PARENT_AFTER),
                                ('Tests/DspBoard/CMakeLists.txt', CHILD_BEFORE, CHILD_AFTER)]:
        path = source / name
        if not path.resolve().is_relative_to(source):
            raise RuntimeError('Reference metadata resolves outside the owned clone')
        data = path.read_bytes()
        newline = b'\r\n' if b'\r\n' in data else b'\n'
        before_bytes = before.encode().replace(b'\n', newline)
        after_bytes = after.encode().replace(b'\n', newline)
        if data.count(before_bytes) != 1 or after_bytes in data:
            raise RuntimeError('Legacy reference metadata anchor changed')
        pending.append((path, data.replace(before_bytes, after_bytes),
                        {'file': name, 'before_sha256': sha(path)}))
    records = []
    for path, data, record in pending:
        path.write_bytes(data)
        record['after_sha256'] = sha(path); records.append(record)
    return records
