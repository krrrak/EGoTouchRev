#pragma once

#include "Hpp2Runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Solvers::Stylus::Hpp2 {

class Hpp2ChargerNoiseProcess {
public:
    void Process(Context& ctx) const {
        auto& hpp2 = ctx.runtime;
        auto& state = ctx.state;
        const std::size_t freqIdx = static_cast<std::size_t>(state.m_curFreqIdx);
        const int frameCount = CurrentFreqFrameCount(state);
        uint16_t maxNoiseSample = 0;
        uint8_t abnormalChannelCount = 0;
        hpp2.line.chargerNoiseRatio.fill(0);

        if (frameCount > 1) {
            const int clearFrame = GetClearFrameIndex(ctx, frameCount);
            const int count = ctx.settings.SampleCount();
            for (int i = 0; i < count; ++i) {
                const uint16_t currentSample = hpp2.line.raw[static_cast<std::size_t>(i)];
                if (IndexValidation(ctx, i, currentSample)) {
                    continue;
                }

                const uint16_t clearSample = state.m_rawHistory[freqIdx][static_cast<std::size_t>(clearFrame)][static_cast<std::size_t>(i)];
                const uint16_t floor = std::max<uint16_t>(ctx.settings.chargerNoiseClearFloor, 1);
                const uint16_t denom = std::max<uint16_t>(clearSample, floor);
                const uint32_t ratio = (static_cast<uint32_t>(currentSample) * 100u) / denom;
                hpp2.line.chargerNoiseRatio[static_cast<std::size_t>(i)] =
                    static_cast<uint16_t>(std::min<uint32_t>(ratio, 0xffffu));

                if (ratio > ctx.settings.chargerNoiseRatioThreshold) {
                    ++abnormalChannelCount;
                    maxNoiseSample = std::max(maxNoiseSample, currentSample);
                    state.m_noiseSum[freqIdx] += currentSample;
                }
            }
        }

        if (state.m_noiseSum[freqIdx] > ctx.settings.chargerNoiseSumThreshold &&
            maxNoiseSample > ctx.settings.chargerNoiseMaxSampleThreshold &&
            abnormalChannelCount > ctx.settings.chargerNoiseAbnormalChannelThreshold) {
            state.m_noiseFlag[freqIdx] = 1;
        }
    }

    void RotateRawHistory(Context& ctx) const {
        auto& hpp2 = ctx.runtime;
        auto& state = ctx.state;
        const std::size_t freqIdx = static_cast<std::size_t>(state.m_curFreqIdx);
        for (int i = kNumRawHistoryFrames - 1; i > 0; --i) {
            state.m_rawHistory[freqIdx][static_cast<std::size_t>(i)] = state.m_rawHistory[freqIdx][static_cast<std::size_t>(i - 1)];
            state.m_noiseFlagHistory[freqIdx][static_cast<std::size_t>(i)] = state.m_noiseFlagHistory[freqIdx][static_cast<std::size_t>(i - 1)];
            state.m_noiseSumHistory[freqIdx][static_cast<std::size_t>(i)] = state.m_noiseSumHistory[freqIdx][static_cast<std::size_t>(i - 1)];
        }
        auto& head = state.m_rawHistory[freqIdx][0];
        head.fill(0);
        const int count = ctx.settings.SampleCount();
        for (int i = 0; i < count; ++i) {
            head[static_cast<std::size_t>(i)] = hpp2.line.raw[static_cast<std::size_t>(i)];
        }
        state.m_noiseFlagHistory[freqIdx][0] = state.m_noiseFlag[freqIdx];
        state.m_noiseSumHistory[freqIdx][0] = state.m_noiseSum[freqIdx];

        int& frameCount = CurrentFreqFrameCountRef(state);
        frameCount = std::min(frameCount + 1, kNumRawHistoryFrames);
    }

private:
    static int GetClearFrameIndex(const Context& ctx, int frameCount) {
        const auto& state = ctx.state;
        const std::size_t freqIdx = static_cast<std::size_t>(state.m_curFreqIdx);
        int fallbackNoiseIndex = -1;
        uint32_t minNoiseSum = 0xffffu;
        const int historyLimit = std::min(frameCount, kNumRawHistoryFrames);
        for (int i = 0; i < historyLimit; ++i) {
            const std::size_t idx = static_cast<std::size_t>(i);
            if (state.m_noiseFlagHistory[freqIdx][idx] == 0) {
                return i;
            }
            const uint32_t noiseSum = state.m_noiseSumHistory[freqIdx][idx];
            if (noiseSum < minNoiseSum) {
                minNoiseSum = noiseSum;
                fallbackNoiseIndex = i;
            }
        }
        if (fallbackNoiseIndex >= 0) {
            return fallbackNoiseIndex;
        }
        return 0;
    }

    static bool IndexValidation(const Context& ctx, int index, uint16_t rawSample) {
        if (rawSample < ctx.settings.chargerNoiseMinRawSample) {
            return true;
        }
        const std::size_t freqIdx = static_cast<std::size_t>(ctx.state.m_curFreqIdx);
        if (IsProtectedPeakBoundary(ctx, index, 0, ctx.state.m_prevPeakBoundaryDim1ByFreq[freqIdx])) {
            return true;
        }
        if (IsProtectedPeakBoundary(ctx, index, ctx.settings.sensorTxCount, ctx.state.m_prevPeakBoundaryDim2ByFreq[freqIdx])) {
            return true;
        }
        return false;
    }

    static bool IsProtectedPeakBoundary(const Context& ctx, int globalIndex, int offset, const PeakBoundary& boundary) {
        if (!boundary.valid || boundary.left < 0 || boundary.right < boundary.left) {
            return false;
        }
        const int localIndex = globalIndex - offset;
        const int length = offset == 0 ? ctx.settings.sensorTxCount : ctx.settings.sensorRxCount;
        if (localIndex < 0 || localIndex >= length) {
            return false;
        }
        const int radius = static_cast<int>(ctx.settings.chargerNoisePeakProtectRadius);
        const int protectedLeft = std::max(0, boundary.left - radius);
        const int protectedRight = std::min(length - 1, boundary.right + radius);
        return localIndex >= protectedLeft && localIndex <= protectedRight;
    }

    static int CurrentFreqFrameCount(const State& state) {
        return state.m_curFreqIdx == 0 ? state.m_f1FrameCnt : state.m_f2FrameCnt;
    }

    static int& CurrentFreqFrameCountRef(State& state) {
        return state.m_curFreqIdx == 0 ? state.m_f1FrameCnt : state.m_f2FrameCnt;
    }
};

} // namespace Solvers::Stylus::Hpp2
