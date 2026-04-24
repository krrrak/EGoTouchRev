#pragma once

#include "SolverTypes.h"

namespace Asa {

class StylusCoordinateFilter {
public:
    inline void Process(Solvers::StylusFrameData& stylus) const {
        stylus.output.valid = stylus.point.valid;
        stylus.output.inRange = stylus.point.valid;
        stylus.output.tipDown = stylus.tipSwitchActive || stylus.pressure > 0;
        stylus.output.pressure = stylus.pressure;
        stylus.output.confidence = stylus.point.confidence;
        stylus.output.pipelineStage = stylus.pipelineStage;
        stylus.output.point = stylus.point;
        stylus.output.point.valid = stylus.output.valid;
        stylus.output.point.pressure = stylus.output.pressure;
    }
};

} // namespace Asa
