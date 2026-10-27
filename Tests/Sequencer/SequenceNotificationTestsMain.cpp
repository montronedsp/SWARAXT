// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>
#include "Engine/SequenceState.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

namespace {
using State = swaraxt::SequenceState;
using Boundary = State::NotificationPoint;
int failures = 0;

void expect(bool condition, const char* message)
{
    if (! condition) { ++failures; std::printf("FAIL: %s\n", message); }
}

void wait(juce::WaitableEvent& event)
{
    expect(event.wait(3000), "controlled interleaving completes");
}

void dispatch()
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
}

struct Recording : State::Listener {
    State& state;
    int calls = 0, last = -1, depth = 0, maxDepth = 0;
    bool messageThread = true;
    std::function<void()> first;
    explicit Recording(State& source) : state(source) {}
    void sequenceStateChanged() override
    {
        ++calls;
        ++depth;
        maxDepth = juce::jmax(maxDepth, depth);
        messageThread = messageThread && juce::MessageManager::getInstance()->isThisTheMessageThread();
        last = state.snapshot().scaleDegree[0];
        if (calls == 1 && first) first();
        --depth;
    }
};

void expectSettled(State& state, const Recording& listener, int generation)
{
    dispatch();
    expect(listener.last == generation, "last worker publication reaches the listener without another store");
    expect(listener.messageThread && listener.maxDepth == 1, "delivery is message-thread-only and nonrecursive");
    expect(! state.notificationArmedForTests() && ! state.notificationPendingForTests(),
           "finite publication burst leaves no stranded handoff flags");
}

void queuedAndConcurrentProducers()
{
    State state;
    Recording listener(state);
    state.addListener(&listener);
    std::array<std::thread, 4> producers;
    juce::WaitableEvent start(true);
    for (size_t i = 0; i < producers.size(); ++i)
        producers[i] = std::thread([&, i] {
            if (! start.wait(3000)) return;
            for (int n = 0; n != 4; ++n)
                state.setScaleDegree(0, static_cast<int16_t>(1 + static_cast<int>(i) * 4 + n));
        });
    start.signal();
    for (auto& producer : producers) producer.join();
    expect(listener.calls == 0, "worker publications never call listeners inline");
    expect(state.notifyStatsForTests().asyncSchedules == 1, "concurrent producers coalesce into one queued drain");
    const int finalGeneration = state.snapshot().scaleDegree[0];
    expectSettled(state, listener, finalGeneration);
    expect(listener.calls == 1, "one queued drain sees the final generation");
    state.removeListener(&listener);
}

struct ExitPublication {
    Boundary point;
    juce::WaitableEvent start, done;
    bool used = false;
    bool timedOut = false;
    explicit ExitPublication(Boundary selected) : point(selected) {}
    static void hook(void* context, Boundary point) noexcept
    {
        auto& self = *static_cast<ExitPublication*>(context);
        if (point != self.point || self.used) return;
        self.used = true;
        self.start.signal();
        self.timedOut = ! self.done.wait(3000);
    }
};

void publicationAtDrainExit(Boundary point)
{
    State state;
    Recording listener(state);
    state.addListener(&listener);
    ExitPublication schedule(point);
    state.setNotificationHook(ExitPublication::hook, &schedule);
    std::thread producer([&] {
        if (schedule.start.wait(3000)) state.setScaleDegree(0, 2);
        schedule.done.signal();
    });
    state.setScaleDegree(0, 1);
    producer.join();
    expect(schedule.used && ! schedule.timedOut, "publication overlaps the selected drain exit boundary");
    expectSettled(state, listener, 2);
    expect(listener.calls == 2, "exit overlap delivers old and new generations once each");
    const auto schedules = state.notifyStatsForTests().asyncSchedules;
    expect(schedules == (point == Boundary::delivered ? 0u : 1u),
           "an armed drain continues inline; a disarmed drain yields to the queued owner");
    state.setNotificationHook(nullptr, nullptr);
    state.removeListener(&listener);
}

