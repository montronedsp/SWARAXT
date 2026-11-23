# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact upstream pluginval state-cache repair in an owned tool checkout."""
import hashlib
from pathlib import Path

SOURCE_SHA = 'ed19c2c16b57a6d94db391bea3ef4a80b769d5bf'
VENDOR_SHA = '5179f4e720d8406ebd1b5401c86aea8da6cc83c9'
FIX_SHA = '0507946fc17418e5170010b6ad2db16f7c70c744'
BEFORE_SHA256 = '0d81ea3edd095ce1110740b1ef1e383a0cf8f7768a6160657ef2cd7da657f0e1'
AFTER_SHA256 = '9698aac439eac1998a66603e210b24b1d1e3d633b2881929657a16c9420fa591'
PORTABILITY_FILES = {'Source/Validator.cpp': {
    'before_sha256': '1e642c5040d1cd888d0430af7902d95a9b37b40f3fd0df046a78b66bb643fd17',
    'after_sha256': '1a6ef7eaafd952ae07606555bcc976a8a7ea06aa104d47f822f2ebc197c77b6b'}}
ANCHOR = b'        const auto tolaratedDiff = 0.1f;'
INSERT = b'''        // Hosted parameters report the last value sent to them until a state is
        // restored, so restore once here to start from the plugin's own values
        callSetStateInformationOnMessageThreadIfVST3 (instance, originalState);

'''


def git_argv(*args):
    # text=auto attributes use core.eol even with autocrlf=false on Windows.
    return ['git', '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', *args]


def apply(source, owned_root):
    source = Path(source).resolve()
    if source != Path(owned_root).resolve() / 'pluginval-source':
        raise RuntimeError('pluginval repair requires the build-owned tool checkout')
    files = {'Source/tests/BasicTests.cpp': {'before_sha256': BEFORE_SHA256, 'after_sha256': AFTER_SHA256},
             **PORTABILITY_FILES}
    pending = {}
    for name, hashes in files.items():
        path = source / name
        if not path.resolve().is_relative_to(source):
            raise RuntimeError('pluginval source resolves outside the owned checkout')
        before = path.read_bytes()
        if hashlib.sha256(before).hexdigest() != hashes['before_sha256']:
            raise RuntimeError('Pinned pluginval source changed: ' + name)
        after = before
        if name == 'Source/tests/BasicTests.cpp':
            if before.count(ANCHOR) != 1:
                raise RuntimeError('pluginval state-test anchor changed')
            after = after.replace(ANCHOR, INSERT + ANCHOR)
        if after.count(b'#include <thread>') != 1:
            raise RuntimeError('pluginval chrono include anchor changed')
        after = after.replace(b'#include <thread>', b'#include <chrono>\n#include <thread>')
        if hashlib.sha256(after).hexdigest() != hashes['after_sha256']:
            raise RuntimeError('pluginval correction differs from the reviewed patch')
        pending[path] = after
    for path, after in pending.items():
        path.write_bytes(after)
    return {'source_sha': SOURCE_SHA, 'vendor_juce_sha': VENDOR_SHA,
            'upstream_fix_sha': FIX_SHA, 'file': 'Source/tests/BasicTests.cpp',
            'before_sha256': BEFORE_SHA256, 'after_sha256': AFTER_SHA256,
            'compiler_portability': PORTABILITY_FILES}
