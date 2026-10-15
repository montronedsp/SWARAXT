// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>
#include "Plugin/PluginEditor.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__linux__)
extern "C" void swaraxtBeginAllocationAudit() __attribute__((weak));
extern "C" unsigned long swaraxtEndAllocationAudit() __attribute__((weak));
#endif

namespace {
using Selection = swaraxt::ExperimentalSvfSelection;
using swaraxt::ShruthiSvf2pMode;
using swaraxt::ShruthiSvfDualRouting;
using swaraxt::ShruthiSvfCalibration;
int failures = 0;

void expect(bool ok, const char* message)
{
    if (! ok) { ++failures; std::printf("FAIL: %s\n", message); }
}

void pump(int milliseconds = 2)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(milliseconds);
}

void click(juce::Button& button)
{
    button.triggerClick();
    auto dispatched = std::make_shared<bool>(false);
    juce::MessageManager::callAsync([dispatched] { *dispatched = true; });
    for (int attempt = 0; attempt < 100 && ! *dispatched; ++attempt) pump(10);
    expect(*dispatched, "queued button click dispatched before assertion");
}

bool equal(const Selection& a, const Selection& b)
{
    return Selection::pack(a) == Selection::pack(b);
}

bool finite(const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite(buffer.getSample(ch, i))) return false;
    return true;
}

void process(SwaraXtAudioProcessor& processor)
{
    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
    expect(finite(buffer), "real processBlock output finite");
}

// Only read DSP after processing has returned, with no audio worker running.
void checkApplied(SwaraXtAudioProcessor& processor, const Selection& wanted)
{
    const auto& dual = processor.engineForTests().experimentalSvfDualForTests();
    expect(dual.routing() == wanted.routing && dual.mode1() == wanted.mode1
        && dual.mode2() == wanted.mode2 && dual.calibration() == wanted.calibration,
        "requested configuration applied by audio processing");
}

bool menuChecked(const juce::PopupMenu& menu, int id)
{
    for (juce::PopupMenu::MenuItemIterator it(menu, true); it.next();)
        if (it.getItem().itemID == id) return it.getItem().isTicked;
    return false;
}

