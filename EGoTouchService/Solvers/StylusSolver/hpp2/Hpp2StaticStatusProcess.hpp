#pragma once

#include "Hpp2PipelineContext.hpp"
#include "Hpp2PressureProcess.hpp"

namespace Solvers::Stylus::Hpp2 {

class Hpp2StaticStatusProcess {
public:
    void Process(Hpp2Context& ctx) const {
        auto& runtime = ctx.frame.stylus.runtime;
        runtime.decision.tipDownCandidate =
            runtime.decision.inRangeCandidate && runtime.pressure.outputPressure != 0;
        runtime.decision.authoritativeDown = runtime.decision.tipDownCandidate;
        Hpp2PressureProcess::PublishPressure(ctx.frame);
        ctx.state.m_prevPressure = runtime.pressure.outputPressure;
    }
};

} // namespace Solvers::Stylus::Hpp2
