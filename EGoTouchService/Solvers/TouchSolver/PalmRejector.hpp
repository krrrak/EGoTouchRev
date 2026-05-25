#pragma once
// ── TouchPipeline Module: PalmRejector ──
// Header-only. Analyzes palm/fist MacroZones before peak detection.

#include "SolverTypes.h"
#include "PalmTypes.hpp"
#include <vector>
#include <algorithm>

namespace Solvers { namespace Touch {

class PalmRejector {
public:
    bool  m_enabled = true;
    int   m_areaThreshold = 50;
    int   m_signalSumThreshold = 80000;
    float m_densityThresholdLow = 400.0f;
    int   m_areaMinForDensity  = 20;
    bool  m_elongatedEnabled    = true;
    int   m_elongatedMinArea    = 10;
    float m_elongatedAspectRatio = 4.0f;
    int   m_lastRejectedCount = 0;

    bool  m_analyzerEnabled = true;
    int   m_candidateAreaThreshold = 35;
    int   m_candidateSignalThreshold = 80000;
    int   m_likelyAreaThreshold = 55;
    float m_fillRatioThreshold = 0.40f;
    float m_flatSharpnessThreshold = 2.20f;
    int   m_strongPeakProminence = 2000;

    inline int Process(std::vector<MacroZone>& macroZones,
                       const HeatmapFrame& frame) {
        Analyze(frame, macroZones);
        m_lastRejectedCount = 0;
        return 0;
    }

    inline void Analyze(const HeatmapFrame& frame,
                        const std::vector<MacroZone>& macroZones) {
        m_zoneFeatures.clear();
        if (!m_enabled || !m_analyzerEnabled) return;

        m_zoneFeatures.reserve(macroZones.size());
        for (int zi = 0; zi < static_cast<int>(macroZones.size()); ++zi) {
            m_zoneFeatures.push_back(BuildFeature(frame, macroZones[static_cast<size_t>(zi)], zi));
        }
    }

    const std::vector<MacroZoneFeature>& GetZoneFeatures() const { return m_zoneFeatures; }

private:
    static constexpr int kRows = 40;
    static constexpr int kCols = 60;

    std::vector<MacroZoneFeature> m_zoneFeatures;

    inline MacroZoneFeature BuildFeature(const HeatmapFrame& frame,
                                         const MacroZone& zone,
                                         int zoneIndex) const {
        MacroZoneFeature feature;
        feature.zoneIndex = zoneIndex;
        feature.area = zone.area;
        feature.signalSum = zone.signalSum;
        feature.bboxW = std::max(0, zone.maxC - zone.minC + 1);
        feature.bboxH = std::max(0, zone.maxR - zone.minR + 1);
        feature.bboxArea = feature.bboxW * feature.bboxH;
        feature.fillRatio = feature.bboxArea > 0
            ? static_cast<float>(zone.area) / static_cast<float>(feature.bboxArea)
            : 0.0f;
        feature.meanSignal = zone.area > 0
            ? static_cast<float>(zone.signalSum) / static_cast<float>(zone.area)
            : 0.0f;
        feature.density = feature.meanSignal;

        const int longSide = std::max(feature.bboxW, feature.bboxH);
        const int shortSide = std::min(feature.bboxW, feature.bboxH);
        feature.aspectRatio = shortSide > 0
            ? static_cast<float>(longSide) / static_cast<float>(shortSide)
            : 1.0f;

        double varianceSum = 0.0;
        for (int idx : zone.pixels) {
            const int r = idx / kCols;
            const int c = idx % kCols;
            const int sig = frame.heatmapMatrix[r][c];
            feature.maxSignal = std::max(feature.maxSignal, sig);
            const double delta = static_cast<double>(sig) - feature.meanSignal;
            varianceSum += delta * delta;
            if (r == 0) feature.edgeTouchMask |= 0x01;
            if (r == kRows - 1) feature.edgeTouchMask |= 0x02;
            if (c == 0) feature.edgeTouchMask |= 0x04;
            if (c == kCols - 1) feature.edgeTouchMask |= 0x08;
        }
        feature.signalVariance = zone.area > 0
            ? static_cast<float>(varianceSum / static_cast<double>(zone.area))
            : 0.0f;
        const float macroProminence = static_cast<float>(feature.maxSignal) - feature.meanSignal;
        const float macroSharpness = static_cast<float>(feature.maxSignal) / std::max(1.0f, feature.meanSignal);
        if (macroSharpness <= m_flatSharpnessThreshold) feature.reasonFlags |= PalmReasonFlatSignalShape;
        if (macroProminence >= static_cast<float>(m_strongPeakProminence)) feature.reasonFlags |= PalmReasonStrongSharpPeakPresent;

        if (zone.area >= m_candidateAreaThreshold) feature.reasonFlags |= PalmReasonLargeArea;
        if (zone.signalSum >= m_candidateSignalThreshold) feature.reasonFlags |= PalmReasonLargeSignalSum;
        if (zone.area >= m_areaMinForDensity && feature.density < m_densityThresholdLow) feature.reasonFlags |= PalmReasonLowDensity;
        if (m_elongatedEnabled && zone.area >= m_elongatedMinArea && feature.aspectRatio >= m_elongatedAspectRatio) feature.reasonFlags |= PalmReasonElongated;
        if (feature.fillRatio >= m_fillRatioThreshold && zone.area >= m_candidateAreaThreshold) feature.reasonFlags |= PalmReasonHighFillRatio;
        if (feature.edgeTouchMask != 0 && longSide >= 5) feature.reasonFlags |= PalmReasonEdgeWideContact;

        float palmScore = 0.0f;
        if (zone.area >= m_areaThreshold) palmScore += 0.35f;
        else if (zone.area >= m_candidateAreaThreshold) palmScore += 0.20f;
        if (zone.signalSum >= m_signalSumThreshold) palmScore += 0.25f;
        if (feature.reasonFlags & PalmReasonLowDensity) palmScore += 0.15f;
        if (feature.reasonFlags & PalmReasonElongated) palmScore += 0.15f;
        if (feature.reasonFlags & PalmReasonHighFillRatio) palmScore += 0.15f;
        if (feature.reasonFlags & PalmReasonEdgeWideContact) palmScore += 0.10f;
        if (feature.reasonFlags & PalmReasonFlatSignalShape) palmScore += 0.10f;
        if (feature.reasonFlags & PalmReasonStrongSharpPeakPresent) palmScore -= 0.20f;
        feature.palmScore = std::clamp(palmScore, 0.0f, 1.0f);

        float fingerScore = 0.0f;
        if (zone.area < m_candidateAreaThreshold) fingerScore += 0.45f;
        if (longSide <= 5) fingerScore += 0.25f;
        if (feature.aspectRatio < 2.5f) fingerScore += 0.15f;
        if (feature.density >= m_densityThresholdLow) fingerScore += 0.15f;
        if (feature.reasonFlags & PalmReasonStrongSharpPeakPresent) fingerScore += 0.20f;
        feature.fingerScore = std::clamp(fingerScore, 0.0f, 1.0f);

        if (zone.area >= m_likelyAreaThreshold && feature.palmScore >= 0.55f) {
            feature.palmClass = PalmClass::PalmLikely;
        } else if (feature.palmScore >= 0.35f) {
            feature.palmClass = PalmClass::PalmCandidate;
        } else if (feature.fingerScore >= 0.55f) {
            feature.palmClass = PalmClass::FingerLikely;
        } else {
            feature.palmClass = PalmClass::Ambiguous;
        }

        return feature;
    }
};

}} // namespace Solvers::Touch