void selectionMatrix()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    SwaraXtAudioProcessorEditor editor(processor);
    auto& main = editor.mainPanelForTests();
    auto& panel = main.dualSvfPanelForTests();
    auto calibrationPublications = std::make_shared<int>(0);
    const auto publishCalibration = panel.calibrationForTests().onSelect;
    panel.calibrationForTests().onSelect = [calibrationPublications, publishCalibration](int index) {
        ++*calibrationPublications;
        publishCalibration(index);
    };
    expect(! processor.experimentalSvfSelection().enabled, "fresh processor uses Classic");
    expect(main.filterModelForTests().getSelectedId() == 1, "fresh visible selector uses Classic");
    click(main.dualSvfViewButtonForTests());
    expect(panel.isVisible(), "visible detail affordance opens panel");
    expect(! panel.calibrationForTests().isEnabled(), "Classic subordinates Dual controls");
    const char* ids[] = { swaraxt::IDs::filterCutoff, swaraxt::IDs::filterResonance,
        swaraxt::IDs::filterEnvDepth, swaraxt::IDs::filterKeyTracking, swaraxt::IDs::filterLfoDepth };
    std::array<float, 5> common {};
    for (size_t i = 0; i < common.size(); ++i)
        common[i] = processor.getApvts().getRawParameterValue(ids[i])->load();
    Selection expected;
    int count = 0;
    for (int enabled = 0; enabled < 2; ++enabled)
        for (int route = 0; route < 3; ++route)
            for (int m1 = 0; m1 < 3; ++m1)
                for (int m2 = 0; m2 < 3; ++m2)
                    for (int calibration = 0; calibration < 2; ++calibration)
                    {
                        main.filterModelForTests().setSelectedId(2, juce::sendNotificationSync);
                        expected.enabled = true;
                        expect(equal(expected, processor.experimentalSvfSelection()), "enable preserves other fields");
                        click(panel.routingForTests().buttonForTests(route));
                        expected.routing = static_cast<ShruthiSvfDualRouting>(route);
                        expect(equal(expected, processor.experimentalSvfSelection()), "routing changes one field");
                        click(panel.mode1ForTests().buttonForTests(m1));
                        expected.mode1 = static_cast<ShruthiSvf2pMode>(m1);
                        expect(equal(expected, processor.experimentalSvfSelection()), "mode1 changes one field");
                        click(panel.mode2ForTests().buttonForTests(m2));
                        expected.mode2 = static_cast<ShruthiSvf2pMode>(m2);
                        expect(equal(expected, processor.experimentalSvfSelection()), "mode2 changes one field");
                        const int beforeClick = *calibrationPublications;
                        click(panel.calibrationForTests().buttonForTests(calibration));
                        expect(*calibrationPublications == beforeClick + 1, "one calibration publication per real radio-button click");
                        expected.calibration = static_cast<ShruthiSvfCalibration>(calibration);
                        expect(equal(expected, processor.experimentalSvfSelection()), "calibration changes one field");
                        main.filterModelForTests().setSelectedId(enabled + 1, juce::sendNotificationSync);
                        expected.enabled = enabled != 0;
                        const auto requested = processor.experimentalSvfSelection();
                        expect(equal(expected, requested), "complete requested configuration");
                        expect(equal(Selection::unpack(Selection::pack(requested)), requested), "pack/unpack roundtrip");
                        expect(panel.routingForTests().selectedIndex() == route
                            && panel.mode1ForTests().selectedIndex() == m1
                            && panel.mode2ForTests().selectedIndex() == m2
                            && panel.calibrationForTests().selectedIndex() == calibration,
                            "actual GUI selected states match coherent request");
                        expect(panel.requestedTextForTests().contains(enabled ? "DUAL SVF" : "CLASSIC IR3109"),
                               "display identifies requested model");
                        expect(panel.section2TextForTests().contains(route == 0 ? "outside" : "Shared"),
                               "reference routing identifies unused section2");
                        const auto menu = editor.experimentalFilterMenuForTests();
                        expect(menuChecked(menu, enabled ? 601 : 600), "visible selector updates menu model checkmark");
                        if (enabled)
                            expect(menuChecked(menu, 630 + route) && menuChecked(menu, 610 + m1)
                                && menuChecked(menu, 620 + m2) && menuChecked(menu, 640 + calibration),
                                "visible controls update all menu checkmarks");
                        const auto beforeRefresh = Selection::pack(requested);
                        editor.refreshExperimentalSvfForTests();
                        editor.refreshExperimentalSvfForTests();
                        expect(Selection::pack(processor.experimentalSvfSelection()) == beforeRefresh,
                               "refresh does not change published fields");
                        process(processor);
                        checkApplied(processor, expected);
                        ++count;
                    }
    expect(count == 108, "all 108 valid configurations exercised");
    for (size_t i = 0; i < common.size(); ++i)
        expect(common[i] == processor.getApvts().getRawParameterValue(ids[i])->load(), "common filter values preserved");
    const auto menuAction = editor.contextMenuCallbackForTests();
    for (int id : { 601, 631, 611, 622, 641, 640, 632, 630, 610, 612, 620, 621, 600 })
    {
        menuAction(id);
        const auto s = processor.experimentalSvfSelection();
        expect(main.filterModelForTests().getSelectedId() == (s.enabled ? 2 : 1)
            && panel.routingForTests().selectedIndex() == static_cast<int>(s.routing)
            && panel.mode1ForTests().selectedIndex() == static_cast<int>(s.mode1)
            && panel.mode2ForTests().selectedIndex() == static_cast<int>(s.mode2)
            && panel.calibrationForTests().selectedIndex() == static_cast<int>(s.calibration),
            "actual context-menu callback synchronizes visible controls");
    }
    processor.setExperimentalSvfEnabled(true);
    processor.setExperimentalSvfCalibration(ShruthiSvfCalibration::eagleHardware);
    pump(300);
    expect(main.filterModelForTests().getSelectedId() == 2 && panel.calibrationForTests().selectedIndex() == 1,
           "existing editor timer reflects external processor request");
    std::printf("Selection matrix: %d configurations, real controls + processBlock\n", count);
}

