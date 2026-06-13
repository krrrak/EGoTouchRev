#pragma once

#include "SolverTypes.h"
#include "Hpp2Runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Solvers::Stylus::Hpp2 {

class Hpp2PeakSelector {
public:
    void Process(Context& ctx) const {
        // TSACore NoiseProcess also calls NoisesLog(); that path is logging-only
        // instrumentation in the analyzed flow, so the rebuild intentionally omits it.
        UpdatePeaksRank(ctx.state.m_peakTableDim1, ctx.state.m_peakCountDim1, true);
        UpdatePeaksRank(ctx.state.m_peakTableDim2, ctx.state.m_peakCountDim2, false);
        GetRealPeak(ctx);
    }

private:
    static void GetRealPeak(Context& ctx) {
        auto& hpp2 = ctx.runtime;
        auto& state = ctx.state;
        hpp2.selectedPeakDim1 = kInvalidPeak;
        hpp2.selectedPeakDim2 = kInvalidPeak;
        hpp2.bypassCurFrame = false;

        bool freqBypass = false;
        if (hpp2.mainFreq == kFreqF1 && state.m_freqNoiseLatchF1) {
            freqBypass = true;
        }
        if (hpp2.mainFreq == kFreqF2 && state.m_freqNoiseLatchF2) {
            freqBypass = true;
        }

        if (freqBypass) {
            hpp2.bypassCurFrame = true;
            ++state.m_bypassCounter;
            state.m_prevBypassed = true;
            return;
        }

        const PeakUnit* dim1 = nullptr;
        const PeakUnit* dim2 = nullptr;
        if (state.m_peakCountDim1 > 0) {
            dim1 = UpdatePeaksWithUnit(ctx, state.m_peakTableDim1, state.m_peakCountDim1, true);
        }
        if (state.m_peakCountDim2 > 0) {
            dim2 = UpdatePeaksWithUnit(ctx, state.m_peakTableDim2, state.m_peakCountDim2, false);
        }

        if ((dim1 != nullptr && IsSelectedPeakAbnormal(*dim1)) ||
            (dim2 != nullptr && IsSelectedPeakAbnormal(*dim2))) {
            hpp2.bypassCurFrame = true;
            ++state.m_bypassCounter;
            state.m_prevBypassed = true;
            return;
        }

        PublishSelectedPeaks(ctx.frame, dim1, dim2);
    }

    static void UpdatePeaksRank(PeakTable& table, int count, bool dim1) {
        (void)dim1;
        static constexpr int kAvgHighFlag = 0x01;
        static constexpr int kSignalRegionFlag = 0x02;
        static constexpr int kAvgMediumFlag = 0x04;
        static constexpr int kCompositeFlag = 0x08;
        uint16_t strongestSignal = 0;
        int strongestSlot = -1;
        for (int i = 0; i < count; ++i) {
            const auto& unit = table[static_cast<std::size_t>(i)];
            if (unit.valid && strongestSignal <= unit.peakSignal) {
                strongestSignal = unit.peakSignal;
                strongestSlot = i;
            }
        }

        int bestSlot = -1;
        uint16_t bestRank = 0;
        for (int i = 0; i < count; ++i) {
            auto& unit = table[static_cast<std::size_t>(i)];
            unit.rankScore = 0;
            if (!unit.valid) {
                continue;
            }

            uint32_t rank = 0;
            if (i == strongestSlot && (unit.noiseProp & kCompositeFlag) == 0) {
                ++rank;
            }
            if (unit.age > 0x14) {
                ++rank;
            }
            if ((unit.noiseProp & kCompositeFlag) == 0) {
                ++rank;
            }
            if (strongestSignal != 0) {
                rank += (static_cast<uint32_t>(unit.peakSignal) * 10u) / strongestSignal;
            }
            if ((unit.noiseProp & kAvgMediumFlag) == 0) {
                ++rank;
            }
            if ((unit.noiseProp & kAvgHighFlag) == 0) {
                ++rank;
            }
            if ((unit.noiseProp & kSignalRegionFlag) == 0) {
                ++rank;
            }
            if (unit.candidateCoor < 0) {
                rank += 0x14u;
            } else {
                const uint32_t coorMetric = static_cast<uint32_t>(unit.candidateCoor);
                rank += coorMetric < 0x200u ? 0x14u : 0x14u / std::max<uint32_t>(coorMetric >> 9, 1u);
            }

            unit.rankScore = static_cast<uint16_t>(std::min<uint32_t>(rank, 0xffffu));
            if (bestSlot < 0 || unit.rankScore > bestRank ||
                (unit.rankScore == bestRank && unit.peakSignal > table[static_cast<std::size_t>(bestSlot)].peakSignal)) {
                bestSlot = i;
                bestRank = unit.rankScore;
            }
        }

        if (bestSlot < 0 || bestRank == 0) {
            for (int i = 0; i < count; ++i) {
                table[static_cast<std::size_t>(i)].valid = false;
            }
        }
    }

