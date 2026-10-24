// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// GenMC v0.12.0 / LLVM 18 model of slot ownership; ordinary payload accesses
// remain ordinary. Defaults include constructor initialization, one earlier
// publication, one callback and two successive publications, with real scans
// and retry bounds. QUERY adds claim readers; AUDIO_CALLBACKS, PUBLICATIONS,
// PREPUBLISH and WRITERS adjust finite exploration bounds.
// genmc --rc11 --disable-mm-detector --disable-estimation -- -DFIX_ACTIVE=0 TuningOwnershipModel.c
// must report a non-atomic race. FIX_ACTIVE=1 must have no safety violation.
// RC11 detects the original race. The SC interpretation is only a control.
// GenMC v0.12.0's IMM checkErrors does not check non-atomic races: an IMM zero
// exit is not ownership evidence. No dependency is added to normal builds.
// FIX_ACTIVE=1 also defaults to constructor-only initialization. Override
// CTOR_INIT_ONLY=0 with INITIAL_VALID=0 to check the old late-init bypass.
// SLOTS=4 PUBLICATIONS=3 is a negative control: the bridge requires three slots.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#ifndef FIX_ACTIVE
#define FIX_ACTIVE 0
#endif
#ifndef CTOR_INIT_ONLY
#define CTOR_INIT_ONLY FIX_ACTIVE
#endif
#ifndef QUERY
#define QUERY 0
#endif
#ifndef PUBLICATIONS
#define PUBLICATIONS 2
#endif
#ifndef AUDIO_CALLBACKS
#define AUDIO_CALLBACKS 1
#endif
#ifndef AUDIO
#define AUDIO 1
#endif
#ifndef PREPUBLISH
#define PREPUBLISH 1
#endif
#ifndef WRITERS
#define WRITERS 1
#endif
#ifndef INITIAL_VALID
#define INITIAL_VALID 1
#endif
#ifndef SLOTS
#define SLOTS 3
#endif
#define S memory_order_seq_cst
struct Slot { int generation, valid; };
static struct Slot slots[SLOTS];
static struct Slot stable_copy;
static _Atomic int published, latched, retired;
static _Atomic int claims[2] = {-1,-1};
static _Atomic _Bool active, busy;
static int claimed(int slot) {
    for (int i=0; i<2; ++i) if (atomic_load_explicit(&claims[i],S)==slot) return 1;
    return 0;
}
static int free_slot(int p,int l,int r) {
    for(int i=0;i<SLOTS;++i) if(i!=p && i!=l && i!=r && !claimed(i)) return i;
    return -1;
}
static void publish(int generation) {
    if(atomic_exchange_explicit(&busy,1,memory_order_acq_rel)) return;
#if !CTOR_INIT_ONLY
    if(!slots[0].valid) {
        slots[0].generation=generation; slots[0].valid=1;
        atomic_store_explicit(&published,0,S);
        atomic_store_explicit(&retired,0,S);
        atomic_store_explicit(&latched,0,S);
        stable_copy.generation=slots[0].generation;
        stable_copy.valid=slots[0].valid;
        atomic_store_explicit(&busy,0,memory_order_release);
        return;
    }
#endif
    int slot=-1,p=0;
    for(int attempt=0;attempt<8 && slot<0;++attempt) {
        if(!atomic_load_explicit(&active,S)) {
            int current=atomic_load_explicit(&published,S);
            atomic_store_explicit(&retired,current,S);
        }
        p=atomic_load_explicit(&published,S);
        int l=atomic_load_explicit(&latched,S), r=atomic_load_explicit(&retired,S);
        int candidate=free_slot(p,l,r);
        if(candidate<0) continue; /* Sleep has no C++ synchronization effect. */
        int pn=atomic_load_explicit(&published,S), ln=atomic_load_explicit(&latched,S);
        int rn=atomic_load_explicit(&retired,S);
        if(candidate==pn || candidate==ln || candidate==rn || claimed(candidate)) continue;
        slot=candidate; p=pn;
    }
    if(slot<0) { atomic_store_explicit(&busy,0,memory_order_release); return; }
    if(slot==atomic_load_explicit(&published,S) || slot==atomic_load_explicit(&latched,S)
       || slot==atomic_load_explicit(&retired,S) || claimed(slot)) {
        atomic_store_explicit(&busy,0,memory_order_release); return;
    }
    slots[slot].generation=generation;
    slots[slot].valid=1;
    atomic_store_explicit(&retired,p,S);
    atomic_store_explicit(&published,slot,S);
    atomic_store_explicit(&busy,0,memory_order_release);
}
static void *writer(void *arg) {
    int first=(int)(long)arg;
    for(int i=0;i<PUBLICATIONS;++i) publish(first+i);
    return 0;
}
static void *audio(void *arg) {
    (void)arg;
    for(int callback=0;callback<AUDIO_CALLBACKS;++callback) {
    atomic_store_explicit(&active,1,FIX_ACTIVE ? S : memory_order_release);
    int slot=atomic_load_explicit(&published,S);
    atomic_store_explicit(&latched,slot,S);
    if(!slots[slot].valid) { slot=0; atomic_store_explicit(&latched,slot,S); }
    int latest=atomic_load_explicit(&published,S);
    if(latest!=slot) {
        atomic_store_explicit(&latched,latest,S);
        if(slots[latest].valid) slot=latest;
        else atomic_store_explicit(&latched,slot,S);
    }
    int generation=slots[slot].generation;
    int valid=slots[slot].valid;
    stable_copy.generation=generation;
    stable_copy.valid=valid;
    assert(generation>=0 && (!INITIAL_VALID || valid));
    atomic_store_explicit(&active,0,FIX_ACTIVE ? S : memory_order_release);
    }
    return 0;
}
#if QUERY
static void *query(void *arg) {
    (void)arg;
    for(int attempt=0;attempt<4;++attempt) {
        int slot=atomic_load_explicit(&published,S), index=-1;
        for(int i=0;i<2;++i) {
            int expected=-1;
            if(atomic_compare_exchange_strong_explicit(&claims[i],&expected,slot,S,memory_order_relaxed)) { index=i; break; }
        }
        if(index<0) return 0;
        int still=atomic_load_explicit(&published,S)==slot;
        if(!still || !slots[slot].valid) { atomic_store_explicit(&claims[index],-1,S); continue; }
        int generation=slots[slot].generation;
        assert(generation>=0);
        atomic_store_explicit(&claims[index],-1,memory_order_release);
        return 0;
    }
    return 0;
}
#endif
int main(void) {
    for(int i=0;i<2;++i) atomic_store_explicit(&claims[i],-1,memory_order_relaxed);
    if(INITIAL_VALID) {
        assert(!atomic_exchange_explicit(&busy,1,memory_order_acq_rel));
        slots[0].generation=0; slots[0].valid=1;
        atomic_store_explicit(&published,0,S);
        atomic_store_explicit(&retired,0,S);
        atomic_store_explicit(&latched,0,S);
        stable_copy.generation=slots[0].generation;
        stable_copy.valid=slots[0].valid;
        atomic_store_explicit(&busy,0,memory_order_release);
    }
    if(PREPUBLISH) publish(1); /* Published 1, preceding audio latch 0, retired 0. */
    pthread_t a,w[WRITERS];
#if AUDIO
    pthread_create(&a,0,audio,0);
#endif
    for(int i=0;i<WRITERS;++i) pthread_create(&w[i],0,writer,(void*)(long)(2+i*PUBLICATIONS));
#if QUERY
    pthread_t q[QUERY];
    for(int i=0;i<QUERY;++i) pthread_create(&q[i],0,query,0);
#endif
#if AUDIO
    pthread_join(a,0);
#endif
    for(int i=0;i<WRITERS;++i) pthread_join(w[i],0);
#if QUERY
    for(int i=0;i<QUERY;++i) pthread_join(q[i],0);
#endif
    return 0;
}