void calibrationAndSession()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    juce::MemoryBlock classicState;
    processor.getStateInformation(classicState);
    SwaraXtAudioProcessorEditor editor(processor);
    auto& main = editor.mainPanelForTests();
    main.filterModelForTests().setSelectedId(2, juce::sendNotificationSync);
    main.setDualSvfView(true);
    auto& panel = main.dualSvfPanelForTests();
    click(panel.routingForTests().buttonForTests(1));
    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 0);
    processor.processBlock(buffer, midi);
    midi.clear();
    for (int i = 0; i < 24; ++i) processor.processBlock(buffer, midi);
    for (int calibration : { 1, 0, 1, 0 })
    {
        const auto before = processor.experimentalSvfSelection();
        const auto& dual = processor.engineForTests().experimentalSvfDualForTests();
        const double bp = dual.section1().lastBpVolts();
        expect(std::abs(bp) > 1.0e-9, "calibration switch has active filter state");
        click(panel.calibrationForTests().buttonForTests(calibration));
        auto expected = before;
        expected.calibration = static_cast<ShruthiSvfCalibration>(calibration);
        expect(equal(expected, processor.experimentalSvfSelection()), "both calibration directions preserve all other fields");
        expect(dual.section1().lastBpVolts() == bp, "GUI publication does not reset or mutate integrators");
        processor.processBlock(buffer, midi);
        checkApplied(processor, expected);
        expect(finite(buffer), "calibration switch during sustained bass finite");
    }
    juce::MemoryBlock dualState;
    processor.getStateInformation(dualState);
    expect(classicState == dualState, "experimental GUI selection adds no serialized data");
    const auto beforeRestore = processor.experimentalSvfSelection();
    processor.setStateInformation(classicState.getData(), static_cast<int>(classicState.getSize()));
    expect(equal(beforeRestore, processor.experimentalSvfSelection()), "session restoration preserves session-only selection");
    processor.setCurrentProgram(1);
    expect(equal(beforeRestore, processor.experimentalSvfSelection()), "factory preset does not overwrite experimental session choice");
    SwaraXtAudioProcessor fresh;
    fresh.setStateInformation(dualState.getData(), static_cast<int>(dualState.getSize()));
    expect(! fresh.experimentalSvfSelection().enabled, "saved state cannot enable Dual in fresh instance");
    std::printf("Calibration + session compatibility: executed\n");
}

void concurrentProcessing()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    SwaraXtAudioProcessorEditor editor(processor);
    auto& main = editor.mainPanelForTests();
    main.filterModelForTests().setSelectedId(2, juce::sendNotificationSync);
    main.setDualSvfView(true);
    std::atomic<int> bad { 0 }, blocks { 0 };
    std::atomic<bool> start { false };
    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;
        midi.ensureSize(128);
        const char* automationIds[] = { swaraxt::IDs::filterCutoff, swaraxt::IDs::filterResonance,
            swaraxt::IDs::filterEnvDepth, swaraxt::IDs::filterKeyTracking, swaraxt::IDs::filterLfoDepth,
            swaraxt::IDs::mixBalance };
        std::array<juce::RangedAudioParameter*, 6> automation {};
        for (size_t i = 0; i < automation.size(); ++i)
            automation[i] = processor.getApvts().getParameter(automationIds[i]);
        while (! start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 1200; ++i)
        {
            midi.clear();
            if (i == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 0);
            for (size_t parameter = 0; parameter < automation.size(); ++parameter)
                automation[parameter]->setValueNotifyingHost(static_cast<float>((i * (static_cast<int>(parameter) + 3)) % 101) / 100.0f);
            processor.processBlock(buffer, midi);
            if (! finite(buffer)) ++bad;
            const auto s = processor.experimentalSvfSelection();
            if (static_cast<int>(s.routing) > 2 || static_cast<int>(s.mode1) > 2
                || static_cast<int>(s.mode2) > 2 || static_cast<int>(s.calibration) > 1) ++bad;
            ++blocks;
        }
    });
    std::thread writer([&] {
        while (! start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 12000; ++i)
        {
            processor.setExperimentalSvfMode2(static_cast<ShruthiSvf2pMode>(i % 3));
            processor.setExperimentalSvfCalibration(static_cast<ShruthiSvfCalibration>(i % 2));
        }
    });
    start.store(true, std::memory_order_release);
    for (int i = 0; i < 90; ++i)
    {
        auto& panel = main.dualSvfPanelForTests();
        main.filterModelForTests().setSelectedId(2, juce::sendNotificationSync);
        click(panel.routingForTests().buttonForTests(i % 3));
        click(panel.mode1ForTests().buttonForTests(i % 3));
        click(panel.calibrationForTests().buttonForTests(i % 2));
        main.filterModelForTests().setSelectedId((i % 2) + 1, juce::sendNotificationSync);
        if (i % 10 == 0)
        {
            main.setDualSvfView(false);
            editor.setGuiSizeForTests(i % 20 == 0 ? swaraxt::ui::GuiSize::small : swaraxt::ui::GuiSize::large);
            main.setDualSvfView(true);
        }
    }
    writer.join();
    audio.join();
    expect(bad.load() == 0 && blocks.load() == 1200, "actual GUI + second publisher concurrent with 1200 processBlocks");
    process(processor);
    checkApplied(processor, processor.experimentalSvfSelection());
    std::printf("Concurrent GUI/control processing: %d blocks, 7200 host parameter updates, bad=%d\n", blocks.load(), bad.load());
}

