#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace XeFGGamePacing {
// Stateless phase shift, not a recurring clock. Each deadline derives only from
// this packet's original submission. Late packets cannot create timing debt.
inline double PhaseOnlyDeadline(double submitted,double ready,std::uint64_t id,
                                double phaseMs,unsigned parity,double sourceMs,
                                bool enabled,bool pressure) noexcept {
    if(!enabled || pressure || !std::isfinite(submitted) || !std::isfinite(ready) ||
       !std::isfinite(phaseMs) || !std::isfinite(sourceMs) || sourceMs<=0 || ready<submitted)
        return ready;
    const double phase=(std::max)(-4.0,(std::min)(4.0,phaseMs));
    const bool selected=(id&1U)==(parity&1U);
    // A negative relative phase advances selected outputs by holding the other
    // parity instead. No attempt to output an image before it is complete.
    double hold=selected?(std::max)(0.0,phase):(std::max)(0.0,-phase);
    hold=(std::min)(hold,sourceMs*0.24);
    return (std::max)(ready,submitted+hold);
}
}
