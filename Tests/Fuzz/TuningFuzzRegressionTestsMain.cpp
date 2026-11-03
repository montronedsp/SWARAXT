// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tuning/TuningTable.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t);
namespace {
int failures=0;
void expect(bool okay,const char* reason){if(!okay){std::printf("FAIL: %s\n",reason);++failures;}}
void setFrequency(std::vector<uint8_t>& bytes,int note,double value){
    uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));
    const auto offset=bytes.size()-128*8+static_cast<size_t>(note)*8;
    for(int i=0;i<8;++i)bytes[offset+static_cast<size_t>(i)]=static_cast<uint8_t>(bits>>(8*i));
}
}
int main(){
    using namespace swaraxt::tuning;
    TuningSnapshot initial;ParseError error;std::vector<uint8_t> baseline;
    expect(parseScalaAndKeyboard("cache regression\n2\n700.0\n1200.0\n",nullptr,initial,error,ScalaSyntax::standard),"fixture parses");
    expect(writeTuningTable(initial,baseline,error),"fixture serializes");
    if(failures)return 1;
    int accepted=0;
    for(int note=0;note<128;++note)for(int variant=0;variant<4;++variant){
        auto bytes=baseline;const double canonical=initial.hzForMidi(note);
        const double tolerance=1.e-9*std::max(1.,canonical);
        const double changed=variant==0?std::nextafter(canonical,std::numeric_limits<double>::infinity())
            :variant==1?std::nextafter(canonical,0.)
            :variant==2?canonical+tolerance*.25:canonical+tolerance*4.;
        setFrequency(bytes,note,changed);TuningSnapshot loaded;
        const bool read=readTuningTable(bytes.data(),bytes.size(),loaded,error);
        expect(read==(variant<3),"cache validation retains its documented tolerance");
        if(!read)continue;
        ++accepted;expect(loaded.hzForMidi(note)!=canonical,"accepted incoming cache really differs");
        std::vector<uint8_t> rewritten;expect(writeTuningTable(loaded,rewritten,error),"accepted snapshot canonicalizes");
        expect(rewritten==baseline,"canonical bytes equal independently generated original table");
        TuningSnapshot restored;expect(readTuningTable(rewritten.data(),rewritten.size(),restored,error),"canonical table reads");
        for(int key=0;key<128;++key){
            expect(restored.mapped(key)==loaded.mapped(key),"mapping is unchanged");
            expect(restored.hzForMidi(key)==initial.hzForMidi(key),"canonical frequencies match original scale");
        }
        LLVMFuzzerTestOneInput(bytes.data(),bytes.size());
    }
    // These remain rejected; the fuzzer must not loosen the product parser.
    for(size_t n=0;n<std::min<size_t>(baseline.size(),64);++n)LLVMFuzzerTestOneInput(baseline.data(),n);
    const char* scale="valid fuzz scale\n2\n700.0\n1200.0\n";
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(scale),std::strlen(scale));
    std::printf("TUNING_FUZZ_REGRESSION accepted=%d rejected=128 failures=%d\n",accepted,failures);
    return failures?1:0;
}