void coherentPublication()
{
    swaraxt::ExperimentalSvfControl control;
    Selection a, b;
    a.enabled = true;
    a.routing = ShruthiSvfDualRouting::serial;
    a.mode1 = ShruthiSvf2pMode::bandPass;
    a.mode2 = ShruthiSvf2pMode::highPass;
    a.calibration = ShruthiSvfCalibration::eagleHardware;
    b.routing = ShruthiSvfDualRouting::parallel;
    b.mode1 = ShruthiSvf2pMode::highPass;
    b.mode2 = ShruthiSvf2pMode::bandPass;
    control.request(a);
    std::atomic<int> bad { 0 };
    std::thread reader([&] {
        swaraxt::ShruthiSvfDual dual;
        dual.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
        bool enabled = false;
        for (int i = 0; i < 40000; ++i)
        {
            const auto s = control.selection();
            if (! equal(s, a) && ! equal(s, b)) ++bad;
            control.applyOnAudioThread(dual, enabled);
            if (! std::isfinite(dual.process(0.2f, 0.7f))) ++bad;
        }
    });
    for (int i = 0; i < 40000; ++i) control.request(i % 2 == 0 ? b : a);
    reader.join();
    expect(bad.load() == 0, "correlated packed requests never tear while audio applies and renders");
    std::thread modes([&] {
        for (int i = 0; i < 10000; ++i)
            control.patch([](Selection& s) { s.mode1 = ShruthiSvf2pMode::highPass; s.mode2 = ShruthiSvf2pMode::bandPass; });
    });
    std::thread topology([&] {
        for (int i = 0; i < 10000; ++i)
            control.patch([](Selection& s) { s.enabled = true; s.routing = ShruthiSvfDualRouting::parallel;
                                           s.calibration = ShruthiSvfCalibration::eagleHardware; });
    });
    modes.join();
    topology.join();
    auto expected = b;
    expected.enabled = true;
    expected.calibration = ShruthiSvfCalibration::eagleHardware;
    expect(equal(control.selection(), expected), "CAS patches from independent writers preserve all final fields");
    std::printf("Packed publication: 40000 correlated reads/applies + two CAS writers\n");
}

void allocationAudit()
{
#if defined(__linux__)
    // Optional external malloc interposer; sanitizer runs retain their own interceptors.
    if (swaraxtBeginAllocationAudit == nullptr || swaraxtEndAllocationAudit == nullptr)
    {
        std::printf("Audio heap audit: external interposer not loaded\n");
        return;
    }
    swaraxtBeginAllocationAudit();
    auto* proof = static_cast<char*>(std::malloc(17));
    if (proof != nullptr) *static_cast<volatile char*>(proof) = 1;
    const auto probeCount = swaraxtEndAllocationAudit();
    std::free(proof);
    expect(probeCount > 0, "external heap audit detects a deliberate allocation");
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    SwaraXtAudioProcessorEditor editor(processor);
    auto& main = editor.mainPanelForTests();
    main.setDualSvfView(true);
    main.filterModelForTests().setSelectedId(2, juce::sendNotificationSync);
    const int latency = processor.getLatencySamples();
    auto* resonance = processor.getApvts().getParameter(swaraxt::IDs::filterResonance);
    resonance->setValueNotifyingHost(1.0f);
    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    midi.ensureSize(128);
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 0);
    processor.processBlock(buffer, midi);
    midi.clear();
    unsigned long allocations = 0;
    for (int i = 0; i < 180; ++i)
    {
        auto& panel = main.dualSvfPanelForTests();
        click(panel.routingForTests().buttonForTests(i % 3));
        click(panel.mode1ForTests().buttonForTests((i / 3) % 3));
        click(panel.mode2ForTests().buttonForTests((i / 9) % 3));
        click(panel.calibrationForTests().buttonForTests(i % 2));
        processor.getApvts().getParameter(swaraxt::IDs::filterCutoff)->setValueNotifyingHost(static_cast<float>(i % 101) / 100.0f);
        processor.getApvts().getParameter(swaraxt::IDs::filterEnvDepth)->setValueNotifyingHost(static_cast<float>((i * 3) % 101) / 100.0f);
        swaraxtBeginAllocationAudit();
        processor.processBlock(buffer, midi);
        allocations += swaraxtEndAllocationAudit();
        expect(finite(buffer), "maximum-resonance switching remains finite");
    }
    expect(allocations == 0, "zero interposed heap allocations in real audio callback during switching");
    expect(processor.getLatencySamples() == latency, "GUI changes preserve host latency");
    std::printf("Audio heap audit: probe=%lu, 180 switched processBlocks, allocations=%lu, latency=%d\n", probeCount, allocations, latency);
