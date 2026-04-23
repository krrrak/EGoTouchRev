#pragma once

#include "SolverTypes.h"

namespace Asa {

class StylusCoordinateFilter {
public:
    inline void Process(Solvers::StylusFrameData& stylus) const {
        stylus.SyncContractFromLegacyFields();
    }
};

} // namespace Asa
