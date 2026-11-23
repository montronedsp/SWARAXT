// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// GenMC C11-operation model of the C++17 sequence protocol. PROTOCOL=0 is
// original, 1 paired fences, 2 reader fence only, 3 one gate attempt. REV_MASK=3
// reduces the counter to two observed bits to enumerate rollover. Writer RMW
// serialization and constructor publication are retained in every variant.
// CHECK_CONTROL adds the independent host ARP scalar. CHECK_APPLY=1 models
// tag-only engine caching; 2 adds the complete payload comparison.
// Manual verification with GenMC v0.12.0 (LLVM 18); no normal build dependency.
// From this directory, the first command must find an assertion violation;
// the second and third must complete without a safety violation:
// genmc --rc11 --disable-estimation -- -DPROTOCOL=0 SequenceSnapshotModel.c
// genmc --rc11 --disable-estimation -- -DPROTOCOL=3 SequenceSnapshotModel.c
// genmc --imm --disable-estimation -- -DPROTOCOL=3 -DMULTI_WRITER=1 -DREV_MASK=3 -DCHECK_CONTROL=1 -DCHECK_APPLY=2 SequenceSnapshotModel.c
// REV_MASK abstracts observed tag reuse, not a change to payload ordering.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#ifndef PROTOCOL
#define PROTOCOL 0
#endif
#ifndef REV_MASK
#define REV_MASK UINT32_MAX
#endif
#ifndef CHECK_CONTROL
#define CHECK_CONTROL 0
#endif
#ifndef CHECK_APPLY
#define CHECK_APPLY 0
#endif
#ifndef MULTI_WRITER
#define MULTI_WRITER 0
#endif
static atomic_flag writer = ATOMIC_FLAG_INIT;
static _Atomic uint32_t revision;
static _Atomic int x, y;
#if CHECK_CONTROL
static _Atomic int control;
#endif
static void publish(int generation) {
    while (atomic_flag_test_and_set_explicit(&writer, memory_order_acquire)) {}
    atomic_fetch_add_explicit(&revision, 1, memory_order_acq_rel);
#if PROTOCOL == 1
    atomic_thread_fence(memory_order_release);
#endif
    atomic_store_explicit(&x, generation, memory_order_relaxed);
#if CHECK_CONTROL
    atomic_store_explicit(&control, generation, memory_order_relaxed);
#endif
    atomic_store_explicit(&y, generation, memory_order_relaxed);
    atomic_fetch_add_explicit(&revision, 1, memory_order_release);
    atomic_flag_clear_explicit(&writer, memory_order_release);
}
static void *producer(void *arg) {
    (void)arg;
    publish(1);
#if !MULTI_WRITER
    publish(2);
#endif
    return 0;
}
#if MULTI_WRITER
static void *other_producer(void *arg) { (void)arg; publish(2); return 0; }
#endif
#if CHECK_CONTROL
static void *automation(void *arg) {
    (void)arg; atomic_store_explicit(&control, 7, memory_order_release); return 0;
}
#endif
static void *consumer(void *arg) {
    (void)arg;
#if PROTOCOL == 3
    if (atomic_flag_test_and_set_explicit(&writer, memory_order_acquire)) return 0;
#else
    const uint32_t before = atomic_load_explicit(&revision, memory_order_acquire) & REV_MASK;
    if (before & 1) return 0;
#endif
    const int a = atomic_load_explicit(&x, memory_order_relaxed);
#if CHECK_CONTROL
    const int c = atomic_load_explicit(&control, memory_order_acquire);
#endif
    const int b = atomic_load_explicit(&y, memory_order_relaxed);
#if PROTOCOL == 1 || PROTOCOL == 2
    atomic_thread_fence(memory_order_acquire);
#endif
#if PROTOCOL == 3
    const uint32_t after = atomic_load_explicit(&revision, memory_order_relaxed) & REV_MASK;
    atomic_flag_clear_explicit(&writer, memory_order_release);
    (void)after;
#else
    const uint32_t after = atomic_load_explicit(&revision, memory_order_acquire) & REV_MASK;
    if (before != after || (after & 1)) return 0;
#endif
    assert(a == b);
#if CHECK_CONTROL
    assert(c == 7 || c == a);
#endif
#if CHECK_APPLY
    /* Engine starts with generation zero applied, whose revision is two. */
    int applied_x = 0;
    int applied_mode = 0;
    if (after != 2 || (CHECK_APPLY == 2 && (a != 0 || b != 0))) {
        applied_x = a;
        if (CHECK_APPLY == 2) applied_mode = b;
    }
    if (CHECK_APPLY == 1) applied_mode = b;
    assert(applied_x == applied_mode);
#endif
    return 0;
}
int main(void) {
    publish(0); /* Production constructor completes one publication before sharing. */
    pthread_t a,b;
    pthread_create(&a,0,producer,0);
#if MULTI_WRITER
    pthread_t c; pthread_create(&c,0,other_producer,0);
#endif
#if CHECK_CONTROL
    pthread_t d; pthread_create(&d,0,automation,0);
#endif
    pthread_create(&b,0,consumer,0);
    pthread_join(a,0); pthread_join(b,0);
#if CHECK_CONTROL
    pthread_join(d,0);
#endif
#if MULTI_WRITER
    pthread_join(c,0);
#endif
    return 0;
}
