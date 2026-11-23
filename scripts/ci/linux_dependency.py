# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the owned stock Linux dependency; CMake generates the qualified overlay."""
import hashlib
import subprocess
from pathlib import Path
STOCK_904_SHA = '3fb5cbc0340758596dac39f7c8de47d562b4896e'
STOCK_904_HASHES = {'CMakeLists.txt': 'aae85275cc7f08b89dac8a6c0d075d5475ff114a327ece1b77fcf60da1bad738', 'modules/juce_events/native/juce_Messaging_linux.cpp': 'b3adb1f787c9af61fff19a31a1457a47b274efcd8e58f3773b559093e34e5f5b', 'modules/juce_events/messages/juce_MessageManager.cpp': 'dd644cf2f2a3bac0b7f7737cdabac2ad9307e7a33ca2fc748d8c2876c39f97fa', 'modules/juce_audio_basics/buffers/juce_AudioChannelSet.h': '445a53855d3fabc8304db1fdd465b43718924644864a9f429ca4839f00eb930a', 'modules/juce_audio_utils/players/juce_AudioProcessorPlayer.cpp': 'f4c99635ed179993bc3316e3e568ccc0447c89163f39e2caa51952ae68181f1e', 'modules/juce_audio_utils/juce_audio_utils.cpp': '52edf748d788e7162e92002fefa13c45eda998650955534ca0db0fa9d1eaa78d'}

STOCK_904_HASHES['modules/juce_audio_basics/buffers/juce_AudioChannelSet.cpp'] = '6384bdcfdc26f369c85e30dbdb869ad249e054bd90ea75b234e8f4729af25d34'

def apply(source, owned_root):
    source = Path(source).resolve()
    if source != Path(owned_root).resolve() / 'JUCE':
        raise RuntimeError('Linux dependency verification requires the owned JUCE checkout')
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=all', '--ignored'], text=True)
    if revision != STOCK_904_SHA or dirty:
        raise RuntimeError('Linux requires pristine exact JUCE 9.0.4')
    for name, expected in STOCK_904_HASHES.items():
        path = source / name
        if not path.resolve().is_relative_to(source) or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise RuntimeError('Linux JUCE identity mismatch: ' + name)
    return []  # No input dependency edits. ChannelType and MIDI overlays are build-owned.
