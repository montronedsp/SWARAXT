// Copyright2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Retain all16-pattern firmware/MIDI/state assertions; UI assertions are excluded.
#define main swaraxtUnusedOriginalMain
#include "../Regression/ControlIntegrityTestsMain.cpp"
#undef main
void currentArpHeadless(){
    for(int pattern=0;pattern<16;++pattern){
        pump();P p;Host host;p.setPlayHead(&host);p.prepareToPlay(48000,128);
        set(p,swaraxt::IDs::seqMode,1);set(p,swaraxt::IDs::seqClockMode,1);set(p,swaraxt::IDs::seqMidiOut,1);set(p,swaraxt::IDs::arpGate,11);
        auto* a=p.getApvts().getParameter(swaraxt::IDs::arpPattern);
        a->setValueNotifyingHost(float(pattern)/15);pump();check(get(p,swaraxt::IDs::arpPattern)==pattern,"normalized current host reaches physical parameter");
        set(p,swaraxt::IDs::arpPattern,float(pattern));check(p.sequenceState().snapshot().arpPattern==pattern,"physical host parameter reaches sequence publication");
        // ARP intentionally does not emit external MIDI in the existing product.
        // Observe the actual firmware-generated note callbacks, then test the
        // supported external SEQ route without changing that policy.
        InternalNotes internal;swaraxt::SequencerMidiTestAccess::tap(p.engineForTests(),{InternalNotes::on,InternalNotes::off,&internal});
        for(int step=0;step<16;++step)p.sequenceState().setStep(step,uint8_t(0x80|60),0x70);
        juce::AudioBuffer<float>b(2,128);juce::MidiBuffer m;m.ensureSize(2048);std::map<std::pair<int,int>,bool> held;int ons=0;
        for(int block=0;block<256;++block){m.clear();if(!block)for(int note:{60,64,67})m.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);if(block==128)host.ppq+=2.0;p.processBlock(b,m);if(block%16==0)pump();for(auto e:m){auto n=e.getMessage();if(n.isNoteOnOrOff()){held[{n.getChannel(),n.getNoteNumber()}]=n.isNoteOn();ons+=n.isNoteOn();check(e.samplePosition>=0&&e.samplePosition<128,"arp MIDI event timestamp");}}host.ppq+=128.0/12000.0;}
        check(p.engineForTests().shruthiPart().sequencer_settings().arp_pattern==pattern,"all16 values reach firmware during playback");
        host.playing=false;m.clear();p.processBlock(b,m);for(auto e:m){auto n=e.getMessage();if(n.isNoteOff())held[{n.getChannel(),n.getNoteNumber()}]=false;}
        check(internal.ons>0,"every ROM/Sequence pattern executes real firmware note callbacks");
        check(ons==0,"existing ARP external-MIDI suppression is preserved");
        // Mid-playback host changes must reach the firmware without restarting.
        host.playing=true;set(p,swaraxt::IDs::arpPattern,float((pattern+1)%16));m.clear();p.processBlock(b,m);
        check(p.engineForTests().shruthiPart().sequencer_settings().arp_pattern==(pattern+1)%16,"pattern change during playback reaches firmware");
        set(p,swaraxt::IDs::arpPattern,float(pattern));
        if(pattern==15){
            for(int step=0;step<16;++step)p.sequenceState().setStep(step,60,0x70);
            for(int k=0;k<4;++k){m.clear();p.processBlock(b,m);if(k%16==0)pump();host.ppq+=128.0/12000.0;}
            const int before=internal.ons;
            for(int k=0;k<256;++k){m.clear();p.processBlock(b,m);if(k%16==0)pump();host.ppq+=128.0/12000.0;}
            check(internal.ons==before,"pattern15 follows closed Sequence gates instead of ROM");
            for(int step=0;step<16;++step)p.sequenceState().setStep(step,uint8_t(0x80|60),0x70);
            for(int k=0;k<256;++k){m.clear();p.processBlock(b,m);if(k%16==0)pump();host.ppq+=128.0/12000.0;}
            check(internal.ons>before,"pattern15 follows reopened Sequence gates");
        }
        // reset restores the production tap and starts an independent MIDI ledger.
        p.engineForTests().reset();set(p,swaraxt::IDs::seqMode,2);p.sequenceState().setPitchMode(0);
        for(int k=0;k<256;++k){m.clear();p.processBlock(b,m);if(k%16==0)pump();host.ppq+=128.0/12000.0;for(auto event:m){auto n=event.getMessage();if(n.isNoteOnOrOff()){held[{n.getChannel(),n.getNoteNumber()}]=n.isNoteOn();ons+=n.isNoteOn();}}}
        host.playing=false;for(int k=0;k<8;++k){m.clear();p.processBlock(b,m);for(auto event:m){auto n=event.getMessage();if(n.isNoteOff())held[{n.getChannel(),n.getNoteNumber()}]=false;}}
        check(ons>0,"supported external SEQ route emits actual MIDI with every pattern value");
        check(std::none_of(held.begin(),held.end(),[](const auto& v){return v.second;}),"stop discharges external SEQ ledger");
        juce::MemoryBlock state;p.getStateInformation(state);P fresh;fresh.setStateInformation(state.getData(),int(state.getSize()));fresh.prepareToPlay(96000,64);check(get(fresh,swaraxt::IDs::arpPattern)==pattern&&fresh.sequenceState().snapshot().arpPattern==pattern,"all16 state restore and reprepare");
        p.setCurrentProgram(1);pump();check(p.sequenceState().snapshot().arpPattern==int(get(p,swaraxt::IDs::arpPattern)),"factory load leaves coherent arp parameter");p.setPlayHead(nullptr);
        std::printf("arp pattern=%d MIDI ons=%d PASS checks so far=%d\n",pattern,ons,checks);
    }
}
int main() { juce::ScopedJuceInitialiser_GUI init;currentArpHeadless();std::printf("CURRENT_ARP checks=%d failures=%d\n",checks,failures);return failures?1:0; }
