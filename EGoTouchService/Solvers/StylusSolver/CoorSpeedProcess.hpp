#pragma once

#include "AsaTypes.hpp"
#include "SolverTypes.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace Solvers::Stylus {

class CoorSpeedProcess {
public:
    bool m_enabled = true;

    inline void Reset() {
        m_xHistory.fill(kInvalidCoord);
        m_yHistory.fill(kInvalidCoord);
        m_historyCount = 0;
        m_prevPressureActive = false;
    }

    inline void Process(HeatmapFrame& frame) {
        auto& runtime = frame.stylus.runtime;
        const auto& coor = runtime.tx1.coordinate.reportGlobalCoor;
        const bool pressureActive = runtime.pressure.outputPressure > 0;

        if (!m_enabled || !coor.valid) {
            m_prevPressureActive = pressureActive;
            return;
        }

        // Mirror TSACore GetRealTimeCoor2Buf/GetCoorSpeed: speed is derived from the
        // raw mapped coordinate history, not from the already filtered post chain.
        // That keeps IIR coefficient selection from being biased by earlier smoothing.
        for (int i = kHistorySize - 1; i > 0; --i) {
            m_xHistory[static_cast<std::size_t>(i)] = m_xHistory[static_cast<std::size_t>(i - 1)];
            m_yHistory[static_cast<std::size_t>(i)] = m_yHistory[static_cast<std::size_t>(i - 1)];
        }
        m_xHistory[0] = coor.dim1;
        m_yHistory[0] = coor.dim2;
        if (m_historyCount < kHistorySize) {
            ++m_historyCount;
        }

        // ── Pairwise distance accumulation (TSACore GetCoorSpeed loop) ──
        int cumulativeDistX10 = 0;  // local_14: sum of sqrt(dx²+dy²)*100, scaled x10
        int lastValidIdx = 0;

        for (int c = 1; c < m_historyCount; ++c) {
            if (m_xHistory[static_cast<std::size_t>(c)] == kInvalidCoord) {
                continue;  // skip invalid sentinel
            }
            lastValidIdx = c;

            const int32_t dx = m_xHistory[static_cast<std::size_t>(c - 1)] -
                               m_xHistory[static_cast<std::size_t>(c)];
            const int32_t dy = m_yHistory[static_cast<std::size_t>(c - 1)] -
                               m_yHistory[static_cast<std::size_t>(c)];

            // sqrt((dx² + dy²) * 100)
            const double dist = std::sqrt(static_cast<double>(
                static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy) * 100.0);
            cumulativeDistX10 += static_cast<int>(dist);
        }

        if (lastValidIdx == 0) {
            runtime.post.speedValue = 0;
            runtime.post.speedShortAvgDist = 0;
            runtime.post.speedFullAvgDist = 0;
            runtime.post.speedAvgDx = 0;
            runtime.post.speedAvgDy = 0;
            m_prevPressureActive = pressureActive;
            return;
        }

        // First-to-current distance
        const int32_t dxFirst = m_xHistory[0] - m_xHistory[static_cast<std::size_t>(lastValidIdx)];
        const int32_t dyFirst = m_yHistory[0] - m_yHistory[static_cast<std::size_t>(lastValidIdx)];
        int instantDist = static_cast<int>(
            std::sqrt(static_cast<double>(
                static_cast<int64_t>(dxFirst) * dxFirst +
                static_cast<int64_t>(dyFirst) * dyFirst) * 100.0));
        if (instantDist == 0) instantDist = 1;

        // Cumulative distance (÷10)
        const int cumDist = (cumulativeDistX10 > 0) ? cumulativeDistX10 / 10 : 1;

        // Average distance per frame
        runtime.post.speedFullAvgDist = cumDist / lastValidIdx;

        int speedShortAvgDist = runtime.post.speedFullAvgDist;
        if (lastValidIdx > 3) {
            int dist3X10 = 0;
            for (int c = 1; c <= 3; ++c) {
                const int32_t dx = m_xHistory[static_cast<std::size_t>(c - 1)] -
                                   m_xHistory[static_cast<std::size_t>(c)];
                const int32_t dy = m_yHistory[static_cast<std::size_t>(c - 1)] -
                                   m_yHistory[static_cast<std::size_t>(c)];
                dist3X10 += static_cast<int>(std::sqrt(static_cast<double>(
                    static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy) * 100.0));
            }
            speedShortAvgDist = (dist3X10 / 10) / 3;
        }
        runtime.post.speedShortAvgDist = speedShortAvgDist;

        // Average dx/dy over valid points
        runtime.post.speedAvgDx = std::abs(dxFirst) / lastValidIdx;
        runtime.post.speedAvgDy = std::abs(dyFirst) / lastValidIdx;

        // Speed value: 1-frame instant distance (drives IIR coefficient selection)
        int speedInstant = 0;
        if (m_historyCount >= 2 && m_xHistory[1] != kInvalidCoord) {
            const int32_t dx = m_xHistory[0] - m_xHistory[1];
            const int32_t dy = m_yHistory[0] - m_yHistory[1];
            speedInstant = static_cast<int>(std::sqrt(static_cast<double>(
                static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy) * 100.0)) / 10;
        }
        runtime.post.speedValue = speedInstant;

        m_prevPressureActive = pressureActive;
    }

private:
    static constexpr int kHistorySize = 24;
    static constexpr int32_t kInvalidCoord = 0x7FFFFFFF;

    std::array<int32_t, kHistorySize> m_xHistory{};
    std::array<int32_t, kHistorySize> m_yHistory{};
    int m_historyCount = 0;
    bool m_prevPressureActive = false;
};

} // namespace Solvers::Stylus