    static const PeakUnit* UpdatePeaksWithUnit(Context& ctx,
                                                   const PeakTable& table,
                                                   int count,
                                                   bool dim1) {
        const PeakUnit* selected = SelectHighestRankPeak(table, count);
        if (selected == nullptr) {
            return nullptr;
        }

        auto& hpp2 = ctx.runtime;
        const uint8_t selectedIndex = static_cast<uint8_t>(selected->index);
        const std::size_t freqIdx = static_cast<std::size_t>(ctx.state.m_curFreqIdx);
        const PeakBoundary selectedBoundary{
            selected->leftBoundary,
            selected->rightBoundary,
            selected->leftBoundary >= 0 && selected->rightBoundary >= selected->leftBoundary,
        };
        if (dim1) {
            hpp2.selectedPeakDim1 = selectedIndex;
            ctx.state.m_prevPeakDim1ByFreq[freqIdx] = selectedIndex;
            ctx.state.m_prevPeakBoundaryDim1ByFreq[freqIdx] = selectedBoundary;
        } else {
            hpp2.selectedPeakDim2 = selectedIndex;
            ctx.state.m_prevPeakDim2ByFreq[freqIdx] = selectedIndex;
            ctx.state.m_prevPeakBoundaryDim2ByFreq[freqIdx] = selectedBoundary;
        }
        return selected;
    }

    static const PeakUnit* SelectHighestRankPeak(const PeakTable& table, int count) {
        const PeakUnit* best = nullptr;
        for (int i = 0; i < count; ++i) {
            const auto& unit = table[static_cast<std::size_t>(i)];
            if (!unit.valid) {
                continue;
            }
            if (best == nullptr || unit.rankScore > best->rankScore ||
                (unit.rankScore == best->rankScore && unit.peakSignal > best->peakSignal) ||
                (unit.rankScore == best->rankScore && unit.peakSignal == best->peakSignal && unit.netSignal > best->netSignal)) {
                best = &unit;
            }
        }
        return best;
    }

    static bool IsSelectedPeakAbnormal(const PeakUnit& unit) {
        // TSACore GetRealPeak checks selected-unit abnormal flag bytes after
        // UpdatePeaksWithUnit.  HPP2 line mode exposes the comparable state as
        // noiseProp bits from UpdatePeakNoiseFlags.
        return unit.noiseProp != 0;
    }

    static void PublishSelectedPeaks(HeatmapFrame& frame, const PeakUnit* dim1, const PeakUnit* dim2) {
        auto& runtime = frame.stylus.runtime.hpp2;
        runtime.signal.signalX = dim1 != nullptr ? dim1->peakSignal : 0;
        runtime.signal.signalY = dim2 != nullptr ? dim2->peakSignal : 0;
        runtime.signal.maxRawPeak = std::max(runtime.signal.signalX, runtime.signal.signalY);
        runtime.signal.recheckPassed = dim1 != nullptr && dim2 != nullptr;
        runtime.signal.dim1EdgeActive = dim1 != nullptr && dim1->onEdge;
        runtime.signal.dim2EdgeActive = dim2 != nullptr && dim2->onEdge;
        runtime.signal.dim1EdgeSignal = runtime.signal.dim1EdgeActive ? dim1->netSignal : 0;
        runtime.signal.dim2EdgeSignal = runtime.signal.dim2EdgeActive ? dim2->netSignal : 0;
    }
};

} // namespace Solvers::Stylus::Hpp2
