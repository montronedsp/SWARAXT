// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginEditor.h"
#include "Ui/SwaraXtBoardPanel.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace swaraxt;
void require(bool condition, const char* name) { if (!condition) throw std::runtime_error(name); }
void set(SwaraXtAudioProcessor& p, const char* id, float value)
{
    auto* parameter=p.getApvts().getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
std::vector<float> values(SwaraXtAudioProcessor& p)
{
    std::vector<float> result;
    for(auto* parameter:p.getParameters())result.push_back(parameter->getValue());
    return result;
}
bool visibleInEditor(const juce::Component& component)
{
    for(auto* current=&component;current!=nullptr;current=current->getParentComponent())
        if(!current->isVisible())return false;
    return true;
}
void checkCombo(juce::ComboBox& combo, juce::ComboBox& reference)
{
    const auto font=combo.getLookAndFeel().getComboBoxFont(combo);
    require(font==reference.getLookAndFeel().getComboBoxFont(reference),"shared SWARA ComboBox typeface");
    require(combo.getHeight()==ui::Layout::selectorHeight && combo.getTransform().isIdentity(),"native untransformed ComboBox geometry");
    bool found=false;
    for(auto* child:combo.getChildren())
        if(auto* label=dynamic_cast<juce::Label*>(child))
        {
            found=true;
            require(combo.getLocalBounds().contains(label->getBounds()),"internal Label contained");
            require(label->getFont()==font,"internal Label uses SWARA font");
            require(label->findColour(juce::Label::textColourId)==ui::Palette::skin().comboText,"skin-specific ComboBox text");
        }
    require(found,"ComboBox Label exists");
}
void snapshot(SwaraXtAudioProcessorEditor& editor,const juce::String& name)
{
    const auto directory=juce::SystemStats::getEnvironmentVariable("SWARA_BOARD_GUI_OUTPUT",{});
    if(directory.isEmpty())return;
    const juce::File root(directory);
    require(root.createDirectory().wasOk(),"external snapshot directory");
    const auto file=root.getChildFile(name+".png");
    juce::FileOutputStream stream(file);
    stream.setPosition(0);stream.truncate();
    juce::PNGImageFormat format;
    require(stream.openedOk() && format.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),stream),"GUI snapshot");
}
void testEditor()
{
    SwaraXtAudioProcessor p;p.prepareToPlay(48000,512);
    const auto initial=values(p);
    SwaraXtAudioProcessorEditor editor(p);
    editor.setVisible(true);
    auto& panel=editor.boardPanelForTests();
    editor.setBoardEditorViewForTests(true);
    require(values(p)==initial,"editor construction/view is observational");
    require(panel.modelCombo().getNumItems()==2 && panel.fxCombo().getNumItems()==17
        && panel.routeCombo().getNumItems()==5,"complete parameter-derived choice lists");
    require(!visibleInEditor(panel.routeCombo()),"Classic hides Board route");
    require(!panel.control1().isVisible()&&!panel.control2().isVisible(),"Off hides FX controls");
    for(auto skin:{ui::SkinId::pastel,ui::SkinId::midnightGold,ui::SkinId::neonCobalt,ui::SkinId::jungle,ui::SkinId::rossocorsa})
    {
        editor.setSkinForTests(skin);
        for(auto size:{ui::GuiSize::small,ui::GuiSize::medium,ui::GuiSize::large})
        {
            editor.setGuiSizeForTests(size);
            for(auto* combo:{&panel.modelCombo(),&panel.fxCombo(),&panel.routeCombo()})
                checkCombo(*combo,editor.mixOperatorComboForTests());
            require(!panel.modelCombo().getParentComponent()->getBounds().intersects(panel.fxCombo().getParentComponent()->getBounds()),"model and FX selectors separate");
            require(!panel.control1().getBounds().intersects(panel.control2().getBounds()),"FX knobs separate");
            for(int model=0;model<2;++model)
            {
                set(p,IDs::filterModel,float(model));
                require(visibleInEditor(panel.routeCombo())==(model==1),"automation updates route visibility");
                for(int effect=0;effect<17;++effect)
                {
                    set(p,IDs::dspFxProgram,float(effect));
                    require(panel.fxCombo().getSelectedItemIndex()==effect,"all FX automation reflected");
                    require(panel.control1().isVisible()==(effect!=0),"context control 1 visibility");
                    require(panel.control2().isVisible()==(effect!=0 && effect!=15),"context control 2 visibility");
                    require(panel.replayButton().isVisible()==(effect==15),"looper exposes record/replay toggle");
                    if(effect!=0)require(panel.control1().labelText().isNotEmpty(),"context name present");
                    const auto before=values(p);
                    editor.setBoardEditorViewForTests(false);
                    require(visibleInEditor(editor.filterKeyTrackForTests()),"filter controls return");
                    editor.setBoardEditorViewForTests(true);
                    require(values(p)==before,"FILTER/FX navigation cannot change sound");
                }
            }
            set(p,IDs::dspFxProgram,9);
            snapshot(editor,"board-"+juce::String(int(skin))+"-"+juce::String(int(size)));
        }
    }
    set(p,IDs::dspFxProgram,15);
    set(p,IDs::dspFxParam2,31);
    require(!panel.replayButton().getToggleState(),"CV2 below 128 is record");
    set(p,IDs::dspFxParam2,32);
    require(panel.replayButton().getToggleState(),"CV2 at 128 is replay");
    panel.replayButton().setToggleState(false,juce::sendNotificationSync);
    require(p.getApvts().getRawParameterValue(IDs::dspFxParam2)->load()==0,"record toggle reaches native endpoint");
    panel.replayButton().setToggleState(true,juce::sendNotificationSync);
    require(p.getApvts().getRawParameterValue(IDs::dspFxParam2)->load()==63,"replay toggle reaches native endpoint");
    snapshot(editor,"board-looper-large");
    editor.setModuleViewsForTests(false,true);
    editor.setSequencerEditorViewForTests(true);
    require(!editor.modMatrixHeaderVisibleForTests(),"SEQ header isolation preserved");
    snapshot(editor,"board-sequence-large");
    editor.setSequencerEditorViewForTests(false);
    require(!editor.modMatrixHeaderVisibleForTests(),"ARP header isolation preserved");
    snapshot(editor,"board-arp-large");
    juce::MemoryBlock state;p.getStateInformation(state);
    SwaraXtAudioProcessor restored;restored.setStateInformation(state.getData(),int(state.getSize()));
    const auto before=values(restored);
    {
        SwaraXtAudioProcessorEditor recreated(restored);
        recreated.setBoardEditorViewForTests(true);
        require(recreated.boardPanelForTests().modelCombo().getSelectedItemIndex()==1
            && recreated.boardPanelForTests().fxCombo().getSelectedItemIndex()==15,"recreated editor restored Board/FX");
    }
    require(values(restored)==before,"editor recreation preserves all parameters");
    std::cout<<"Board GUI: 5 skins x 3 sizes x 2 models x 17 effects, context, looper switch, view/state isolation PASS\n";
}
}
int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    try {testEditor();}
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
    return 0;
}