#endif
}

void layoutAndLifetime()
{
    auto processor = std::make_unique<SwaraXtAudioProcessor>();
    processor->prepareToPlay(48000.0, 256);
    processor->setExperimentalSvfEnabled(true);
    std::atomic<bool> stopAudio { false };
    std::atomic<int> rendered { 0 }, invalid { 0 };
    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;
        midi.ensureSize(128);
        midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 0);
        for (int i = 0; i < 20000 && ! stopAudio.load(std::memory_order_acquire); ++i)
        {
            processor->processBlock(buffer, midi);
            midi.clear();
            if (! finite(buffer)) ++invalid;
            ++rendered;
        }
    });
    std::function<void(int)> pending;
    for (int cycle = 0; cycle < 12; ++cycle)
    {
        auto editor = std::make_unique<SwaraXtAudioProcessorEditor>(*processor);
        auto& main = editor->mainPanelForTests();
        click(main.dualSvfViewButtonForTests());
        click(main.dualSvfPanelForTests().calibrationForTests().buttonForTests(cycle % 2));
        expect(main.filterModelForTests().getSelectedId() == 2, "editor reopening reflects processor selection");
        pending = editor->contextMenuCallbackForTests();
        main.dualSvfPanelForTests().calibrationForTests().buttonForTests(0).triggerClick();
        if (cycle == 0)
        {
            editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
            editor->setVisible(true);
            editor->showContextMenuForTests();
        }
        editor.reset();
        const auto before = processor->experimentalSvfSelection();
        pending(641);
        pump(20);
        expect(equal(before, processor->experimentalSvfSelection()), "destroyed editor cancels pending menu and button callbacks");
    }
    stopAudio.store(true, std::memory_order_release);
    audio.join();
    expect(rendered.load() > 0 && invalid.load() == 0, "editor and popup destruction while actual audio processing continues");
    std::printf("Audio during editor lifecycle: blocks=%d, invalid=%d\n", rendered.load(), invalid.load());
    processor.reset();
    pending(601);
    pump(20);

    SwaraXtAudioProcessor displayProcessor;
    displayProcessor.setExperimentalSvfEnabled(true);
    displayProcessor.setExperimentalSvfRouting(ShruthiSvfDualRouting::parallel);
    displayProcessor.setExperimentalSvfMode2(ShruthiSvf2pMode::bandPass);
    displayProcessor.setExperimentalSvfCalibration(ShruthiSvfCalibration::eagleHardware);
    SwaraXtAudioProcessorEditor editor(displayProcessor);
    auto& main = editor.mainPanelForTests();
    main.setDualSvfView(true);
    const auto screenshots = juce::File::getCurrentWorkingDirectory().getChildFile("screenshots");
    expect(screenshots.createDirectory().wasOk(), "screenshot evidence directory");
    int images = 0;
    for (auto skin : { swaraxt::ui::SkinId::pastel, swaraxt::ui::SkinId::midnightGold,
                      swaraxt::ui::SkinId::neonCobalt, swaraxt::ui::SkinId::jungle, swaraxt::ui::SkinId::rossocorsa })
        for (auto size : { swaraxt::ui::GuiSize::small, swaraxt::ui::GuiSize::medium, swaraxt::ui::GuiSize::large })
        {
            editor.setSkinForTests(skin);
            editor.setGuiSizeForTests(size);
            editor.resized();
            auto& panel = main.dualSvfPanelForTests();
            for (int i = 0; i < panel.getNumChildComponents(); ++i)
            {
                const auto* child = panel.getChildComponent(i);
                expect(panel.getLocalBounds().contains(child->getBounds()) && ! child->getBounds().isEmpty(),
                       "all detail controls have area inside panel");
            }
            expect(editor.getLocalBounds().contains(editor.getLocalArea(&panel, panel.getLocalBounds())),
                   "detail surface fits scaled editor");
            const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
            const auto file = screenshots.getChildFile("skin-" + juce::String(static_cast<int>(skin))
                + "-size-" + juce::String(static_cast<int>(size)) + ".png");
            if (auto output = file.createOutputStream())
                expect(juce::PNGImageFormat().writeImageToStream(image, *output), "real editor snapshot written");
            else expect(false, "open screenshot output");
            ++images;
            click(panel.closeForTests());
            expect(! panel.isVisible(), "close button hides detail surface");
            click(main.dualSvfViewButtonForTests());
            expect(panel.isVisible(), "detail surface reopens after scaling/skin change");
        }
    std::printf("Lifecycle: 12 editor cycles, pending menu/child callbacks; screenshots=%d\n", images);
    editor.setSkinForTests(swaraxt::ui::SkinId::pastel);
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::medium);
    const auto saveStateImage = [&](const char* name) {
        const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
        auto output = screenshots.getChildFile(name).createOutputStream();
        expect(output != nullptr && juce::PNGImageFormat().writeImageToStream(image, *output), "requested-state editor snapshot");
    };
    main.filterModelForTests().setSelectedId(1, juce::sendNotificationSync);
    saveStateImage("classic-detail.png");
    main.setDualSvfView(false);
    saveStateImage("classic-main.png");
    main.filterModelForTests().setSelectedId(2, juce::sendNotificationSync);
    main.setDualSvfView(true);
    click(main.dualSvfPanelForTests().routingForTests().buttonForTests(0));
    saveStateImage("section1-eagle.png");
    click(main.dualSvfPanelForTests().routingForTests().buttonForTests(1));
    click(main.dualSvfPanelForTests().calibrationForTests().buttonForTests(0));
    saveStateImage("serial-paper.png");
    expect(main.dualSvfPanelForTests().keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)), "Escape closes detail surface");
    expect(! main.dualSvfPanelForTests().isVisible(), "Escape leaves detail surface hidden");
}
}

