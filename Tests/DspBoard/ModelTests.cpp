// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine/DspBoard/BoardProcessor.h"
#include <complex>
#include <iostream>
#include <stdexcept>

namespace {
using namespace swaraxt::board;
using Complex = std::complex<double>;
constexpr double pi = 3.14159265358979323846;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
Complex stage(double f, double r1, double r2, double cf, double cg, bool gbw)
{
    const Complex s(0, 2*pi*f);
    const auto follower = gbw ? (2*pi*2.8e6)/(s+2*pi*2.8e6) : Complex(1,0);
    return follower/(1. + s*cg*(r1+r2) + s*r1*cf*(1.-follower) + s*s*r1*r2*cf*cg);
}
Complex analog(double f, bool input)
{
    if (input)
        return stage(f,47e3,3.3e3,10e-9,1e-9,true) * stage(f,39e3,3.9e3,10e-9,220e-12,true)
            * stage(f,12e3,4.7e3,10e-9,220e-12,true) * stage(f,47e3,18e3,10e-9,10e-12,true)
            / (1.+Complex(0,2*pi*f*100e3*68e-12));
    const auto w = pi*f/sampleRate;
    return stage(f,33e3,2.2e3,10e-9,470e-12,false) * stage(f,39e3,4.7e3,10e-9,33e-12,false)
        * (w == 0 ? 1. : std::sin(w)/w) * std::exp(Complex(0,-w));
}
template<std::size_t N>
Complex discrete(double f, const std::array<std::array<double,6>,N>& sos)
{
    const auto z = std::exp(Complex(0,-2*pi*f/sampleRate));
    Complex h(1,0);
    for (const auto& c : sos) h *= (c[0]+c[1]*z+c[2]*z*z)/(1.+c[4]*z+c[5]*z*z);
    return h;
}
void responseTests()
{
    for (const bool input : {true,false})
    {
        double maxMagnitude = 0, maxPhase = 0, lowerMagnitude = 0, lowerPhase = 0;
        for (int f = 0; f <= 19000; f += 5)
        {
            const auto actual = input ? discrete(f,inputSos) : discrete(f,outputSos);
            const auto error = actual/analog(f,input);
            const double magnitude = std::abs(20*std::log10(std::abs(error)));
            const double phase = std::abs(std::arg(error)*180/pi);
            maxMagnitude = std::max(maxMagnitude,magnitude); maxPhase = std::max(maxPhase,phase);
            if (f<=18000) { lowerMagnitude=std::max(lowerMagnitude,magnitude); lowerPhase=std::max(lowerPhase,phase); }
        }
        // Schematic-fit error budget, NOT a physical board calibration claim.
        // Output's nonperiodic analog phase is hardest at native Nyquist; the
        // final 1kHz explicitly has a looser budget, not hidden in an RMS score.
        require(maxMagnitude < (input ? .20 : 2.15), "magnitude model budget 0..19k");
        require(maxPhase < (input ? 4.3 : 13.), "phase model budget 0..19k");
        require(lowerMagnitude < (input ? .20 : .70), "magnitude model budget 0..18k");
        require(lowerPhase < (input ? 4.3 : 5.2), "phase model budget 0..18k");
        std::cout << (input ? "Input" : "Output") << " schematic model: max dB=" << maxMagnitude << " phase degrees=" << maxPhase << "; <=18k dB=" << lowerMagnitude << " phase=" << lowerPhase << " explicit delay=0\n";
    }
    BoardResponse<inputSos.size()> input;
    BoardResponse<outputSos.size()> output;
    double inputL1=0,outputL1=0,peak=0;
    for (int i=0;i<32768;++i)
    {
        const double x=i==0?1:0;
        inputL1+=std::abs(input.process(x,inputSos));
        const double y=output.process(x,outputSos);
        outputL1+=std::abs(y); peak=std::max(peak,std::abs(y));
    }
    require(inputL1<2.4 && outputL1<5.8 && peak<.85,"impulse/headroom budget");
    std::cout << "Impulse L1 input=" << inputL1 << " output=" << outputL1 << " output peak=" << peak << '\n';
    for (int code=0;code<256;++code)
        require(BoardInputModel::quantize((code-128)/128.)==code,"all ADC retained codes");
    require(BoardInputModel::quantize(-2)==0 && BoardInputModel::quantize(2)==255,"ADC clipping");
    require(BoardInputModel::quantize(std::numeric_limits<double>::quiet_NaN())==128,"ADC finite guard");
}
void safetyTests()
{
    double peak=0;
    std::uint64_t samples=0;
    for (int route=0;route<5;++route)
        for (int effect=0;effect<17;++effect)
            for (int history=0;history<6;++history)
            {
                BoardControl c; c.model=Model::dspBoard; c.route=static_cast<Route>(route); c.effect=effectFromChoice(effect);
                BoardProcessor a,b;
                a.reset(c.effect); b.reset(c.effect);
                std::uint32_t random=0x21;
                for (int block=0;block<220;++block)
                {
                    c.cv1=static_cast<std::uint8_t>(history==0?0:history==1?254:block%255);
                    c.cv2=static_cast<std::uint8_t>(history==0?254:history==1?0:block<110?32:200);
                    c.dca=static_cast<std::uint8_t>(history==0?0:254);
                    c.cutoff=static_cast<std::uint8_t>(history<2?254:block%255);
                    c.resonance=static_cast<std::uint8_t>(history<2?0:254);
                    FloatBlock x{};
                    for (std::size_t i=0;i<blockSize;++i)
                    {
                        random=random*1664525u+1013904223u;
                        x[i]=history==0?0.f:history==1?1.f:history==2?-1.f:history==3?(i&1?1.f:-1.f):history==4?(block==0&&i==0?1.f:0.f):float(int(random>>24)-128)/128.f;
                    }
                    auto y=x;
                    a.processBoard(x,c);
                    b.processBoard(y,c);
                    require(x==y,"instance independence");
                    for (const auto v:x)
                    {
                        require(std::isfinite(v) && std::abs(v)<3.,"finite bounded Board output");
                        peak=std::max(peak,static_cast<double>(std::abs(v))); ++samples;
                        if(history==0 && c.effect==Effect::looper) require(v==0,"empty loop output zero");
                    }
                }
            }
    std::cout << "510 route/effect histories: " << samples << " samples, peak=" << peak << ", finite; instances identical\n";
    BoardProcessor p;
    BoardControl c; c.effect=Effect::looper; c.cv1=128;c.cv2=200;
    p.reset(c.effect);
    FloatBlock x{};
    p.processClassicFx(x,c);
    require(!p.hasValidLoop(),"replay before record invalid");
    for(const auto v:x) require(v==0,"empty replay silence");
    c.cv2=0;x.fill(.25f);
    p.processClassicFx(x,c);
    require(p.hasValidLoop(),"successful record valid");
    c.cv2=200;x.fill(0);p.processClassicFx(x,c);
    require(p.needsAudio(c),"loop replay prevents dormancy");
    p.reset(c.effect);require(!p.hasValidLoop(),"reset invalidates loop");
    for(int old=0;old<17;++old)
        for(int next=0;next<17;++next)
        {
            c.effect=effectFromChoice(old);p.reset(c.effect);x.fill(.75f);p.processClassicFx(x,c);
            c.effect=effectFromChoice(next);p.reset(c.effect);x.fill(0);
            BoardProcessor fresh;fresh.reset(c.effect);FloatBlock expected{};
            p.processClassicFx(x,c);fresh.processClassicFx(expected,c);
            require(x==expected,"program reset no stale storage");
        }
    c.effect=Effect::off;x.fill(.1234567f);auto identical=x;p.processClassicFx(x,c);
    require(x==identical,"Classic Off bypass identity");
}
void fixedPointTailTest()
{
    BoardEffects fx;
    Block samples{};
    samples[0]=-16;
    fx.process(samples,Effect::combPositive,0,1,120);
    bool varying=false;
    for(int b=0;b<5000;++b)
    {
        samples.fill(0);fx.process(samples,Effect::combPositive,0,1,120);
        if(b>4000) for(std::size_t i=1;i<blockSize;++i) varying=varying||samples[i]!=samples[i-1];
    }
    require(varying,"source one-code positive feedback limit cycle");
    std::cout << "Positive comb feedback=1 retains AC limit-cycle after >4 seconds: confirmed\n";
}
void compensatedFilterTailTest()
{
    BoardControl c;
    c.model=Model::dspBoard;c.route=Route::lowPassLast;c.effect=Effect::off;
    c.cutoff=0;c.resonance=1;c.dca=0;
    BoardProcessor processor;
    for(int block=0;block<3000;++block)
    {
        FloatBlock samples{};processor.processBoard(samples,c);
    }
    require(!processor.needsAudio(c),"zero effective feedback must not prevent dormancy");
    require(std::isfinite(BoardProcessor::tailSeconds(c)),"zero effective feedback must have finite advertised tail");
    c.resonance=254;
    require(processor.needsAudio(c),"nonzero feedback retains conservative autonomous-filter processing");
    require(std::isinf(BoardProcessor::tailSeconds(c)),"autonomous filter retains sustained tail");
    std::cout << "Cutoff-compensated filter feedback tail policy PASS\n";
}
}
int main()
{
    try { responseTests(); safetyTests(); fixedPointTailTest(); compensatedFilterTailTest(); }
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
    return 0;
}
