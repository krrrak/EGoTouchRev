#pragma once

#include "SolverTypes.h"

namespace Asa {

class StylusOutputGate {
public:
    inline void Process(Solvers::StylusFrameData& stylus) const {
        stylus.interop.recheckEnabled = stylus.recheckEnabled;
        stylus.interop.recheckPassed = stylus.recheckPassed;
        stylus.interop.recheckOverlap = stylus.recheckOverlap;
        stylus.interop.recheckThreshold = stylus.recheckThreshold;
        stylus.interop.recheckThresholdMulti = stylus.recheckThresholdMulti;
        stylus.interop.touchNullLike = stylus.touchNullLike;
        stylus.interop.touchSuppressActive = stylus.touchSuppressActive;
        stylus.interop.touchSuppressFrames = stylus.touchSuppressFrames;
        stylus.interop.signalX = stylus.signalX;
        stylus.interop.signalY = stylus.signalY;
        stylus.interop.maxRawPeak = stylus.maxRawPeak;
    }
};

} // namespace Asa
