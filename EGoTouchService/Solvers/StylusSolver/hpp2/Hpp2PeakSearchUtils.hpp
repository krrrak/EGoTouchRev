#pragma once

#include "Hpp2CoordinateSolver.hpp"
#include "Hpp2Runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace Solvers::Stylus::Hpp2 {

struct Hpp2PeakSearchUtils {
    static bool IsLocalPeak(const Settings& settings,
                            const std::array<uint16_t, kMaxSamples>& line,
                            int offset,
                            int length,
                            int index) {
        const uint16_t current = line[static_cast<std::size_t>(offset + index)];
        if (current <= settings.peakSignalFloor) {
            return false;
        }
        const int neighborDist = std::max(1, settings.peakSearchNeighborDist);
        for (int delta = 1; delta <= neighborDist; ++delta) {
            if (index >= delta &&
                line[static_cast<std::size_t>(offset + index - delta)] > current) {
                return false;
            }
            if (index + delta < length &&
                line[static_cast<std::size_t>(offset + index + delta)] >= current) {
                return false;
            }
        }
        return true;
    }

    static void SearchPeakBoundary(const std::array<uint16_t, kMaxSamples>& line,
                                   int offset,
                                   int length,
                                   int peakIndex,
                                   PeakUnit& unit) {
        static constexpr uint32_t kBoundarySlopeQ5 = 0x23; // TSACore SearchPeakBoundary for group 0/1.
        static constexpr int kContributionPermilleFloor = 50;
        static constexpr uint32_t kAccumSignalFloor = 200;

        int left = peakIndex;
        if (left != 0) {
            left = peakIndex - 1;
            int contributionPermille = 1000;
            uint32_t accumulated = line[static_cast<std::size_t>(offset + peakIndex)];
            while (left != 0 && contributionPermille > kContributionPermilleFloor) {
                const uint32_t prev = line[static_cast<std::size_t>(offset + left - 1)];
                const uint32_t current = line[static_cast<std::size_t>(offset + left)];
                if (prev >= ((kBoundarySlopeQ5 * current) >> 5)) {
                    break;
                }
                accumulated += current;
                accumulated = std::max(accumulated, kAccumSignalFloor);
                contributionPermille = static_cast<int>((current * 1000u) / accumulated);
                if (contributionPermille > kContributionPermilleFloor) {
                    --left;
                }
            }
        }

        int right = peakIndex;
        if (right < length - 1) {
            right = peakIndex + 1;
            int contributionPermille = 1000;
            uint32_t accumulated = line[static_cast<std::size_t>(offset + peakIndex)];
            while (right < length - 1 && contributionPermille > kContributionPermilleFloor) {
                const uint32_t next = line[static_cast<std::size_t>(offset + right + 1)];
                const uint32_t current = line[static_cast<std::size_t>(offset + right)];
                if (next >= ((kBoundarySlopeQ5 * current) >> 5)) {
                    break;
                }
                accumulated += current;
                accumulated = std::max(accumulated, kAccumSignalFloor);
                contributionPermille = static_cast<int>((current * 1000u) / accumulated);
                if (contributionPermille > kContributionPermilleFloor) {
                    ++right;
                }
            }
        }

        unit.leftBoundary = left;
        unit.rightBoundary = right;
        unit.width = right - left + 1;
    }

    static void UpdatePeakPrpt(const std::array<uint16_t, kMaxSamples>& searchProfile,
                               const std::array<uint16_t, kMaxSamples>& signalProfile,
                               const std::array<uint16_t, kMaxSamples>* averageProfile,
                               int offset,
                               int length,
                               PeakUnit& unit) {
        uint32_t searchRegionSum = 0;
        uint32_t signalRegionSum = 0;
        uint32_t averageProfileSum = 0;
        uint16_t baselineMin = 0xffff;
        uint8_t regionWidth = 0;
        for (int i = unit.leftBoundary; i <= unit.rightBoundary; ++i) {
            const std::size_t sampleIndex = static_cast<std::size_t>(offset + i);
            const uint16_t searchSample = searchProfile[sampleIndex];
            ++regionWidth;
            searchRegionSum += searchSample;
            signalRegionSum += signalProfile[sampleIndex];
            baselineMin = std::min(baselineMin, searchSample);
            if (averageProfile != nullptr) {
                averageProfileSum += (*averageProfile)[sampleIndex];
            }
        }

        uint32_t threeNeighborSum = 0;
        const int neighborStart = unit.index == 0 ? 0 : std::max(0, unit.index - 1);
        const int neighborEnd = unit.index == length - 1 ? length - 1 : std::min(length - 1, unit.index + 1);
        for (int i = neighborStart; i <= neighborEnd; ++i) {
            threeNeighborSum += searchProfile[static_cast<std::size_t>(offset + i)];
        }

        const uint16_t peakSample = searchProfile[static_cast<std::size_t>(offset + unit.index)];
        uint16_t peakSignal = peakSample > baselineMin ? static_cast<uint16_t>(peakSample - baselineMin) : 0;
        if (peakSignal == 0) {
            peakSignal = 1;
        }
        const uint32_t net = searchRegionSum - static_cast<uint32_t>(unit.width) * baselineMin;
        const uint32_t averageMean = regionWidth == 0 ? 0 : averageProfileSum / regionWidth;
        unit.signalRegionSum = signalRegionSum;
        unit.threeNeighborSum = static_cast<int>(std::min<uint32_t>(threeNeighborSum, 0xffffu));
        unit.avgBaseline = static_cast<uint16_t>(std::min<uint32_t>(averageMean, 0xffffu));
        unit.peakSignal = peakSignal;
        unit.netSignal = static_cast<uint16_t>(std::min<uint32_t>(net, 0xffffu));
    }

