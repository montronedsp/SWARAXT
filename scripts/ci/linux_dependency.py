# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded Linux fixes in an owned, pinned JUCE checkout only."""
import hashlib
from pathlib import Path

EDITS = (
    ('modules/juce_audio_basics/buffers/juce_AudioChannelSet.h',
     '    enum ChannelType\n', '    enum ChannelType : unsigned int\n'),
    ('modules/juce_events/messages/juce_MessageManager.cpp',
     '''bool MessageManagerLock::attemptLock (Thread* threadToCheck, ThreadPoolJob* jobToCheck)
{
    jassert (threadToCheck == nullptr || jobToCheck == nullptr);
''',
     '''bool MessageManagerLock::attemptLock (Thread* threadToCheck, ThreadPoolJob* jobToCheck)
{
    jassert (threadToCheck == nullptr || jobToCheck == nullptr);

    if (MessageManager::existsAndIsLockedByCurrentThread())
        return (threadToCheck == nullptr || ! threadToCheck->threadShouldExit())
            && (jobToCheck == nullptr || ! jobToCheck->shouldExit());
'''),
    ('modules/juce_events/native/juce_Messaging_linux.cpp',
     '''        if (bytesInSocket >= maxBytesInSocketQueue)
        {
            // Is the message thread overloaded by tasks taking too long?
            jassertfalse;
            return;
        }

        bytesInSocket++;

        ScopedUnlock ul (lock);
        unsigned char x = 0xff;
        [[maybe_unused]] auto numBytes = write (getWriteHandle(), &x, 1);
''',
     '''        if (wakeBytePending)
            return;

        // Keep one wakeup readable until the queue is empty, including when
        // dispatch yields to other file descriptors with callbacks remaining.
        unsigned char x = 0xff;
        ssize_t numBytes;
        do { numBytes = write (getWriteHandle(), &x, 1); }
        while (numBytes < 0 && errno == EINTR);
        jassert (numBytes == 1);
        wakeBytePending = numBytes == 1;
'''),
    ('modules/juce_events/native/juce_Messaging_linux.cpp',
     '''    int bytesInSocket = 0;
    static constexpr int maxBytesInSocketQueue = 128;
''',
     '''    bool wakeBytePending = false;
'''),
    ('modules/juce_events/native/juce_Messaging_linux.cpp',
     '''        if (bytesInSocket > 0)
        {
            --bytesInSocket;

            ScopedUnlock ul (lock);
            unsigned char x;
            [[maybe_unused]] auto numBytes = read (fd, &x, 1);
        }
''',
     '''        if (queue.isEmpty() && wakeBytePending)
        {
            unsigned char x;
            ssize_t numBytes;
            do { numBytes = read (fd, &x, 1); }
            while (numBytes < 0 && errno == EINTR);
            jassert (numBytes == 1);
            wakeBytePending = false;
        }
'''),
)


def apply(source, owned_root):
    source = Path(source).resolve()
    if source != Path(owned_root).resolve() / 'JUCE':
        raise RuntimeError('Linux dependency fixes require the build-owned JUCE checkout')
    originals, pending = {}, {}
    for name, old, new in EDITS:
        path = source / name
        if not path.resolve().is_relative_to(source):
            raise RuntimeError('Dependency file resolves outside the owned checkout')
        if name not in originals:
            originals[name] = path.read_bytes()
            pending[name] = originals[name]
        before = pending[name]
        newline = b'\r\n' if b'\r\n' in before else b'\n'
        anchor = old.encode().replace(b'\n', newline)
        if before.count(anchor) != 1:
            raise RuntimeError('Pinned Linux dependency anchor changed: ' + name)
        pending[name] = before.replace(anchor, new.encode().replace(b'\n', newline))
    records = []
    for name, after in pending.items():
        (source / name).write_bytes(after)
        records.append({'file': name, 'before_sha256': hashlib.sha256(originals[name]).hexdigest(),
                        'after_sha256': hashlib.sha256(after).hexdigest()})
    return records
