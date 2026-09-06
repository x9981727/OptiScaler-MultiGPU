#include "XeFGPresentationStats.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

static void require(bool ok,const char* step)
{
    if(!ok) { std::cerr << "FAIL: " << step << '\n'; std::exit(1); }
}
static bool near(double a,double b) { return std::abs(a-b)<0.001; }
int main()
{
    using Stats=MultiGPU::XeFGPresentationStats;
    Stats doubled; Stats::Snapshot s;
    for(int i=0;i<=60;++i) doubled.Record(i*20.0,2,true,true);
    require(doubled.Read(1200,s),"window available");
    require(near(s.renderFps,50)&&near(s.queuedFps,100)&&s.outputKnown,"real render rate and SDK output counted independently");
    require(s.renderFrames==50&&s.queuedFrames==100,"SDK counts retained");
    require(!doubled.Read(3601,s),"old snapshot expires");

    Stats skipped;
    for(int i=0;i<=60;++i) skipped.Record(i*20.0,1,true,true);
    require(skipped.Read(1200,s)&&near(s.renderFps,50)&&near(s.queuedFps,50),"Active without generated frames does not fabricate doubling");

    Stats mixed;
    for(int i=0;i<=50;++i) mixed.Record(i*20.0,i%2?1:2,true,true);
    require(mixed.Read(1000,s)&&near(s.queuedFps,75),"mixed interpolation success uses reported counts");

    Stats missing;
    for(int i=0;i<=50;++i) missing.Record(i*20.0,2,i!=20,true);
    require(missing.Read(1000,s)&&!s.outputKnown&&near(s.renderFps,50),"failed query keeps render rate but marks output unknown");
    require(!missing.Record(1020,1,true,false),"enable-state transition starts a fresh window");
    require(!missing.Read(1020,s),"transition discards stale FG output");
    for(int i=52;i<=102;++i) missing.Record(i*20.0,1,true,false);
    require(missing.Read(2040,s)&&s.outputKnown&&near(s.queuedFps,50),"disabled measurement reports rendered frames");
    require(!missing.Record(6000,2,true,true)&&!missing.Read(6000,s),"long pause resets measurement");
    std::cout << "PASS: SDK frame counts, skipped interpolation, unknown output and transition resets\n";
}
