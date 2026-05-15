#pragma once

#include "LinearFilterProcess.hpp"
#include "SolverTypes.h"

#include <algorithm>

namespace Solvers::Stylus {

class StylusPostProcessor {
public:
    enum FilterMode : int {
        IirQ8 = 0,
        OneEuro = 1,
        Bypass = 2,
    };

    bool m_enabled = true;
    int m_filterMode = Bypass;
    int m_sensorRows = 40;
    int m_sensorCols = 60;
    LinearFilterProcess m_linearFilter;

    inline bool Process(HeatmapFrame& frame) {
        auto& stylus = frame.stylus;
        auto& flow = stylus.runtime.flow;
        auto& post = stylus.runtime.post;
        auto& decision = stylus.runtime.decision;

        flow.pipelineStage = 3;
        if (!m_enabled || !stylus.runtime.tx1.globalCoor.valid) {
            post = {};
            m_linearFilter.Reset();
            return true;
        }

        post.finalValid = decision.inRangeCandidate;
        post.finalPressure = decision.tipDownCandidate
            ? stylus.runtime.pressure.outputPressure
            : 0;

        Asa::AsaCoorResult filteredCoor = stylus.runtime.tx1.globalCoor;
        if (m_filterMode == IirQ8) {
            filteredCoor = m_linearFilter.Process(
                stylus.runtime.tx1.globalCoor,
                post.finalPressure != 0,
                std::max(0, m_sensorCols) * Asa::kCoorUnit,
                std::max(0, m_sensorRows) * Asa::kCoorUnit);
        } else {
            m_linearFilter.Reset();
        }

        post.postCoor = filteredCoor;
        post.finalCoor = filteredCoor;
        post.linearFilterState = m_linearFilter.State();
        post.linearFilterActive = m_linearFilter.Active();
        post.linearFilterDeltaDim1 = m_linearFilter.LastDeltaDim1();
        post.linearFilterDeltaDim2 = m_linearFilter.LastDeltaDim2();
        post.confidence = std::clamp(
            static_cast<float>(stylus.runtime.signal.maxRawPeak) / 4095.0f,
            0.0f,
            1.0f);

        post.point.valid = post.finalValid;
        post.point.x = static_cast<float>(post.finalCoor.dim1);
        post.point.y = static_cast<float>(post.finalCoor.dim2);
        post.point.reportX = static_cast<uint16_t>(std::clamp(post.finalCoor.dim1, 0, 0xFFFF));
        post.point.reportY = static_cast<uint16_t>(std::clamp(post.finalCoor.dim2, 0, 0xFFFF));
        post.point.pressure = post.finalPressure;
        post.point.rawPressure = stylus.runtime.pressure.rawPressure;
        post.point.mappedPressure = stylus.runtime.pressure.mappedPressure;
        post.point.peakTx1 = stylus.runtime.signal.tx1Composite;
        post.point.peakTx2 = stylus.runtime.signal.tx2Composite;
        post.point.tx1X = static_cast<float>(stylus.runtime.tx1.globalCoor.dim1);
        post.point.tx1Y = static_cast<float>(stylus.runtime.tx1.globalCoor.dim2);
        post.point.tx2X = static_cast<float>(stylus.runtime.tx2.globalCoor.dim1);
        post.point.tx2Y = static_cast<float>(stylus.runtime.tx2.globalCoor.dim2);
        post.point.confidence = post.confidence;
        return true;
    }
};

} // namespace Solvers::Stylus