#if JUCE_LINUX
#include <dlfcn.h>

// Default Xvfb resets when its last client disconnects, so ICCCM atoms do not
// survive a separate process. JUCE stores WM_PROTOCOLS with
// XInternAtom(..., only_if_exists) and then passes that atom to
// XChangeProperty. This console test is not a JUCEApplication, so JUCE does
// not install its X error handler and the default handler aborts on BadAtom.
// One connection is kept open so the server does not reset before JUCE opens
// its own display.
void keepResettingXvfbIcccmAtomsAlive()
{
    const char* displayName = std::getenv("DISPLAY");
    if (displayName == nullptr || displayName[0] == '\0')
        return;

    void* library = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
    if (library == nullptr)
        return;

    using OpenDisplay = void* (*)(const char*);
    using InternAtom = unsigned long (*)(void*, const char*, int);
    using FlushDisplay = int (*)(void*);
    auto openDisplay = reinterpret_cast<OpenDisplay>(dlsym(library, "XOpenDisplay"));
    auto internAtom = reinterpret_cast<InternAtom>(dlsym(library, "XInternAtom"));
    auto flushDisplay = reinterpret_cast<FlushDisplay>(dlsym(library, "XFlush"));
    if (openDisplay == nullptr || internAtom == nullptr || flushDisplay == nullptr)
        return;

    static void* heldDisplay = nullptr;
    heldDisplay = openDisplay(displayName);
    if (heldDisplay == nullptr)
        return;

    static const char* names[] = {
        "WM_PROTOCOLS",
        "WM_TAKE_FOCUS",
        "WM_DELETE_WINDOW",
        "_NET_WM_PING",
        "WM_CHANGE_STATE",
        "WM_STATE",
        "_NET_WM_WINDOW_TYPE",
        "_NET_WM_STATE",
        "_NET_WM_STATE_HIDDEN",
        "_NET_WM_STATE_MAXIMIZED_HORZ",
        "_NET_WM_STATE_MAXIMIZED_VERT",
    };
    for (const char* name : names)
        internAtom(heldDisplay, name, 0);
    flushDisplay(heldDisplay);
}
#endif

int main()
{
#if JUCE_LINUX
    keepResettingXvfbIcccmAtomsAlive();
#endif
    juce::ScopedJuceInitialiser_GUI juce;
    selectionMatrix();
    calibrationAndSession();
    concurrentProcessing();
    coherentPublication();
    allocationAudit();
    layoutAndLifetime();
    std::printf("Dual SVF GUI failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