    static int GetPeakPos(int groupId,
                          const std::array<uint16_t, kMaxSamples>& line,
                          int offset,
                          int length,
                          const PeakUnit& unit) {
        const int edgeThresholdLast = groupId == 0 ? 5000 : 4500;
        const int edgeThresholdFirst = groupId == 0 ? 5000 : 3700;
        // TSACore GetPeakPos (0x6baad7cb) takes the gravity-data path:
        // GetFictiousEdge(peakGroupId, peakIndex) -> UpdateTX1GravityData or
        // UpdateTX2GravityData -> Gravity -> coarseIndex * 0x400 + gravityOffset.
        // This rebuild intentionally keeps the triangle coordinate path because,
        // in HPP2 line mode, SolveByTriangle produces the same coarse + sub-pitch
        // position class from peakPos and neighbor samples without materializing
        // TSACore's transient gravity buffers.
        return Hpp2CoordinateMath::SolveByTriangle(line, offset, length, unit.index, 50, edgeThresholdLast, edgeThresholdFirst);
    }

    static void UpdatePeakNoiseFlags(const Settings& settings, PeakUnit& unit) {
        // HPP2 group 0/1 does not feed non-zero historyAvgSignal or last-output
        // coordinates into TSACore UpdatePeakNoisePrpt, but UpdatePeakPrpt's local
        // average/signal gates are still consumed by UpdatePeaksRank.
        static constexpr int kAvgHighFlag = 0x01;       // TSACore candidate +0x18.
        static constexpr int kSignalRegionFlag = 0x02;  // TSACore candidate +0x19.
        static constexpr int kAvgMediumFlag = 0x04;     // TSACore candidate +0x1a.
        static constexpr int kCompositeFlag = 0x08;     // TSACore candidate +0x02.
        unit.noiseProp = 0;
        if (unit.avgBaseline > 300) {
            unit.noiseProp |= kAvgHighFlag;
        }
        if (unit.avgBaseline > 200) {
            unit.noiseProp |= kAvgMediumFlag;
        }
        if (unit.signalRegionSum > 1000 && unit.netSignal < unit.signalRegionSum) {
            unit.noiseProp |= kSignalRegionFlag;
        }
        if ((unit.noiseProp & (kAvgHighFlag | kSignalRegionFlag)) != 0 ||
            (unit.peakSignal > 1000 && static_cast<uint32_t>(unit.peakSignal) * 3u < unit.netSignal)) {
            unit.noiseProp |= kCompositeFlag;
        }
        if (unit.width < settings.peakMinWidth || unit.width > settings.peakMaxWidth) {
            unit.noiseProp |= kCompositeFlag;
        }
    }

    static Peak FindPeak(const Settings& settings,
                         const std::array<uint16_t, kMaxSamples>& line,
                         int offset,
                         int length) {
        Peak peak{};
        for (int i = 0; i < length; ++i) {
            if (!IsLocalPeak(settings, line, offset, length, i)) {
                continue;
            }
            PeakUnit unit{};
            unit.valid = true;
            unit.index = i;
            SearchPeakBoundary(line, offset, length, i, unit);
            UpdatePeakPrpt(line, line, nullptr, offset, length, unit);
            if (unit.netSignal < settings.peakNetSignalFloor || unit.width < settings.peakMinWidth || unit.width > settings.peakMaxWidth) {
                continue;
            }
            if (!peak.valid || unit.peakSignal > peak.signal) {
                peak.index = unit.index;
                peak.signal = unit.peakSignal;
                peak.valid = true;
            }
        }
        return peak;
    }
};

} // namespace Solvers::Stylus::Hpp2
