// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine/DspBoard/BoardPostDacFilter.h"
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using swaraxt::board::BoardPostDacFilter;
namespace {
constexpr double pi = 3.141592653589793238462643383279502884;
void require(bool c, const char* why) { if (!c) throw std::runtime_error(why); }

double modeledDb(double frequency)
{
    const std::complex<double> z = std::polar(1., -2.*pi*frequency/BoardPostDacFilter::nativeRate);
    std::complex<double> h = BoardPostDacFilter::dcCompensation;
    for (auto c : BoardPostDacFilter::sections_)
        h *= (c[0]+c[1]*z+c[2]*z*z)/(1.+c[3]*z+c[4]*z*z);
    return 20.*std::log10(std::abs(h));
}

double analogChebyDb(double frequency)
{
    // Four-pole Chebyshev Type I, 3-dB peak-to-peak ripple, corner 19k.
    const double x = frequency/19000.;
    const double t4 = 8*x*x*x*x-8*x*x+1.;
    const double epsSq = std::pow(10.,.3)-1.;
    return 10.*std::log10((1.+epsSq)/(1.+epsSq*t4*t4));
}

void response()
{
    require(std::abs(modeledDb(0))<1e-10,"DC gain unity");
    double maxError = 0, maxRipple = 0;
    for (int hz=0;hz<=16000;hz+=20)
    {
        const double actual = modeledDb(hz);
        const double target = analogChebyDb(hz);
        maxError = std::max(maxError,std::abs(actual-target));
        maxRipple = std::max(maxRipple,actual);
        require(actual>-.15 && actual<3.2,"baseband bounded ripple");
    }
    require(maxError<.1,"0..16 kHz board passband model within 0.1 dB");
    require(maxRipple>2.7 && maxRipple<3.2,"3 dB nominal ripple");
    for (int hz=16000;hz<=19000;hz+=20)
        require(std::abs(modeledDb(hz)-analogChebyDb(hz))<.45,"upper passband within 0.45 dB");
    std::cout<<"Analog-vs-native baseband max error: "<<maxError<<" dB; ripple peak "<<maxRipple<<" dB\n";
}

void stateAndSafety()
{
    BoardPostDacFilter a,b;
    std::array<float,40> block{};
    double peak=0.;
    std::uint32_t seed=0x21;
    for (int j=0;j<5000;++j)
    {
        for (auto& v:block) {
            seed=seed*1664525u+1013904223u;
            v=static_cast<float>(static_cast<int>(seed>>24)-128)/128.f;
        }
        auto x=block;
        a.process(x,x.size());
        for (std::size_t i=0;i<block.size();++i)
        {
            const auto v=b.process(block[i]);
            require(v==x[i],"sample/block partition exact");
            require(std::isfinite(v) && std::abs(v)<4.0f,"finite and bounded output");
            peak=std::max(peak,double(std::abs(v)));
        }
    }
    a.reset();b.reset();
    for(int i=0;i<8192;++i)
        require(a.process(i==0?1.f:0.f)==b.process(i==0?1.f:0.f),"reset reproducibility");
    a.reset();
    require(a.process(0.f)==0.f,"hard reset outputs exact silence");
    a.process(1.f);
    require(a.process(std::numeric_limits<float>::quiet_NaN())==0.f,"NaN guard");
    require(a.process(0.f)==0.f,"NaN resets all section memory");
    require(std::isfinite(peak),"peak finite");
    std::cout<<"200000 samples: block/sample bit-exact and finite; peak="<<peak<<"\n";
}
}
int main()
{
    try { response(); stateAndSafety(); }
    catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
    std::cout<<"Post-DAC model tests PASS\n";
}
