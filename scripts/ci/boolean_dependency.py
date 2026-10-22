# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Canonical boolean normalization in the owned pinned JUCE dependency."""
import hashlib
from pathlib import Path

FILE = 'modules/juce_audio_processors_headless/utilities/juce_AudioParameterBool.cpp'
OLD = b'value = newValue; valueChanged (get());'
NEW = b'value = newValue >= 0.5f ? 1.0f : 0.0f; valueChanged (get());'


def apply(source, owned_root):
    source = Path(source).resolve()
    if source != Path(owned_root).resolve() / 'JUCE':
        raise RuntimeError('Boolean normalization requires the build-owned JUCE checkout')
    path = source / FILE
    if not path.resolve().is_relative_to(source):
        raise RuntimeError('Boolean dependency resolves outside the owned checkout')
    before = path.read_bytes()
    if before.count(OLD) != 1:
        raise RuntimeError('Pinned JUCE boolean implementation changed')
    after = before.replace(OLD, NEW)
    path.write_bytes(after)
    return {'file': FILE, 'before_sha256': hashlib.sha256(before).hexdigest(),
            'after_sha256': hashlib.sha256(after).hexdigest()}
