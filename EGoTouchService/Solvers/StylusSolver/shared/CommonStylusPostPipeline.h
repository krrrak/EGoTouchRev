#pragma once

#include "AftCoorProcess.hpp"
#include "CoorIIRProcess.hpp"
#include "CoorReviseProcess.hpp"
#include "CoorSpeedProcess.hpp"
#include "LinearFilterProcess.hpp"
#include "SolverTypes.h"

namespace Solvers::Stylus {

// CommonStylusPostPipeline — ASA_CoorPostProcess-style coordinate post chain.
//
// TSACore routes successful HPP2 and HPP3 protocol processing through the same
// ASA_CoorPostProcess tail. Keep these stateful filters outside protocol
// sub-pipelines so HPP2 can reuse the exact same post chain when it is added.
class CommonStylusPostPipeline {
public:
    LinearFilterProcess  m_linearFilterProcess;
    CoorReviseProcess    m_coorReviseProcess;
    CoorSpeedProcess     m_coorSpeedProcess;
    CoorIIRProcess       m_coorIIRProcess;
    AftCoorProcess       m_aftCoorProcess;

    inline void Process(HeatmapFrame& frame) {
        m_linearFilterProcess.Process(frame);
        m_coorReviseProcess.Process(frame);
        m_coorSpeedProcess.Process(frame);
        m_coorIIRProcess.Process(frame);
        m_aftCoorProcess.Process(frame);
    }

    inline void ResetOnTerminal() {
        m_linearFilterProcess.Reset();
        m_coorReviseProcess.Reset();
        m_coorSpeedProcess.Reset();
        m_coorIIRProcess.Reset();
        m_aftCoorProcess.Reset();
    }
};

} // namespace Solvers::Stylus
