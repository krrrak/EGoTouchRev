#pragma once

#include "Hpp2Runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace Solvers::Stylus::Hpp2 {

class Hpp2CmfProcess {
public:
    void Process(Context& ctx) const {
        ctx.state.m_cmnSum.fill(0);
        ctx.state.m_cmnMax.fill(0);
        ctx.state.m_cmnMin.fill(0xffff);
        ProcessGroup(ctx, 0, 0, ctx.settings.sensorTxCount);
        ProcessGroup(ctx, 1, ctx.settings.sensorTxCount, ctx.settings.sensorRxCount);
    }

private:
    static void ProcessGroup(Context& ctx, int group, int offset, int length) {
        if (length <= 0) {
            return;
        }

        auto& hpp2 = ctx.runtime;
        std::array<uint16_t, kMaxSamples> slidingMin{};
        for (int i = 0; i < length; ++i) {
            uint16_t minValue = 0xffff;
            const int start = std::max(0, i - ctx.settings.cmfWindowRadius);
            const int end = std::min(length - 1, i + ctx.settings.cmfWindowRadius);
            for (int j = start; j <= end; ++j) {
                minValue = std::min(minValue, hpp2.line.raw[static_cast<std::size_t>(offset + j)]);
            }
            slidingMin[static_cast<std::size_t>(i)] = minValue;
        }

        uint32_t cmnSum = 0;
        uint16_t cmnMax = 0;
        uint16_t cmnMin = 0xffff;
        uint32_t cmnRangeSum = 0;
        const std::size_t groupIndex = static_cast<std::size_t>(group);
        const bool rangeEnabled = ctx.settings.cmnRangeSumEnabled[groupIndex] != 0;
        const int rangeStart = std::clamp(ctx.settings.cmnRangeStart[groupIndex], 0, length - 1);
        const int rangeEnd = std::clamp(ctx.settings.cmnRangeEnd[groupIndex], 0, length - 1);
        const bool validRange = rangeEnabled && rangeStart <= rangeEnd;

        for (int i = 0; i < length; ++i) {
            uint16_t maxValue = 0;
            const int start = std::max(0, i - ctx.settings.cmfWindowRadius);
            const int end = std::min(length - 1, i + ctx.settings.cmfWindowRadius);
            for (int j = start; j <= end; ++j) {
                maxValue = std::max(maxValue, slidingMin[static_cast<std::size_t>(j)]);
            }
            const int idx = offset + i;
            const uint16_t raw = hpp2.line.raw[static_cast<std::size_t>(idx)];
            const uint16_t subtracted = raw > maxValue ? static_cast<uint16_t>(raw - maxValue) : 0;
            hpp2.line.cmnBaseline[static_cast<std::size_t>(idx)] = maxValue;
            hpp2.line.cmnSubtracted[static_cast<std::size_t>(idx)] = subtracted;
            cmnSum += maxValue;
            cmnMax = std::max(cmnMax, maxValue);
            cmnMin = std::min(cmnMin, maxValue);
            if (validRange && i >= rangeStart && i <= rangeEnd) {
                cmnRangeSum += subtracted;
            }
        }
        ctx.state.m_cmnSum[groupIndex] = cmnSum;
        ctx.state.m_cmnMax[groupIndex] = cmnMax;
        ctx.state.m_cmnMin[groupIndex] = cmnMin;
        RotateCmnHistory(ctx.state, groupIndex, cmnSum, cmnMax, cmnMin, cmnRangeSum);
    }

    static void RotateCmnHistory(State& state,
                                 std::size_t group,
                                 uint32_t cmnSum,
                                 uint16_t cmnMax,
                                 uint16_t cmnMin,
                                 uint32_t cmnRangeSum) {
        for (int i = kCmnHistorySize - 1; i > 0; --i) {
            const auto dst = static_cast<std::size_t>(i);
            const auto src = static_cast<std::size_t>(i - 1);
            state.m_cmnSumHistory[group][dst] = state.m_cmnSumHistory[group][src];
            state.m_cmnMaxHistory[group][dst] = state.m_cmnMaxHistory[group][src];
            state.m_cmnMinHistory[group][dst] = state.m_cmnMinHistory[group][src];
            state.m_cmnRangeSumHistory[group][dst] = state.m_cmnRangeSumHistory[group][src];
        }
        state.m_cmnSumHistory[group][0] = cmnSum;
        state.m_cmnMaxHistory[group][0] = cmnMax;
        state.m_cmnMinHistory[group][0] = cmnMin;
        state.m_cmnRangeSumHistory[group][0] = cmnRangeSum;
    }
};

} // namespace Solvers::Stylus::Hpp2
