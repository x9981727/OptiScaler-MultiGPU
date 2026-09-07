#include "ManagedXeFGSequence.h"
#include "ManagedXeFGProtocol.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <limits>

static void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
struct Result {
    unsigned mismatch = 0, rendered = 0, originals = 0, sleepCalls = 0;
    std::uint32_t serial = 72, expected = 73, lastSleep = 72;
};
static Result replay(bool patched, unsigned missing)
{
    Result out;
    const auto frame = [&](bool complete) {
        const auto id = patched ? MultiGPU::NextManagedXeFGId(out.serial, complete) : ++out.serial;
        std::vector<unsigned> phases;
        auto native = MultiGPU::RunManagedXeFGProtocol(complete,id,
            [&](auto value) {
                ++out.sleepCalls; out.lastSleep = value;
                if (value != out.expected) { ++out.mismatch; return false; }
                ++out.expected; return true;
            },
            [&](auto value, unsigned phase) {
                require(value == out.lastSleep, "marker ID differs from Sleep ID");
                phases.push_back(phase); return true;
            },
            [&](auto value) { require(value == out.lastSleep, "tag ID differs from Sleep ID"); return true; },
            [&]() -> std::int32_t { ++out.rendered; return 0x087a0001; },
            [&](int) -> std::int32_t { ++out.originals; return 0x087a0001; });
        require(native == 0x087a0001,"native HRESULT changed");
        if (complete && out.mismatch==0) require(phases==std::vector<unsigned>({0,1,2,3,4,5}),"marker order");
    };
    for(unsigned n=0;n<missing;++n) frame(false);
    frame(true);
    return out;
}
int main()
{
    // Model the observed old-code gap using the actual production protocol helper.
    const auto bad=replay(false,2364);
    require(bad.mismatch==1 && bad.lastSleep==2437 && bad.expected==73,"old source reproduction changed");
    std::cout << "REPRODUCED v25: requested="<<bad.lastSleep<<", expected="<<bad.expected<<"\n";
    for(unsigned n: {0u,1u,2364u,100000u}) {
        const auto fixed=replay(true,n);
        require(fixed.mismatch==0 && fixed.lastSleep==73 && fixed.sleepCalls==1 && fixed.rendered==1 && fixed.originals==n,
                "incomplete packet consumed XeLL ID");
    }
    std::uint32_t serial=(std::numeric_limits<std::uint32_t>::max)();
    require(MultiGPU::NextManagedXeFGId(serial,true)==0 && serial==(std::numeric_limits<std::uint32_t>::max)(),"wrapped ID");
    std::cout << "PASS: incomplete packets preserve XeLL sequence; one real original fallback per packet; marker/tag IDs agree; HRESULT preserved; wrap rejected.\n";
    std::cout << "NOT TESTED: Intel DLL, AMD GPUs, image flicker, performance or 67->134 displayed FPS.\n";
}