void publicationDuringListener(bool reentrant)
{
    State state;
    Recording listener(state);
    state.addListener(&listener);
    juce::WaitableEvent start, done;
    std::thread producer;
    if (reentrant)
        listener.first = [&] { state.setScaleDegree(0, 2); };
    else
    {
        producer = std::thread([&] {
            if (start.wait(3000)) state.setScaleDegree(0, 2);
            done.signal();
        });
        listener.first = [&] { start.signal(); wait(done); };
    }
    state.setScaleDegree(0, 1);
    if (producer.joinable()) producer.join();
    expectSettled(state, listener, 2);
    expect(listener.calls == 2 && state.notifyStatsForTests().asyncSchedules == 0,
           "publication during callback is drained without recursive delivery or a second job");
    state.removeListener(&listener);
}

void publicationBeforeScheduling()
{
    State state;
    Recording listener(state);
    state.addListener(&listener);
    struct Pause {
        std::atomic<bool> first { true }, timedOut { false };
        juce::WaitableEvent published, resume;
        static void hook(void* context, Boundary point) noexcept
        {
            auto& self = *static_cast<Pause*>(context);
            if (point != Boundary::published || ! self.first.exchange(false)) return;
            self.published.signal();
            self.timedOut.store(! self.resume.wait(3000));
        }
    } pause;
    state.setNotificationHook(Pause::hook, &pause);
    std::thread producer([&] { state.setScaleDegree(0, 1); });
    wait(pause.published);
    state.setScaleDegree(0, 2);
    expect(listener.last == 2, "a second producer can deliver before the first producer acquires scheduling ownership");
    pause.resume.signal();
    producer.join();
    expect(! pause.timedOut.load(), "paused publication resumes");
    expectSettled(state, listener, 2);
    expect(state.notifyStatsForTests().asyncSchedules == 1, "resumed producer queues only one bounded follow-up");
    state.setNotificationHook(nullptr, nullptr);
    state.removeListener(&listener);
}

void failedPostAndRecovery()
{
    State state;
    Recording listener(state);
    state.addListener(&listener);
    state.rejectNotificationPostsForTests(true);
    std::thread failed([&] { state.setScaleDegree(0, 1); });
    failed.join();
    dispatch();
    expect(listener.calls == 0 && state.notificationPendingForTests() && ! state.notificationArmedForTests(),
           "failed dispatch leaves dirty state retryable, without pretending delivery succeeded");
    state.rejectNotificationPostsForTests(false);
    std::thread retry([&] { state.setScaleDegree(0, 2); });
    retry.join();
    expectSettled(state, listener, 2);
    expect(listener.calls == 1, "next successful publication recovers from dispatcher rejection");
    state.removeListener(&listener);
}

void listenerAndOwnerLifetime()
{
    State state;
    Recording listener(state);
    state.addListener(&listener);
    std::thread removed([&] { state.setScaleDegree(0, 1); });
    removed.join();
    state.removeListener(&listener);
    dispatch();
    expect(listener.calls == 0, "listener removal invalidates a queued delivery");
    state.addListener(&listener);
    state.setScaleDegree(0, 2);
    expect(listener.last == 2 && listener.calls == 1, "new listener lifetime receives subsequent publications");
    state.removeListener(&listener);

    auto owner = std::make_unique<State>();
    Recording destroyed(*owner);
    owner->addListener(&destroyed);
    std::thread queued([&] { owner->setScaleDegree(0, 3); });
    queued.join();
    owner.reset();
    dispatch();
    expect(destroyed.calls == 0, "independently owned hub never invokes a listener after owner destruction");

    struct SelfRemoving : State::Listener {
        State& source;
        int calls = 0;
        explicit SelfRemoving(State& value) : source(value) {}
        void sequenceStateChanged() override { ++calls; source.removeListener(this); }
    } self(state);
    state.addListener(&self);
    state.setScaleDegree(0, 4);
    state.setScaleDegree(0, 5);
    expect(self.calls == 1, "reentrant removal is safe and prevents subsequent callbacks");
}
} // namespace

int main()
{
    // A message dispatcher is required; no window, editor or chooser is created.
    juce::ScopedJuceInitialiser_GUI juce;
    queuedAndConcurrentProducers();
    publicationAtDrainExit(Boundary::delivered);
    publicationAtDrainExit(Boundary::disarmed);
    publicationDuringListener(false);
    publicationDuringListener(true);
    publicationBeforeScheduling();
    failedPostAndRecovery();
    listenerAndOwnerLifetime();
    std::printf("Sequence notification contracts: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
