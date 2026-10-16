#include "Engine/DspBoard/BoardPostDacFilter.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>

using swaraxt::board::BoardPostDacFilter;
static void check(bool ok, const char *why) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
static double magnitude(double frequency) {
    using C = std::complex<double>;
    constexpr double pi=3.1415926535897932384626433832795;
    const auto z=std::exp(C(0,-2*pi*frequency/BoardPostDacFilter::nativeRate));
    C h(BoardPostDacFilter::dcCompensation,0);
    for (const auto& c : BoardPostDacFilter::sections_)
        h *= (c[0]+c[1]*z+c[2]*z*z)/(1.+c[3]*z+c[4]*z*z);
    return std::abs(h);
}
static double targetDb(double frequency) {
    const double ratio = frequency / 19000.;
    const double t=8.*ratio*ratio*ratio*ratio-8.*ratio*ratio+1.;
    const double epsSq=std::pow(10.,0.3)-1.;
    return 10.*std::log10((1.+epsSq)/(1.+epsSq*t*t));
}
int main() {
    check(std::abs(20*std::log10(magnitude(0)))<1.e-8, "unity DC");
    double maxError=0;
    for (int f=0;f<=16000;f+=20) {
        auto db=20*std::log10(magnitude(f));
        maxError=std::max(maxError,std::abs(db-targetDb(f)));
    }
    check(maxError<0.08,"0..16 kHz Chebyshev analog baseband fit");
    check(20*std::log10(magnitude(8000))>2.5,"8k ripple present");
    check(20*std::log10(magnitude(19000))<0.6,"19k not overboosted");
    BoardPostDacFilter filter;
    double sum=0, peak=0;
    for (int n=0;n<100000;++n) {
        const float y=filter.process(n==0?1.f:0.f);
        check(std::isfinite(y),"finite impulse response");
        sum+=y; peak=std::max(peak,std::abs(double(y)));
    }
    check(std::abs(sum-1.)<1.e-4,"impulse sums to unity");
    check(peak<2.,"bounded impulse");
    filter.reset();
    check(filter.process(0.f)==0.f,"reset silence");
    filter.process(.3f);
    check(filter.process(std::numeric_limits<float>::quiet_NaN())==0.f,"nonfinite input sanitized");
    check(filter.process(0.f)==0.f,"nonfinite resets state");
    BoardPostDacFilter a,b;
    for (int n=0;n<100000;++n) {
        float x=float((n*17)%127-63)/64.f;
        check(a.process(x)==b.process(x),"deterministic independent instances");
    }
    std::printf("PASS: response error <=16k %.5f dB; impulse peak %.5f; finite/reset/independence\n",maxError,peak);
}
