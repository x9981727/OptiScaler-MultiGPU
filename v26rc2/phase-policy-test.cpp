#include "PhaseOnlyDeadline.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
static void Expect(bool condition){if(!condition)throw std::runtime_error("phase-only deadline policy regression");}
static void RunPhaseOnlyTests(){
    using XeFGGamePacing::PhaseOnlyDeadline;
    unsigned checks=0;
    for(double fps:{30.,55.,67.,80.,120.,144.,240.,500.,1000.})
        for(double phase:{-4.,-2.2,0.,2.2,4.})
            for(unsigned parity:{0U,1U}) {
                const double source=1000.0/fps,output=source/2;
                double first=0,last=0;
                for(unsigned i=0;i<2000;++i){
                    // Each output is attached to its own submission. There is
                    // no accumulator, previous target, configured FPS or epoch
                    // debt which can survive a source-speed change.
                    const double submitted=1000+i*output;
                    double due=PhaseOnlyDeadline(submitted,submitted,i,phase,parity,source,true,false);
                    Expect(due>=submitted && due-submitted<=std::min(4.0,source*.24)+1e-8);
                    Expect(PhaseOnlyDeadline(submitted,submitted+10,i,phase,parity,source,true,false)==submitted+10);
                    Expect(PhaseOnlyDeadline(submitted,submitted,i,phase,parity,source,true,true)==submitted);
                    Expect(PhaseOnlyDeadline(submitted,submitted,i,phase,parity,source,false,false)==submitted);
                    if(i==0)first=due;last=due;++checks;
                }
                Expect(std::abs((last-first)-(1999*output))<=4.00000001);
            }
    // Starting with a 30-FPS source must not retain a 30/60 rate once the input
    // accelerates: 5,000 independent irregular submissions have bounded age.
    double submitted=90000;
    for(unsigned i=0;i<5000;++i){
        double source=i%3==0?1000./240:(i%3==1?1000./55:1000./144);
        submitted+=source/2;
        double due=PhaseOnlyDeadline(submitted,submitted,i,-2.2,0,source,true,false);
        Expect(due-submitted<=source*.24+1e-8);++checks;
    }
    const double nan=std::numeric_limits<double>::quiet_NaN();
    Expect(PhaseOnlyDeadline(10,11,1,nan,0,15,true,false)==11);
    Expect(PhaseOnlyDeadline(10,11,1,2,0,0,true,false)==11);
    Expect(PhaseOnlyDeadline(nan,11,1,2,0,15,true,false)==11);
    std::printf("PASS: %u phase-only policies (30-1000 source FPS, irregular cadence, expired deadlines, disabled state, pressure bypass, no cumulative rate clock). Mathematical policy test, not game FPS validation.\n",checks);
}
#ifdef PHASE_POLICY_STANDALONE
int main(){RunPhaseOnlyTests();}
#endif
