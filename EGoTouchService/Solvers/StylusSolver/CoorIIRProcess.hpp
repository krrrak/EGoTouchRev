#pragma once

#include "AsaTypes.hpp"
#include "SolverTypes.h"

#include <cstdint>

namespace Solvers::Stylus {

class CoorIIRProcess {
public:
    bool m_enabled = true;

    // ── IIR coefficient selection (GetIIRCoef equivalent) ──
    // In-band mode params (edge NOT active)
    uint8_t m_coefLowInBand = 10;     // flash[0xA5E]
    uint8_t m_coefHighInBand = 200;   // flash[0xA5F]
    uint8_t m_speedTholdInBand = 20;  // 0x14

    // Edge mode params (edge active)
    uint8_t m_coefLowEdge = 10;    // flash[0xA5C]
    uint8_t m_coefHighEdge = 100;  // flash[0xA5D]
    uint8_t m_speedTholdEdge = 10; // 0x0A

    int m_speedMax = 205;  // 0xCD — speed value at which high coef is fully engaged
    uint8_t m_maxCoef = 255;  // flash[0xA60] — denominator in IIR formula

    // ── Output ──
    uint16_t m_currentCoef = 10;

    inline void Reset() {
        m_iirFracX = 0;
        m_iirFracY = 0;
        m_prevFiltX = 0;
        m_prevFiltY = 0;
        m_prevInRange = false;
        m_frameCount = 0;
        m_currentCoef = m_coefLowInBand;
    }

    inline void Process(HeatmapFrame& frame) {
        auto& runtime = frame.stylus.runtime;
        auto& coor = runtime.post.finalCoor;
        const bool inRange = coor.valid;  // stylus detected (hover OR touch)

        if (!m_enabled || !inRange) {
            Reset();
            return;
        }

        // ── Detect entry into in-range (from out-of-range) ──
        if (!m_prevInRange && inRange) {
            m_frameCount = 0;
            m_iirFracX = 0;
            m_iirFracY = 0;
            m_prevFiltX = coor.dim1;
            m_prevFiltY = coor.dim2;
        }
        m_prevInRange = inRange;
        ++m_frameCount;

        // ── GetIIRCoef: speed-adaptive coefficient selection ──
        const bool edgeActive = runtime.signal.dim1EdgeActive ||
                                runtime.signal.dim2EdgeActive;
        m_currentCoef = SelectCoef(runtime.post.speedValue, edgeActive, edgeActive);
        runtime.post.iirCoef = m_currentCoef;

        // ── CoorFilterProcess: conditional IIR filtering ──
        // TSACore gate: filter when (in-range flag set AND counter_c >= 2),
        // i.e. after 2+ frames of detection (hover OR touch).
        // Pressure is NOT part of the gate.
        if (m_frameCount < 2) {
            m_iirFracX = 0;
            m_iirFracY = 0;
            m_prevFiltX = coor.dim1;
            m_prevFiltY = coor.dim2;
            runtime.post.iirFilterActive = false;
            return;
        }

        // Apply IIR filter (active for both hover and touch after warm-up)
        runtime.post.iirFilterActive = true;
        ApplyIIR(coor.dim1, coor.dim2);

        m_prevFiltX = coor.dim1;
        m_prevFiltY = coor.dim2;
    }

private:
    uint8_t m_iirFracX = 0;
    uint8_t m_iirFracY = 0;
    int32_t m_prevFiltX = 0;
    int32_t m_prevFiltY = 0;
    bool m_prevInRange = false;
    int m_frameCount = 0;

    inline uint16_t SelectCoef(int speedValue, bool edgeActive, bool peakOnEdge) const {
        uint8_t coefLow;
        uint8_t coefHigh;
        int speedThreshold;

        if (edgeActive) {
            coefLow = m_coefLowEdge;
            coefHigh = m_coefHighEdge;
            speedThreshold = m_speedTholdEdge;
        } else {
            coefLow = m_coefLowInBand;
            coefHigh = m_coefHighInBand;
            speedThreshold = m_speedTholdInBand;
        }

        // Peak-on-edge halves the coefficient range
        if (peakOnEdge) {
            coefHigh = static_cast<uint8_t>(coefHigh >> 1);
            coefLow = static_cast<uint8_t>(coefLow >> 1);
        }

        uint16_t selected;
        if (speedValue < speedThreshold) {
            selected = coefLow;
        } else if (speedValue >= m_speedMax) {
            selected = coefHigh;
        } else {
            // Linear interpolation between low and high
            selected = static_cast<uint16_t>(
                coefLow + (static_cast<int>(coefHigh - coefLow) *
                           (speedValue - speedThreshold)) /
                              (m_speedMax - speedThreshold));
        }
        return selected;
    }

    inline void ApplyIIR(int32_t& dim1, int32_t& dim2) {
        // Fixed-point IIR: new_sample << 8, old_sample = prev_inner * 256 + prev_frac
        const int32_t newX = dim1 << 8;
        const int32_t oldX = (m_prevFiltX << 8) + static_cast<int32_t>(m_iirFracX);
        const int32_t newY = dim2 << 8;
        const int32_t oldY = (m_prevFiltY << 8) + static_cast<int32_t>(m_iirFracY);

        const int32_t coef = static_cast<int32_t>(m_currentCoef);
        const int32_t antiCoef = static_cast<int32_t>(m_maxCoef) - coef;

        // filtered = (coef * new + (maxCoef - coef) * old) / maxCoef
        const int32_t filtX = (coef * newX + antiCoef * oldX) / static_cast<int32_t>(m_maxCoef);
        const int32_t filtY = (coef * newY + antiCoef * oldY) / static_cast<int32_t>(m_maxCoef);

        dim1 = filtX >> 8;
        m_iirFracX = static_cast<uint8_t>(filtX & 0xFF);

        dim2 = filtY >> 8;
        m_iirFracY = static_cast<uint8_t>(filtY & 0xFF);
    }
};

} // namespace Solvers::Stylus
