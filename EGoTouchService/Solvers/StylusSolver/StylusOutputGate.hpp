#pragma once

#include "SolverTypes.h"

namespace Asa {

class StylusOutputGate {
public:
    inline void Process(Solvers::StylusFrameData& stylus) const {
        stylus.SyncContractFromLegacyFields();
    }
};

} // namespace Asa
