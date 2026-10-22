// SPDX-FileCopyrightText: 2026 MontroneDSP
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if JUCE_LINUX
#include <atomic>
#include <cerrno>
#include <pthread.h>
#include <thread>

inline std::atomic<int> nativeMutexDestroyFailures { 0 };
extern "C" int __real_pthread_mutex_destroy (pthread_mutex_t*) noexcept;
extern "C" int __wrap_pthread_mutex_destroy (pthread_mutex_t* mutex) noexcept
{
    const int result = __real_pthread_mutex_destroy (mutex);
    if (result != 0)
    {
        ++nativeMutexDestroyFailures;
        std::fprintf (stderr, "FAIL: native mutex destruction returned %d\n", result);
    }
    return result;
}

inline int runNativeFrameworkProbes()
{
    std::atomic<int> invalidOwners { 0 };
    const auto exerciseLocks = [&]
    {
        const juce::MessageManagerLock outer;
        if (! outer.lockWasGained()) ++invalidOwners;
        {
            const juce::MessageManagerLock inner;
            if (! inner.lockWasGained()) ++invalidOwners;
        }
        if (! juce::MessageManager::existsAndIsLockedByCurrentThread()) ++invalidOwners;
    };
    exerciseLocks();
    std::atomic<bool> done { false };
    std::thread worker ([&] { exerciseLocks(); done = true; });
    const auto lockDeadline = juce::Time::getMillisecondCounter() + 10000;
    while (! done && juce::Time::getMillisecondCounter() < lockDeadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    if (! done) std::abort();
    worker.join();

    std::atomic<int> delivered { 0 };
    for (int i = 0; i < 512; ++i)
        juce::MessageManager::callAsync ([&] { juce::Thread::sleep (3); ++delivered; });
    const auto queueDeadline = juce::Time::getMillisecondCounter() + 10000;
    while (delivered < 512 && juce::Time::getMillisecondCounter() < queueDeadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    std::printf ("Framework probe: %d/512 callbacks; %d invalid owners; %d invalid mutex destructions\n",
                 delivered.load(), invalidOwners.load(), nativeMutexDestroyFailures.load());
    if (delivered != 512 || invalidOwners != 0 || nativeMutexDestroyFailures != 0) return 1;
    std::puts ("FRAMEWORK_LOCK_QUEUE_PASS");
    return 0;
}
#endif
